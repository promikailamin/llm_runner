#include "http.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace agent {
namespace {

using Clock = std::chrono::steady_clock;

std::string trim_copy(const std::string& value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return "";
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

std::string lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

int remaining_ms(Clock::time_point deadline) {
  const auto now = Clock::now();
  if (now >= deadline) return 0;
  return static_cast<int>(
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
}

void set_nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int connect_socket(const std::string& host, int port, int timeout_ms) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  addrinfo* result = nullptr;
  const std::string service = std::to_string(port);
  const int gai = getaddrinfo(host.c_str(), service.c_str(), &hints, &result);
  if (gai != 0) {
    throw std::runtime_error("could not resolve " + host + ": " + gai_strerror(gai));
  }

  int connected = -1;
  std::string last_error = "no address";
  for (addrinfo* entry = result; entry != nullptr; entry = entry->ai_next) {
    const int fd = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (fd < 0) {
      last_error = std::strerror(errno);
      continue;
    }
    set_nonblocking(fd);
    const int rc = connect(fd, entry->ai_addr, entry->ai_addrlen);
    if (rc == 0) {
      connected = fd;
      break;
    }
    if (errno != EINPROGRESS) {
      last_error = std::strerror(errno);
      close(fd);
      continue;
    }
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    const int ready = poll(&pfd, 1, timeout_ms);
    if (ready <= 0) {
      last_error = ready == 0 ? "connect timed out" : std::strerror(errno);
      close(fd);
      continue;
    }
    int error = 0;
    socklen_t length = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0) {
      connected = fd;
      break;
    }
    last_error = std::strerror(error);
    close(fd);
  }
  freeaddrinfo(result);

  if (connected < 0) {
    throw std::runtime_error("could not connect to " + host + ":" + service + ": " +
                             last_error);
  }
  return connected;
}

bool send_all(int fd, const std::string& data, Clock::time_point deadline) {
  size_t offset = 0;
  while (offset < data.size()) {
    const ssize_t sent =
        send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
    if (sent > 0) {
      offset += static_cast<size_t>(sent);
      continue;
    }
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      const int timeout = remaining_ms(deadline);
      if (timeout <= 0) return false;
      pollfd pfd{};
      pfd.fd = fd;
      pfd.events = POLLOUT;
      poll(&pfd, 1, timeout);
      continue;
    }
    if (sent < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

std::string recv_all(int fd, Clock::time_point deadline, size_t maximum) {
  std::string data;
  char buffer[16384];
  while (true) {
    const ssize_t received = recv(fd, buffer, sizeof(buffer), 0);
    if (received > 0) {
      data.append(buffer, static_cast<size_t>(received));
      if (data.size() > maximum) {
        throw std::runtime_error("HTTP response exceeded size limit");
      }
      continue;
    }
    if (received == 0) break;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      const int timeout = remaining_ms(deadline);
      if (timeout <= 0) throw std::runtime_error("HTTP read timed out");
      pollfd pfd{};
      pfd.fd = fd;
      pfd.events = POLLIN;
      const int ready = poll(&pfd, 1, timeout);
      if (ready < 0 && errno == EINTR) continue;
      if (ready <= 0) throw std::runtime_error("HTTP read timed out");
      continue;
    }
    if (errno == EINTR) continue;
    throw std::runtime_error(std::string("HTTP read failed: ") + std::strerror(errno));
  }
  return data;
}

std::string decode_chunked(const std::string& body) {
  std::string decoded;
  size_t position = 0;
  while (position < body.size()) {
    const auto line_end = body.find("\r\n", position);
    if (line_end == std::string::npos) throw std::runtime_error("invalid chunked body");
    const std::string size_text = body.substr(position, line_end - position);
    const size_t extension = size_text.find(';');
    const std::string hex =
        trim_copy(extension == std::string::npos ? size_text : size_text.substr(0, extension));
    if (hex.empty()) throw std::runtime_error("invalid chunk size");
    const size_t chunk_size = static_cast<size_t>(std::stoull(hex, nullptr, 16));
    position = line_end + 2;
    if (chunk_size == 0) break;
    if (position + chunk_size > body.size()) throw std::runtime_error("truncated chunked body");
    decoded.append(body, position, chunk_size);
    position += chunk_size;
    if (body.compare(position, 2, "\r\n") != 0) {
      throw std::runtime_error("invalid chunk terminator");
    }
    position += 2;
  }
  return decoded;
}

}  // namespace

HttpUrl HttpUrl::parse(const std::string& raw) {
  std::string value = trim_copy(raw);
  if (value.empty()) throw std::runtime_error("empty URL");
  if (value.find("://") == std::string::npos) value = "http://" + value;

  const auto scheme_end = value.find("://");
  HttpUrl url;
  url.scheme = lower_copy(value.substr(0, scheme_end));
  if (url.scheme != "http") {
    throw std::runtime_error("only http:// URLs are supported, got " + url.scheme +
                             "://");
  }

  std::string rest = value.substr(scheme_end + 3);
  const auto slash = rest.find('/');
  std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
  url.base_path = slash == std::string::npos ? "" : rest.substr(slash);
  if (!url.base_path.empty() && url.base_path.back() == '/') {
    url.base_path.pop_back();
  }
  if (authority.empty()) throw std::runtime_error("URL is missing a host");

  std::string port_text;
  if (authority.front() == '[') {
    const auto close = authority.find(']');
    if (close == std::string::npos) throw std::runtime_error("invalid IPv6 literal");
    url.host = authority.substr(1, close - 1);
    if (close + 1 < authority.size()) {
      if (authority[close + 1] != ':') throw std::runtime_error("invalid authority");
      port_text = authority.substr(close + 2);
    }
  } else {
    const auto colon = authority.rfind(':');
    if (colon == std::string::npos) {
      url.host = authority;
    } else {
      url.host = authority.substr(0, colon);
      port_text = authority.substr(colon + 1);
    }
  }

  if (url.host.empty()) throw std::runtime_error("URL is missing a host");
  if (!port_text.empty()) {
    try {
      url.port = std::stoi(port_text);
    } catch (...) {
      throw std::runtime_error("invalid port: " + port_text);
    }
    if (url.port < 1 || url.port > 65535) throw std::runtime_error("invalid port: " + port_text);
  }
  return url;
}

std::string HttpUrl::authority() const {
  std::string value = host.find(':') != std::string::npos ? "[" + host + "]" : host;
  if (port != 80) value += ":" + std::to_string(port);
  return value;
}

HttpClient::HttpClient(HttpUrl url, int timeout_seconds)
    : url_(std::move(url)), timeout_seconds_(timeout_seconds) {}

std::string HttpClient::join_path(const std::string& path) const {
  if (path.empty()) return url_.base_path.empty() ? "/" : url_.base_path;
  if (path.front() == '/') return url_.base_path + path;
  return url_.base_path + "/" + path;
}

HttpResponse HttpClient::get(const std::string& path,
                             const std::map<std::string, std::string>& headers) {
  return request("GET", path, "", headers);
}

HttpResponse HttpClient::post(const std::string& path, const std::string& body,
                              const std::string& content_type) {
  return request("POST", path, body, {{"Content-Type", content_type}});
}

HttpResponse HttpClient::request(const std::string& method, const std::string& path,
                                 const std::string& body,
                                 const std::map<std::string, std::string>& headers) {
  const auto deadline = Clock::now() + std::chrono::seconds(timeout_seconds_);
  const int fd = connect_socket(url_.host, url_.port, remaining_ms(deadline));

  std::ostringstream request;
  request << method << " " << join_path(path) << " HTTP/1.1\r\n";
  request << "Host: " << url_.authority() << "\r\n";
  request << "User-Agent: llm-agent/1.0\r\n";
  request << "Accept: application/json\r\n";
  request << "Connection: close\r\n";
  for (const auto& [key, value] : headers) {
    request << key << ": " << value << "\r\n";
  }
  if (!body.empty() || method == "POST") {
    request << "Content-Length: " << body.size() << "\r\n";
  }
  request << "\r\n";
  request << body;

  const std::string wire = request.str();
  if (!send_all(fd, wire, deadline)) {
    close(fd);
    throw std::runtime_error("HTTP send failed");
  }

  std::string raw;
  try {
    raw = recv_all(fd, deadline, 64U * 1024U * 1024U);
  } catch (...) {
    close(fd);
    throw;
  }
  close(fd);

  const auto header_end = raw.find("\r\n\r\n");
  if (header_end == std::string::npos) throw std::runtime_error("invalid HTTP response");

  const std::string head = raw.substr(0, header_end);
  std::string payload = raw.substr(header_end + 4);

  HttpResponse response;
  std::istringstream stream(head);
  std::string line;
  if (!std::getline(stream, line)) throw std::runtime_error("empty HTTP response");
  if (!line.empty() && line.back() == '\r') line.pop_back();
  {
    std::istringstream status(line);
    std::string version;
    status >> version >> response.status;
    if (response.status <= 0) throw std::runtime_error("invalid HTTP status line");
  }
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    response.headers[lower_copy(trim_copy(line.substr(0, colon)))] =
        trim_copy(line.substr(colon + 1));
  }

  const auto encoding = response.headers.find("transfer-encoding");
  if (encoding != response.headers.end() &&
      lower_copy(encoding->second).find("chunked") != std::string::npos) {
    payload = decode_chunked(payload);
  }
  response.body = std::move(payload);
  return response;
}

}  // namespace agent
