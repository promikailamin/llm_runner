#include "tools.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

namespace agent {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaxCapturedBytes = 4U * 1024U * 1024U;

fs::path resolve_path(const std::string& workdir, const std::string& value) {
  fs::path path(value);
  if (path.is_relative()) path = fs::path(workdir) / path;
  return path.lexically_normal();
}

std::string read_stream(std::ifstream& stream) {
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

nlohmann::json shell_json(const ShellOutcome& outcome, std::size_t maximum) {
  nlohmann::json result;
  result["ok"] = !outcome.timed_out && outcome.exit_code == 0;
  result["exit_code"] = outcome.exit_code;
  result["signal"] = outcome.signal_number;
  result["timed_out"] = outcome.timed_out;
  result["output_truncated"] = outcome.output_truncated;
  result["duration_ms"] = outcome.duration_ms;
  result["stdout"] = truncate_output(outcome.stdout_text, maximum);
  result["stderr"] = truncate_output(outcome.stderr_text, maximum);
  return result;
}

void append_limited(std::string& target, const char* data, std::size_t length,
                    bool& truncated) {
  const std::size_t room = kMaxCapturedBytes > target.size()
                               ? kMaxCapturedBytes - target.size()
                               : 0;
  const std::size_t take = std::min(room, length);
  target.append(data, take);
  if (take < length) truncated = true;
}

void read_available(int fd, std::string& target, bool& truncated, bool& closed) {
  char buffer[16384];
  while (true) {
    const ssize_t count = read(fd, buffer, sizeof(buffer));
    if (count > 0) {
      append_limited(target, buffer, static_cast<std::size_t>(count), truncated);
      continue;
    }
    if (count == 0) {
      closed = true;
      return;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) return;
    if (errno == EINTR) continue;
    closed = true;
    return;
  }
}

}  // namespace

std::string truncate_output(const std::string& value, std::size_t maximum) {
  if (maximum == 0 || value.size() <= maximum) return value;
  const std::size_t keep = maximum > 200 ? maximum - 200 : maximum / 2;
  const std::size_t head = keep * 2 / 3;
  const std::size_t tail = keep - head;
  std::ostringstream out;
  out << value.substr(0, head);
  out << "\n... [truncated " << (value.size() - head - tail) << " bytes] ...\n";
  out << value.substr(value.size() - tail);
  return out.str();
}

ShellOutcome run_shell(const std::string& command, const std::string& workdir,
                       int timeout_seconds) {
  ShellOutcome outcome;
  if (timeout_seconds <= 0) timeout_seconds = 120;

  int out_pipe[2];
  int err_pipe[2];
  if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
    throw std::runtime_error("could not create pipes");
  }

  const auto started = Clock::now();
  const pid_t pid = fork();
  if (pid < 0) {
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);
    throw std::runtime_error("could not fork shell");
  }

  if (pid == 0) {
    setpgid(0, 0);
    dup2(out_pipe[1], STDOUT_FILENO);
    dup2(err_pipe[1], STDERR_FILENO);
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);
    if (!workdir.empty() && chdir(workdir.c_str()) != 0) {
      const std::string message = "could not chdir to " + workdir + "\n";
      ssize_t ignored = write(STDERR_FILENO, message.data(), message.size());
      (void)ignored;
      _exit(127);
    }
    execl("/bin/bash", "bash", "-lc", command.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }

  close(out_pipe[1]);
  close(err_pipe[1]);
  fcntl(out_pipe[0], F_SETFL, fcntl(out_pipe[0], F_GETFL, 0) | O_NONBLOCK);
  fcntl(err_pipe[0], F_SETFL, fcntl(err_pipe[0], F_GETFL, 0) | O_NONBLOCK);

  const auto deadline = started + std::chrono::seconds(timeout_seconds);
  bool out_closed = false;
  bool err_closed = false;
  bool truncated = false;
  bool child_done = false;
  bool term_sent = false;
  bool kill_sent = false;
  auto child_done_at = Clock::now();
  auto kill_deadline = deadline + std::chrono::seconds(3);
  int status = 0;

  while (true) {
    if (!child_done) {
      const pid_t waited = waitpid(pid, &status, WNOHANG);
      if (waited == pid) {
        child_done = true;
        child_done_at = Clock::now();
      }
    }

    if (out_closed && err_closed && child_done) break;

    const auto now = Clock::now();
    if (!child_done && !term_sent && now >= deadline) {
      outcome.timed_out = true;
      term_sent = true;
      kill(-pid, SIGTERM);
      kill(pid, SIGTERM);
      kill_deadline = now + std::chrono::seconds(3);
    }
    if (!child_done && term_sent && !kill_sent && now >= kill_deadline) {
      kill(-pid, SIGKILL);
      kill(pid, SIGKILL);
      kill_sent = true;
    }
    if (child_done && ((!out_closed || !err_closed)) &&
        now - child_done_at > std::chrono::seconds(2)) {
      break;
    }

    pollfd fds[2];
    nfds_t count = 0;
    if (!out_closed) {
      fds[count].fd = out_pipe[0];
      fds[count].events = POLLIN;
      fds[count].revents = 0;
      ++count;
    }
    if (!err_closed) {
      fds[count].fd = err_pipe[0];
      fds[count].events = POLLIN;
      fds[count].revents = 0;
      ++count;
    }
    if (count > 0) {
      poll(fds, count, 50);
      if (!out_closed) read_available(out_pipe[0], outcome.stdout_text, truncated, out_closed);
      if (!err_closed) read_available(err_pipe[0], outcome.stderr_text, truncated, err_closed);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }

  if (!child_done) {
    if (!term_sent) {
      kill(-pid, SIGTERM);
      kill(pid, SIGTERM);
    }
    waitpid(pid, &status, 0);
  }

  close(out_pipe[0]);
  close(err_pipe[0]);

  if (WIFEXITED(status)) {
    outcome.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    outcome.signal_number = WTERMSIG(status);
    outcome.exit_code = 128 + outcome.signal_number;
  }
  outcome.output_truncated = truncated;
  outcome.duration_ms = static_cast<long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started)
          .count());
  if (term_sent || kill_sent) outcome.timed_out = true;
  return outcome;
}

nlohmann::json tool_definitions() {
  nlohmann::json tools = nlohmann::json::array();

  tools.push_back({
      {"type", "function"},
      {"function",
       {{"name", "run_shell"},
        {"description",
         "Run a bash command in the repository workspace and return its exit code, "
         "stdout, and stderr. Use this to inspect files, run builds, and execute tests."},
        {"parameters",
         {{"type", "object"},
          {"properties",
           {{"command",
             {{"type", "string"},
              {"description", "The bash command to run. Combine steps with && when needed."}}},
            {"timeout_seconds",
             {{"type", "integer"},
              {"description", "Maximum seconds before the command is killed."}}},
            {"workdir",
             {{"type", "string"},
              {"description", "Optional working directory relative to the repository."}}}}},
          {"required", nlohmann::json::array({"command"})}}}}}});

  tools.push_back({
      {"type", "function"},
      {"function",
       {{"name", "read_file"},
        {"description", "Read a text file with optional line offset and limit."},
        {"parameters",
         {{"type", "object"},
          {"properties",
           {{"path", {{"type", "string"}, {"description", "File path."}}},
            {"offset", {{"type", "integer"}, {"description", "First line, starting at 1."}}},
            {"limit", {{"type", "integer"}, {"description", "Maximum lines to return."}}}}},
          {"required", nlohmann::json::array({"path"})}}}}}});

  tools.push_back({
      {"type", "function"},
      {"function",
       {{"name", "write_file"},
        {"description", "Create or overwrite a text file, creating parent directories."},
        {"parameters",
         {{"type", "object"},
          {"properties",
           {{"path", {{"type", "string"}, {"description", "File path."}}},
            {"content", {{"type", "string"}, {"description", "Complete new file content."}}}}},
          {"required", nlohmann::json::array({"path", "content"})}}}}}});

  tools.push_back({
      {"type", "function"},
      {"function",
       {{"name", "list_dir"},
        {"description", "List files and directories under a path."},
        {"parameters",
         {{"type", "object"},
          {"properties",
           {{"path", {{"type", "string"}, {"description", "Directory path."}}},
            {"recursive", {{"type", "boolean"}, {"description", "Recurse into subdirectories."}}}}},
          {"required", nlohmann::json::array({"path"})}}}}}});

  return tools;
}

std::string execute_tool(const std::string& name, const nlohmann::json& arguments,
                         const ToolContext& context) {
  try {
    if (!arguments.is_object()) {
      return "{\"ok\":false,\"error\":\"tool arguments must be a JSON object\"}";
    }

    if (name == "run_shell") {
      if (!arguments.contains("command") || !arguments["command"].is_string()) {
        return "{\"ok\":false,\"error\":\"run_shell requires a string command\"}";
      }
      const std::string command = arguments["command"].get<std::string>();
      int timeout = context.default_shell_timeout;
      if (arguments.contains("timeout_seconds") && arguments["timeout_seconds"].is_number()) {
        timeout = arguments["timeout_seconds"].get<int>();
      }
      std::string workdir = context.workdir;
      if (arguments.contains("workdir") && arguments["workdir"].is_string()) {
        workdir = resolve_path(context.workdir, arguments["workdir"].get<std::string>()).string();
      }
      const ShellOutcome outcome = run_shell(command, workdir, timeout);
      return shell_json(outcome, context.max_output_chars).dump();
    }

    if (name == "read_file") {
      if (!arguments.contains("path") || !arguments["path"].is_string()) {
        return "{\"ok\":false,\"error\":\"read_file requires a string path\"}";
      }
      const fs::path path = resolve_path(context.workdir, arguments["path"].get<std::string>());
      std::ifstream input(path, std::ios::binary);
      if (!input) {
        return nlohmann::json{{"ok", false}, {"error", "could not open " + path.string()}}
            .dump();
      }
      std::string content = read_stream(input);
      bool truncated = false;
      if (content.size() > kMaxCapturedBytes) {
        content = content.substr(0, kMaxCapturedBytes);
        truncated = true;
      }
      const int offset = arguments.value("offset", 1);
      const int limit = arguments.value("limit", 400);
      std::vector<std::string> lines;
      std::istringstream stream(content);
      std::string line;
      int number = 0;
      while (std::getline(stream, line)) {
        ++number;
        if (number < std::max(1, offset)) continue;
        if (limit > 0 && static_cast<int>(lines.size()) >= limit) {
          truncated = true;
          break;
        }
        lines.push_back(line);
      }
      std::ostringstream selected;
      for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index > 0) selected << "\n";
        selected << lines[index];
      }
      return nlohmann::json{{"ok", true},
                            {"path", path.string()},
                            {"offset", std::max(1, offset)},
                            {"lines", lines.size()},
                            {"truncated", truncated},
                            {"content", truncate_output(selected.str(), context.max_output_chars)}}
          .dump();
    }

    if (name == "write_file") {
      if (!arguments.contains("path") || !arguments["path"].is_string() ||
          !arguments.contains("content") || !arguments["content"].is_string()) {
        return "{\"ok\":false,\"error\":\"write_file requires path and content\"}";
      }
      const fs::path path = resolve_path(context.workdir, arguments["path"].get<std::string>());
      if (path.has_parent_path()) fs::create_directories(path.parent_path());
      std::ofstream output(path, std::ios::binary | std::ios::trunc);
      if (!output) {
        return nlohmann::json{{"ok", false}, {"error", "could not write " + path.string()}}
            .dump();
      }
      const std::string content = arguments["content"].get<std::string>();
      output << content;
      output.close();
      return nlohmann::json{{"ok", true}, {"path", path.string()}, {"bytes", content.size()}}
          .dump();
    }

    if (name == "list_dir") {
      if (!arguments.contains("path") || !arguments["path"].is_string()) {
        return "{\"ok\":false,\"error\":\"list_dir requires a string path\"}";
      }
      const fs::path root = resolve_path(context.workdir, arguments["path"].get<std::string>());
      if (!fs::exists(root) || !fs::is_directory(root)) {
        return nlohmann::json{{"ok", false}, {"error", "not a directory: " + root.string()}}
            .dump();
      }
      const bool recursive = arguments.value("recursive", false);
      nlohmann::json entries = nlohmann::json::array();
      constexpr std::size_t maximum_entries = 500;
      bool truncated = false;
      auto add_entry = [&](const fs::directory_entry& entry) {
        if (entries.size() >= maximum_entries) {
          truncated = true;
          return false;
        }
        std::error_code error;
        nlohmann::json item;
        item["path"] = entry.path().lexically_relative(root).generic_string();
        item["type"] = entry.is_directory(error) ? "dir" : "file";
        if (!error && entry.is_regular_file(error)) {
          item["size"] = static_cast<long long>(entry.file_size(error));
        }
        entries.push_back(item);
        return true;
      };
      if (recursive) {
        for (const auto& entry : fs::recursive_directory_iterator(
                 root, fs::directory_options::skip_permission_denied)) {
          if (!add_entry(entry)) break;
        }
      } else {
        for (const auto& entry : fs::directory_iterator(
                 root, fs::directory_options::skip_permission_denied)) {
          if (!add_entry(entry)) break;
        }
      }
      return nlohmann::json{{"ok", true}, {"path", root.string()}, {"entries", entries},
                            {"truncated", truncated}}
          .dump();
    }

    return nlohmann::json{{"ok", false}, {"error", "unknown tool: " + name}}.dump();
  } catch (const std::exception& error) {
    return nlohmann::json{{"ok", false}, {"error", error.what()}}.dump();
  }
}

}  // namespace agent
