cmake_minimum_required(VERSION 3.16)

project(llm_agent VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(llm-agent
  src/main.cpp
  src/agent.cpp
  src/http.cpp
  src/tools.cpp
)

target_include_directories(llm-agent PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

if(MSVC)
  target_compile_options(llm-agent PRIVATE /W4 /O2)
else()
  target_compile_options(llm-agent PRIVATE -Wall -Wextra -O2)
endif()

find_package(Threads REQUIRED)
target_link_libraries(llm-agent PRIVATE Threads::Threads)

option(LLM_AGENT_STATIC "Link a fully static binary" OFF)
if(LLM_AGENT_STATIC AND NOT MSVC)
  target_link_options(llm-agent PRIVATE -static)
endif()

install(TARGETS llm-agent RUNTIME DESTINATION bin)

enable_testing()
add_test(NAME llm-agent-self-test COMMAND llm-agent --self-test)
