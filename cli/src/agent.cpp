#include "agent.hpp"

#include "http.hpp"
#include "tools.hpp"

#include <sys/utsname.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace agent {
namespace fs = std::filesystem;
namespace {

using Clock = std::chrono::steady_clock;

std::string trim_copy(const std::string& value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return "";
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

std::string iso_time_now() {
  const std::time_t now = std::time(nullptr);
  std::tm utc{};
  gmtime_r(&now, &utc);
  std::ostringstream out;
  out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

std::string uname_text() {
  utsname info{};
  if (uname(&info) != 0) return "Linux";
  return std::string(info.sysname) + " " + info.release + " " + info.machine;
}

class Logger {
 public:
  Logger(const std::string& path, bool quiet) : quiet_(quiet) {
    if (!path.empty()) {
      file_.open(path, std::ios::app);
    }
  }

  void info(const std::string& message) { write("INFO", message, false); }
  void warn(const std::string& message) { write("WARN", message, false); }
  void error(const std::string& message) { write("ERROR", message, true); }
  void raw(const std::string& message) {
    if (file_.is_open()) file_ << message << "\n";
  }

 private:
  void write(const std::string& level, const std::string& message, bool to_error) {
    if (file_.is_open()) {
      file_ << "[" << iso_time_now() << "] [" << level << "] " << message << "\n";
      file_.flush();
    }
    if (!quiet_ || to_error) {
      std::cerr << "[" << level << "] " << message << "\n";
    }
  }

  std::ofstream file_;
  bool quiet_;
};

struct ToolCall {
  std::string id;
  std::string name;
  nlohmann::json arguments = nlohmann::json::object();
};

struct ChatResult {
  std::string content;
  std::vector<ToolCall> calls;
  bool native_calls = false;
  nlohmann::json message = nlohmann::json::object();
};

nlohmann::json arguments_from(const nlohmann::json& container) {
  for (const char* key : {"arguments", "parameters", "input", "args"}) {
    if (container.contains(key)) {
      const nlohmann::json& value = container[key];
      if (value.is_object()) return value;
      if (value.is_string()) {
        try {
          nlohmann::json parsed = nlohmann::json::parse(value.get<std::string>());
          if (parsed.is_object()) return parsed;
        } catch (...) {
          return nlohmann::json{{"command", value.get<std::string>()}};
        }
      }
    }
  }
  return nlohmann::json::object();
}

bool tool_call_from_json(const nlohmann::json& value, ToolCall& call) {
  if (!value.is_object()) return false;
  nlohmann::json function = value;
  if (value.contains("function") && value["function"].is_object()) {
    function = value["function"];
  }
  std::string name;
  if (function.contains("name") && function["name"].is_string()) {
    name = function["name"].get<std::string>();
  } else if (value.contains("tool") && value["tool"].is_string()) {
    name = value["tool"].get<std::string>();
  } else if (value.contains("name") && value["name"].is_string()) {
    name = value["name"].get<std::string>();
  }
  if (name.empty()) return false;
  call.name = name;
  call.arguments = arguments_from(function);
  if (call.arguments.empty()) call.arguments = arguments_from(value);
  if (value.contains("id") && value["id"].is_string()) {
    call.id = value["id"].get<std::string>();
  }
  return true;
}

std::vector<ToolCall> parse_native_calls(const nlohmann::json& message) {
  std::vector<ToolCall> calls;
  if (!message.contains("tool_calls") || !message["tool_calls"].is_array()) return calls;
  for (const auto& entry : message["tool_calls"]) {
    ToolCall call;
    if (tool_call_from_json(entry, call)) calls.push_back(call);
  }
  return calls;
}

void consider_candidate(const std::string& candidate, std::vector<ToolCall>& calls) {
  const std::string text = trim_copy(candidate);
  if (text.empty()) return;
  try {
    const nlohmann::json parsed = nlohmann::json::parse(text);
    if (parsed.is_object()) {
      ToolCall call;
      if (tool_call_from_json(parsed, call)) calls.push_back(call);
    } else if (parsed.is_array()) {
      for (const auto& entry : parsed) {
        ToolCall call;
        if (tool_call_from_json(entry, call)) calls.push_back(call);
      }
    }
  } catch (...) {
  }
}

void scan_balanced_objects(const std::string& text, std::vector<ToolCall>& calls) {
  for (std::size_t start = 0; start < text.size(); ++start) {
    if (text[start] != '{') continue;
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (std::size_t index = start; index < text.size(); ++index) {
      const char c = text[index];
      if (in_string) {
        if (escaped) {
          escaped = false;
        } else if (c == '\\') {
          escaped = true;
        } else if (c == '"') {
          in_string = false;
        }
        continue;
      }
      if (c == '"') {
        in_string = true;
      } else if (c == '{') {
        ++depth;
      } else if (c == '}') {
        --depth;
        if (depth == 0) {
          consider_candidate(text.substr(start, index - start + 1), calls);
          start = index;
          break;
        }
      }
    }
  }
}

bool looks_like_tool_attempt(const std::string& text) {
  const bool has_name = text.find("\"name\"") != std::string::npos ||
                        text.find("\"tool\"") != std::string::npos;
  const bool has_arguments = text.find("\"arguments\"") != std::string::npos ||
                             text.find("\"parameters\"") != std::string::npos;
  return has_name && has_arguments;
}

std::vector<ToolCall> extract_text_calls(const std::string& text) {
  std::vector<ToolCall> calls;
  std::size_t position = 0;
  while (true) {
    const auto open = text.find("```", position);
    if (open == std::string::npos) break;
    const auto line_end = text.find('\n', open + 3);
    if (line_end == std::string::npos) break;
    const std::string label = trim_copy(text.substr(open + 3, line_end - (open + 3)));
    const auto close = text.find("```", line_end + 1);
    if (close == std::string::npos) break;
    if (label == "tool" || label == "tool_call" || label == "json" || label.empty()) {
      consider_candidate(text.substr(line_end + 1, close - (line_end + 1)), calls);
    }
    position = close + 3;
  }

  {
    const std::string open_tag = "<tool_call>";
    const std::string close_tag = "</tool_call>";
    std::size_t cursor = 0;
    while (true) {
      const auto open = text.find(open_tag, cursor);
      if (open == std::string::npos) break;
      const auto close = text.find(close_tag, open + open_tag.size());
      if (close == std::string::npos) break;
      consider_candidate(text.substr(open + open_tag.size(), close - open - open_tag.size()),
                         calls);
      cursor = close + close_tag.size();
    }
  }

  {
    const std::string marker = "TOOL_CALL:";
    std::size_t cursor = 0;
    while (true) {
      const auto start = text.find(marker, cursor);
      if (start == std::string::npos) break;
      const auto line_end = text.find('\n', start);
      consider_candidate(text.substr(start + marker.size(),
                                     line_end == std::string::npos
                                         ? std::string::npos
                                         : line_end - start - marker.size()),
                         calls);
      if (line_end == std::string::npos) break;
      cursor = line_end + 1;
    }
  }

  if (calls.empty()) scan_balanced_objects(text, calls);
  return calls;
}

std::string pretty_arguments(const nlohmann::json& value) {
  std::string text = value.dump();
  if (text.size() > 300) text = text.substr(0, 300) + "...";
  return text;
}

void trim_history(nlohmann::json& messages) {
  constexpr std::size_t maximum = 80;
  if (messages.size() <= maximum) return;
  nlohmann::json trimmed = nlohmann::json::array();
  if (!messages.empty() && messages[0].value("role", "") == "system") {
    trimmed.push_back(messages[0]);
  }
  std::size_t start = messages.size() - (maximum - trimmed.size());
  while (start < messages.size() && messages[start].value("role", "") != "user") ++start;
  if (start >= messages.size()) start = messages.size() - 1;
  for (std::size_t index = start; index < messages.size(); ++index) {
    trimmed.push_back(messages[index]);
  }
  messages = std::move(trimmed);
}

std::string strip_final_marker(std::string text) {
  const std::string trimmed = trim_copy(text);
  for (const char* marker : {"FINAL:", "FINAL ANSWER:", "DONE:"}) {
    if (trimmed.rfind(marker, 0) == 0) {
      return trim_copy(trimmed.substr(std::string(marker).size()));
    }
  }
  return trimmed;
}

std::string detect_api(HttpClient& client, const std::string& configured, Logger& logger) {
  if (configured == "ollama" || configured == "openai") return configured;
  try {
    if (client.get("/api/tags").ok()) return "ollama";
  } catch (...) {
  }
  try {
    if (client.get("/v1/models").ok()) return "openai";
  } catch (...) {
  }
  logger.warn("could not auto-detect the API; defaulting to Ollama");
  return "ollama";
}

ChatResult chat_ollama(HttpClient& client, const AgentConfig& config,
                       const nlohmann::json& messages, bool use_tools, Logger& logger) {
  nlohmann::json body = {
      {"model", config.model},
      {"messages", messages},
      {"stream", false},
      {"options", {{"temperature", config.temperature}}},
  };
  if (use_tools) body["tools"] = tool_definitions();

  logger.raw("OLLAMA REQUEST " + body.dump());
  const HttpResponse response = client.post("/api/chat", body.dump());
  logger.raw("OLLAMA RESPONSE " + std::to_string(response.status) + " " + response.body);
  if (!response.ok()) {
    throw std::runtime_error("Ollama /api/chat returned HTTP " +
                             std::to_string(response.status) + ": " +
                             truncate_output(response.body, 2000));
  }

  const nlohmann::json root = nlohmann::json::parse(response.body);
  if (!root.contains("message") || !root["message"].is_object()) {
    throw std::runtime_error("Ollama response did not contain a message");
  }
  ChatResult result;
  result.message = root["message"];
  result.content = result.message.value("content", "");
  result.calls = parse_native_calls(result.message);
  result.native_calls = !result.calls.empty();
  return result;
}

ChatResult chat_openai(HttpClient& client, const AgentConfig& config,
                       const nlohmann::json& messages, bool use_tools, Logger& logger) {
  nlohmann::json body = {
      {"model", config.model},
      {"messages", messages},
      {"stream", false},
      {"temperature", config.temperature},
  };
  if (use_tools) {
    body["tools"] = tool_definitions();
    body["tool_choice"] = "auto";
  }

  logger.raw("OPENAI REQUEST " + body.dump());
  const HttpResponse response = client.post("/v1/chat/completions", body.dump());
  logger.raw("OPENAI RESPONSE " + std::to_string(response.status) + " " + response.body);
  if (!response.ok()) {
    throw std::runtime_error("OpenAI /v1/chat/completions returned HTTP " +
                             std::to_string(response.status) + ": " +
                             truncate_output(response.body, 2000));
  }

  const nlohmann::json root = nlohmann::json::parse(response.body);
  if (!root.contains("choices") || !root["choices"].is_array() || root["choices"].empty()) {
    throw std::runtime_error("OpenAI response did not contain choices");
  }
  const nlohmann::json message = root["choices"][0].value("message", nlohmann::json::object());
  ChatResult result;
  result.message = message;
  if (message.contains("content") && message["content"].is_string()) {
    result.content = message["content"].get<std::string>();
  }
  result.calls = parse_native_calls(message);
  result.native_calls = !result.calls.empty();
  return result;
}

bool tool_support_error(const std::string& message) {
  std::string lowered = message;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lowered.find("tool") != std::string::npos &&
         (lowered.find("support") != std::string::npos ||
          lowered.find("unsupported") != std::string::npos ||
          lowered.find("not available") != std::string::npos);
}

nlohmann::json assistant_message_for_text(const std::string& content) {
  return nlohmann::json{{"role", "assistant"}, {"content", content}};
}

nlohmann::json assistant_message_for_native(const ChatResult& result,
                                            const std::string& api) {
  nlohmann::json message = nlohmann::json::object();
  message["role"] = "assistant";
  message["content"] = result.content;
  nlohmann::json calls = nlohmann::json::array();
  for (const auto& call : result.calls) {
    nlohmann::json entry = nlohmann::json::object();
    if (api == "openai") {
      entry["id"] = call.id.empty() ? "call_" + std::to_string(calls.size()) : call.id;
      entry["type"] = "function";
      entry["function"] = {{"name", call.name}, {"arguments", call.arguments.dump()}};
    } else {
      entry["function"] = {{"name", call.name}, {"arguments", call.arguments}};
    }
    calls.push_back(entry);
  }
  message["tool_calls"] = calls;
  return message;
}

std::string tool_result_message(const ToolCall& call, const std::string& result,
                                bool native, const std::string& api) {
  if (!native) {
    return nlohmann::json{{"role", "user"},
                          {"content", "TOOL RESULT (" + call.name + "):\n" + result +
                                          "\n\nContinue working. If the task is complete, "
                                          "reply with a final summary and no tool call."}}
        .dump();
  }
  if (api == "openai") {
    return nlohmann::json{{"role", "tool"},
                          {"tool_call_id", call.id.empty() ? "call_0" : call.id},
                          {"name", call.name},
                          {"content", result}}
        .dump();
  }
  return nlohmann::json{{"role", "tool"}, {"content", result}}.dump();
}

}  // namespace

std::string default_system_prompt(const AgentConfig& config) {
  std::ostringstream prompt;
  prompt << "You are an autonomous software engineering agent running inside a Linux "
            "shell.\n"
         << "You can inspect files, run commands, and edit the workspace through tool "
            "calls.\n\n"
         << "Environment:\n"
         << "- Working directory: " << config.cwd << "\n"
         << "- System: " << uname_text() << "\n"
         << "- Current UTC time: " << iso_time_now() << "\n"
         << "- Maximum agent steps: " << config.max_steps << "\n"
         << "- Default shell timeout: " << config.shell_timeout_seconds << " seconds\n\n"
         << "Available tools:\n"
         << "- run_shell: run a bash command and receive exit code, stdout, and stderr.\n"
         << "- read_file: read a text file with optional offset and limit.\n"
         << "- write_file: create or overwrite a text file.\n"
         << "- list_dir: list files and directories.\n\n"
         << "Tool calling protocol:\n"
         << "1. Prefer native tool calls when the model and server support them.\n"
         << "2. If native tool calls are unavailable, reply with exactly one fenced "
            "block and no other text:\n"
         << "```tool\n"
         << "{\"name\":\"run_shell\",\"arguments\":{\"command\":\"ls -la\"}}\n"
         << "```\n"
         << "3. Use one tool call at a time. Combine shell commands with && when needed.\n"
         << "4. Inspect the workspace before changing it. Run builds and tests after "
            "edits.\n"
         << "5. Tool results are returned to you. Continue until the task is complete.\n"
         << "6. When finished, reply with a concise final summary and do not emit a tool "
            "call.\n\n"
         << "Rules:\n"
         << "- Do not ask the user questions; make reasonable assumptions and proceed.\n"
         << "- Never print, write, or commit secrets, tokens, or credentials.\n"
         << "- Avoid destructive commands such as rm -rf on important paths.\n"
         << "- Do not claim a command or test succeeded unless you actually ran it.\n"
         << "- Keep all important work inside the repository workspace.\n";
  return prompt.str();
}

int run_agent(const AgentConfig& config) {
  if (config.base_url.empty()) {
    std::cerr << "error: no API URL was provided\n";
    return 2;
  }
  if (config.model.empty()) {
    std::cerr << "error: no model was provided\n";
    return 2;
  }
  if (config.task.empty()) {
    std::cerr << "error: no task was provided\n";
    return 2;
  }

  Logger logger(config.log_file, config.quiet);
  ToolContext tool_context;
  tool_context.workdir = config.cwd;
  tool_context.default_shell_timeout = config.shell_timeout_seconds;
  tool_context.max_output_chars = config.max_output_chars;

  try {
    const HttpUrl url = HttpUrl::parse(config.base_url);
    HttpClient client(url, std::min(config.timeout_seconds, 600));
    const std::string api = detect_api(client, config.api, logger);
    logger.info("Using " + api + " API at " + config.base_url);
    logger.info("Model: " + config.model);
    logger.info("Task: " + config.task);

    nlohmann::json messages = nlohmann::json::array();
    const std::string system_prompt =
        config.system_prompt.empty() ? default_system_prompt(config) : config.system_prompt;
    messages.push_back({{"role", "system"}, {"content", system_prompt}});
    messages.push_back({{"role", "user"}, {"content", config.task}});

    const auto deadline = Clock::now() + std::chrono::seconds(config.timeout_seconds);
    bool use_native_tools = config.native_tools;
    int malformed_tool_retries = 0;

    for (int step = 1; step <= config.max_steps; ++step) {
      if (Clock::now() >= deadline) {
        logger.error("agent time limit reached after " + std::to_string(step - 1) +
                     " step(s)");
        return 2;
      }
      trim_history(messages);
      logger.info("Step " + std::to_string(step) + "/" +
                  std::to_string(config.max_steps));

      ChatResult result;
      try {
        result = api == "openai"
                     ? chat_openai(client, config, messages, use_native_tools, logger)
                     : chat_ollama(client, config, messages, use_native_tools, logger);
      } catch (const std::exception& error) {
        if (use_native_tools && tool_support_error(error.what())) {
          logger.warn("the model rejected native tools; falling back to the text protocol");
          use_native_tools = false;
          --step;
          continue;
        }
        throw;
      }

      std::vector<ToolCall> calls = result.calls;
      bool native = result.native_calls;
      bool fallback_calls = false;
      if (calls.empty()) {
        calls = extract_text_calls(result.content);
        fallback_calls = !calls.empty();
      }

      if (calls.empty()) {
        if (trim_copy(result.content).empty()) {
          logger.warn("empty assistant response; asking the model to continue");
          messages.push_back(
              {{"role", "user"},
               {"content", "Your response was empty. Continue the task or provide a final "
                           "answer."}});
          continue;
        }
        if (malformed_tool_retries < 2 && looks_like_tool_attempt(result.content)) {
          ++malformed_tool_retries;
          logger.warn("assistant response looked like a tool call but was not valid JSON; "
                      "asking the model to retry");
          messages.push_back(assistant_message_for_text(result.content));
          messages.push_back(
              {{"role", "user"},
               {"content", "That tool call could not be parsed. Reply with exactly one "
                           "fenced ```tool block containing valid JSON, for example: "
                           "```tool\n{\"name\":\"run_shell\",\"arguments\":"
                           "{\"command\":\"ls -la\"}}\n```"}});
          continue;
        }
        const std::string final_answer = strip_final_marker(result.content);
        std::cout << final_answer << std::endl;
        logger.info("agent finished after " + std::to_string(step) + " step(s)");
        return 0;
      }

      messages.push_back(native ? assistant_message_for_native(result, api)
                                : assistant_message_for_text(result.content));

      for (const auto& call : calls) {
        logger.info("tool " + call.name + " " + pretty_arguments(call.arguments));
        const std::string output = execute_tool(call.name, call.arguments, tool_context);
        logger.raw("TOOL RESULT " + call.name + " " + output);
        if (!config.quiet) {
          std::cerr << "  -> " << truncate_output(output, 1200) << "\n";
        }
        messages.push_back(
            nlohmann::json::parse(tool_result_message(call, output, native && !fallback_calls,
                                                      api)));
      }
    }

    logger.error("maximum number of steps reached without a final answer");
    return 2;
  } catch (const std::exception& error) {
    logger.error(std::string("agent failed: ") + error.what());
    return 1;
  }
}

bool run_self_test() {
  int failures = 0;
  auto check = [&](const std::string& name, bool condition) {
    std::cout << (condition ? "PASS " : "FAIL ") << name << "\n";
    if (!condition) ++failures;
  };

  try {
    const HttpUrl parsed = HttpUrl::parse("bore.pub:32271");
    check("url host", parsed.host == "bore.pub");
    check("url port", parsed.port == 32271);
    check("url scheme", parsed.scheme == "http");

    const HttpUrl with_path = HttpUrl::parse("http://127.0.0.1:11434/api");
    check("url base path", with_path.base_path == "/api");
    check("url default port", HttpUrl::parse("http://example.com").port == 80);
  } catch (const std::exception& error) {
    std::cout << "FAIL url parsing: " << error.what() << "\n";
    ++failures;
  }

  {
    const nlohmann::json definitions = tool_definitions();
    check("four tools", definitions.is_array() && definitions.size() == 4);
    bool names_ok = definitions.is_array();
    if (names_ok) {
      std::vector<std::string> names;
      for (const auto& entry : definitions) {
        names.push_back(entry["function"].value("name", ""));
      }
      names_ok = std::find(names.begin(), names.end(), "run_shell") != names.end() &&
                 std::find(names.begin(), names.end(), "read_file") != names.end() &&
                 std::find(names.begin(), names.end(), "write_file") != names.end() &&
                 std::find(names.begin(), names.end(), "list_dir") != names.end();
    }
    check("tool names", names_ok);
  }

  {
    const std::string text =
        "I will inspect the workspace.\n```tool\n"
        "{\"name\":\"run_shell\",\"arguments\":{\"command\":\"ls -la\"}}\n```\n";
    const auto calls = extract_text_calls(text);
    check("fenced tool call parsed", calls.size() == 1 && calls[0].name == "run_shell" &&
                                          calls[0].arguments.value("command", "") == "ls -la");
  }

  {
    const auto calls = extract_text_calls(
        "<tool_call>{\"tool\":\"list_dir\",\"parameters\":{\"path\":\".\"}}</tool_call>");
    check("tagged tool call parsed",
          calls.size() == 1 && calls[0].name == "list_dir" &&
              calls[0].arguments.value("path", "") == ".");
  }

  {
    const std::string text =
        "{\"name\":\"run_shell\",\"arguments\":{\"command\":\"ls\","
        "\"timeout_seconds\":{\"type\":\"integer\"}}}}\n{\"output\":\"ok\"}";
    const auto calls = extract_text_calls(text);
    check("unfenced tool call parsed", calls.size() == 1 &&
                                          calls[0].name == "run_shell" &&
                                          calls[0].arguments.value("command", "") == "ls");
  }

  {
    const ShellOutcome outcome = run_shell("printf hello", ".", 5);
    check("shell stdout", outcome.exit_code == 0 && outcome.stdout_text == "hello");
    check("shell not timed out", !outcome.timed_out);
  }

  {
    const ShellOutcome outcome = run_shell("sleep 3", ".", 1);
    check("shell timeout", outcome.timed_out);
  }

  {
    char directory_template[] = "/tmp/llm-agent-selftest-XXXXXX";
    const char* created = mkdtemp(directory_template);
    if (created == nullptr) {
      check("temp directory", false);
    } else {
      const std::string root(created);
      ToolContext context;
      context.workdir = root;
      const std::string written = execute_tool(
          "write_file", {{"path", "nested/file.txt"}, {"content", "hello\nworld\n"}}, context);
      check("write_file", nlohmann::json::parse(written).value("ok", false));
      const std::string read = execute_tool("read_file", {{"path", "nested/file.txt"}}, context);
      const nlohmann::json read_json = nlohmann::json::parse(read);
      check("read_file", read_json.value("ok", false) &&
                             read_json.value("content", "").find("world") != std::string::npos);
      const std::string listed = execute_tool("list_dir", {{"path", "."}, {"recursive", true}},
                                              context);
      const nlohmann::json listed_json = nlohmann::json::parse(listed);
      check("list_dir", listed_json.value("ok", false) &&
                            listed_json["entries"].size() >= 2);
      std::error_code ignored;
      fs::remove_all(root, ignored);
    }
  }

  std::cout << (failures == 0 ? "SELF-TEST PASSED" : "SELF-TEST FAILED") << "\n";
  return failures == 0;
}

}  // namespace agent
