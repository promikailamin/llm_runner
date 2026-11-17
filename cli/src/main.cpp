#include "agent.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr const char* kVersion = "1.0.0";

void print_help() {
  std::cout
      << "llm-agent " << kVersion << "\n\n"
      << "Usage:\n"
      << "  llm-agent --tunnel tunnel.json --task \"Inspect and fix the bug\"\n"
      << "  llm-agent --url http://bore.pub:32271 --model qwen2.5-coder:3b --task \"...\"\n\n"
      << "Connection options:\n"
      << "  --tunnel PATH          Read address, port, and model from a tunnel.json file.\n"
      << "  --url URL              API base URL, for example http://bore.pub:32271.\n"
      << "  --model NAME           Ollama model name (overrides tunnel.json).\n"
      << "  --api NAME             ollama, openai, or auto (default: ollama).\n\n"
      << "Agent options:\n"
      << "  --task TEXT            Task for the agent. Positional text is also accepted.\n"
      << "  --task-file PATH       Read the task from a file.\n"
      << "  --system TEXT          Override the built-in system prompt.\n"
      << "  --system-prompt PATH   Read the system prompt from a file.\n"
      << "  --cwd PATH             Working directory (default: current directory).\n"
      << "  --max-steps N          Maximum agent iterations (default: 40).\n"
      << "  --timeout SECONDS      Overall agent time limit (default: 1800).\n"
      << "  --shell-timeout SEC    Default shell command timeout (default: 120).\n"
      << "  --max-output N         Maximum output characters per tool result (default: 20000).\n"
      << "  --temperature VALUE    Sampling temperature (default: 0.2).\n"
      << "  --log PATH             Append a detailed session log to PATH.\n"
      << "  --no-native-tools      Always use the text tool-call protocol.\n"
      << "  --quiet                Suppress progress output on stderr.\n\n"
      << "Other:\n"
      << "  --self-test            Run local checks without a network connection.\n"
      << "  --version              Print the version.\n"
      << "  --help                 Print this help.\n";
}

std::string read_file(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("could not read " + path);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

struct Arguments {
  agent::AgentConfig config;
  std::string tunnel_path;
  std::string task_file;
  std::string system_prompt_file;
  std::vector<std::string> positional;
  bool help = false;
  bool version = false;
  bool self_test = false;
};

std::string take_value(const std::string& argument, const std::string& name, int& index,
                       int argc, char** argv) {
  const std::string prefix = name + "=";
  if (argument.rfind(prefix, 0) == 0) return argument.substr(prefix.size());
  if (index + 1 >= argc) throw std::runtime_error(name + " requires a value");
  return argv[++index];
}

Arguments parse_arguments(int argc, char** argv) {
  Arguments parsed;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--help" || argument == "-h") {
      parsed.help = true;
    } else if (argument == "--version") {
      parsed.version = true;
    } else if (argument == "--self-test") {
      parsed.self_test = true;
    } else if (argument == "--quiet") {
      parsed.config.quiet = true;
    } else if (argument == "--no-native-tools") {
      parsed.config.native_tools = false;
    } else if (argument.rfind("--tunnel", 0) == 0) {
      parsed.tunnel_path = take_value(argument, "--tunnel", index, argc, argv);
    } else if (argument.rfind("--url", 0) == 0) {
      parsed.config.base_url = take_value(argument, "--url", index, argc, argv);
    } else if (argument.rfind("--model", 0) == 0) {
      parsed.config.model = take_value(argument, "--model", index, argc, argv);
    } else if (argument.rfind("--api", 0) == 0) {
      parsed.config.api = take_value(argument, "--api", index, argc, argv);
    } else if (argument.rfind("--task-file", 0) == 0) {
      parsed.task_file = take_value(argument, "--task-file", index, argc, argv);
    } else if (argument.rfind("--task", 0) == 0) {
      parsed.config.task = take_value(argument, "--task", index, argc, argv);
    } else if (argument.rfind("--system-prompt", 0) == 0) {
      parsed.system_prompt_file = take_value(argument, "--system-prompt", index, argc, argv);
    } else if (argument.rfind("--system", 0) == 0) {
      parsed.config.system_prompt = take_value(argument, "--system", index, argc, argv);
    } else if (argument.rfind("--cwd", 0) == 0) {
      parsed.config.cwd = take_value(argument, "--cwd", index, argc, argv);
    } else if (argument.rfind("--log", 0) == 0) {
      parsed.config.log_file = take_value(argument, "--log", index, argc, argv);
    } else if (argument.rfind("--max-steps", 0) == 0) {
      parsed.config.max_steps =
          std::stoi(take_value(argument, "--max-steps", index, argc, argv));
    } else if (argument.rfind("--timeout", 0) == 0) {
      parsed.config.timeout_seconds =
          std::stoi(take_value(argument, "--timeout", index, argc, argv));
    } else if (argument.rfind("--shell-timeout", 0) == 0) {
      parsed.config.shell_timeout_seconds =
          std::stoi(take_value(argument, "--shell-timeout", index, argc, argv));
    } else if (argument.rfind("--max-output", 0) == 0) {
      parsed.config.max_output_chars =
          std::stoi(take_value(argument, "--max-output", index, argc, argv));
    } else if (argument.rfind("--temperature", 0) == 0) {
      parsed.config.temperature =
          std::stod(take_value(argument, "--temperature", index, argc, argv));
    } else if (argument.rfind("--", 0) == 0) {
      throw std::runtime_error("unknown option: " + argument);
    } else {
      parsed.positional.push_back(argument);
    }
  }
  return parsed;
}

void load_tunnel(const std::string& path, agent::AgentConfig& config) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("could not open tunnel file: " + path);
  const nlohmann::json data = nlohmann::json::parse(input);

  if (config.base_url.empty()) {
    if (data.contains("url") && data["url"].is_string()) {
      config.base_url = data["url"].get<std::string>();
    } else {
      std::string address;
      if (data.contains("address") && data["address"].is_string()) {
        address = data["address"].get<std::string>();
      } else if (data.contains("host") && data.contains("port")) {
        address = data["host"].get<std::string>() + ":" + data["port"].dump();
      }
      if (!address.empty()) config.base_url = "http://" + address;
    }
  }
  if (config.model.empty() && data.contains("model") && data["model"].is_string()) {
    config.model = data["model"].get<std::string>();
  }
}

std::string environment_value(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? "" : std::string(value);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    Arguments parsed = parse_arguments(argc, argv);

    if (parsed.help) {
      print_help();
      return 0;
    }
    if (parsed.version) {
      std::cout << "llm-agent " << kVersion << "\n";
      return 0;
    }
    if (parsed.self_test) {
      return agent::run_self_test() ? 0 : 1;
    }

    agent::AgentConfig config = parsed.config;
    if (!parsed.tunnel_path.empty()) load_tunnel(parsed.tunnel_path, config);

    if (config.base_url.empty()) {
      config.base_url = environment_value("LLM_TUNNEL_URL");
      if (config.base_url.empty()) config.base_url = environment_value("TUNNEL_URL");
    }
    if (config.model.empty()) config.model = environment_value("LLM_MODEL");
    if (config.cwd.empty()) config.cwd = fs::current_path().string();
    if (!parsed.task_file.empty()) config.task = read_file(parsed.task_file);
    if (!parsed.system_prompt_file.empty()) {
      config.system_prompt = read_file(parsed.system_prompt_file);
    }
    if (config.task.empty() && !parsed.positional.empty()) {
      std::ostringstream task;
      for (std::size_t index = 0; index < parsed.positional.size(); ++index) {
        if (index > 0) task << " ";
        task << parsed.positional[index];
      }
      config.task = task.str();
    }

    if (config.base_url.empty()) {
      std::cerr << "error: provide --tunnel, --url, or LLM_TUNNEL_URL\n";
      return 2;
    }
    if (config.model.empty()) {
      std::cerr << "error: provide --model or use a tunnel file that contains a model\n";
      return 2;
    }
    if (config.task.empty()) {
      std::cerr << "error: provide --task, --task-file, or positional task text\n";
      return 2;
    }
    if (config.api != "ollama" && config.api != "openai" && config.api != "auto") {
      std::cerr << "error: --api must be ollama, openai, or auto\n";
      return 2;
    }

    return agent::run_agent(config);
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << "\n";
    return 2;
  }
}
