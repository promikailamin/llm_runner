#pragma once

#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>

namespace agent {

struct ToolContext {
  std::string workdir;
  int default_shell_timeout = 120;
  int max_output_chars = 20000;
};

nlohmann::json tool_definitions();
std::string execute_tool(const std::string& name, const nlohmann::json& arguments,
                         const ToolContext& context);
std::string truncate_output(const std::string& value, std::size_t maximum);

struct ShellOutcome {
  int exit_code = -1;
  int signal_number = 0;
  bool timed_out = false;
  bool output_truncated = false;
  std::string stdout_text;
  std::string stderr_text;
  long duration_ms = 0;
};

ShellOutcome run_shell(const std::string& command, const std::string& workdir,
                       int timeout_seconds);

}  // namespace agent
