#pragma once

#include <map>
#include <string>

namespace agent {

struct HttpUrl {
  std::string scheme;
  std::string host;
  int port = 80;
  std::string base_path;

  static HttpUrl parse(const std::string& raw);
  std::string authority() const;
};

struct HttpResponse {
  int status = 0;
  std::map<std::string, std::string> headers;
  std::string body;

  bool ok() const { return status >= 200 && status < 300; }
};

class HttpClient {
 public:
  explicit HttpClient(HttpUrl url, int timeout_seconds = 300);

  void set_timeout(int timeout_seconds) { timeout_seconds_ = timeout_seconds; }
  const HttpUrl& url() const { return url_; }

  HttpResponse get(const std::string& path,
                   const std::map<std::string, std::string>& headers = {});
  HttpResponse post(const std::string& path, const std::string& body,
                    const std::string& content_type = "application/json");

 private:
  HttpUrl url_;
  int timeout_seconds_;

  HttpResponse request(const std::string& method, const std::string& path,
                       const std::string& body,
                       const std::map<std::string, std::string>& headers);
  std::string join_path(const std::string& path) const;
};

}  // namespace agent
