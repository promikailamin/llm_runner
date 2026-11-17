#pragma once

#include <string>

namespace agent {

struct AgentConfig {
  std::string base_url;
  std::string model;
  std::string task;
  std::string system_prompt;
  std::string cwd;
  std::string log_file;
  std::string api = "ollama";
  int max_steps = 40;
  int timeout_seconds = 1800;
  int shell_timeout_seconds = 120;
  int max_output_chars = 20000;
  double temperature = 0.2;
  bool native_tools = true;
  bool quiet = false;
};

std::string default_system_prompt(const AgentConfig& config);
int run_agent(const AgentConfig& config);
bool run_self_test();

}  // namespace agent
