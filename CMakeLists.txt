cmake_minimum_required(VERSION 3.14)
project(Funes VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The funes binary lands in <project_root>/bin/ so it can find ./ui and ./agents
# relative to the project root when run from there.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

# OpenSSL — needed by httplib for HTTPS (cloud LLM APIs, web_search, web_fetch).
# Install: sudo apt-get install -y libssl-dev
find_package(OpenSSL REQUIRED)

# yaml-cpp — agent definition files.
# Install: sudo apt-get install -y libyaml-cpp-dev
find_library(YAML_CPP_LIB yaml-cpp REQUIRED)

# ── SQLite + sqlite-vec (vendored, public domain / Apache-2.0) ────────────────
# Compiled together as one static lib. SQLITE_CORE lets sqlite-vec register as
# a statically linked extension via sqlite3_vec_init() instead of a loadable .so.
add_library(funes_sqlite STATIC
    third-party/sqlite/sqlite3.c
    third-party/sqlite-vec/sqlite-vec.c
)
target_include_directories(funes_sqlite PUBLIC
    third-party/sqlite
    third-party/sqlite-vec
)
# PUBLIC: sqlite-vec.h includes sqlite3ext.h, which expands to extension-shim
# macros unless SQLITE_CORE is defined in the including TU as well.
target_compile_definitions(funes_sqlite PUBLIC SQLITE_CORE)
target_link_libraries(funes_sqlite PUBLIC ${CMAKE_DL_LIBS} m)

# ── cpp-mcp (vendored, MIT) ───────────────────────────────────────────────────
# Provides the MCP client (external tool servers), plus httplib.h and json.hpp.
#
# IMPORTANT: CPPHTTPLIB_OPENSSL_SUPPORT must be defined for ALL translation
# units that include httplib.h. If any TU compiles httplib with OpenSSL and
# another without, httplib::Client has different class layouts → ODR violation
# → heap corruption at runtime. It's PUBLIC on this target so it propagates to
# everything that links mcp.
set(CPP_MCP_DIR ${CMAKE_SOURCE_DIR}/third-party/cpp-mcp)

add_library(mcp STATIC
    ${CPP_MCP_DIR}/src/mcp_message.cpp
    ${CPP_MCP_DIR}/src/mcp_resource.cpp
    ${CPP_MCP_DIR}/src/mcp_server.cpp
    ${CPP_MCP_DIR}/src/mcp_tool.cpp
    ${CPP_MCP_DIR}/src/mcp_sse_client.cpp
    ${CPP_MCP_DIR}/src/mcp_stdio_client.cpp
)
target_include_directories(mcp PUBLIC
    ${CPP_MCP_DIR}/include
    ${CPP_MCP_DIR}/common
)
target_compile_definitions(mcp PUBLIC CPPHTTPLIB_OPENSSL_SUPPORT)
target_link_libraries(mcp PUBLIC
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
)

# ── funes core library ────────────────────────────────────────────────────────
add_library(funes_core STATIC
    src/core/llm_client.cpp
    src/core/agent_config.cpp
    src/core/context_compressor.cpp
    src/core/text_utils.cpp
    src/core/base64.cpp
    src/core/memory.cpp
    src/core/tools.cpp
    src/core/tools/net_guard.cpp
    src/core/tools/fs_guard.cpp
    src/core/tools/process_runner.cpp
    src/core/tools/pdf_extract.cpp
    src/core/tools/web_search.cpp
    src/core/tools/web_fetch.cpp
    src/core/tools/memory_tools.cpp
    src/core/tools/context_tools.cpp
    src/core/tools/introspection.cpp
    src/core/tools/tool_builder.cpp
    src/core/tools/agent_builder.cpp
    src/core/tools/file_tools.cpp
    src/core/tools/shell_tool.cpp
    src/core/tools/delegation.cpp
    src/core/agent.cpp
)
target_include_directories(funes_core PUBLIC src/core)
target_link_libraries(funes_core PUBLIC mcp funes_sqlite ${YAML_CPP_LIB})

# ── funes binary ──────────────────────────────────────────────────────────────
# Tools scaffolded by the `create_tool` meta-tool (see src/core/tools/tool_builder.cpp)
# land in src/core/tools/generated/ as plain .cpp files that self-register via a
# static initializer (see http_tool_runtime.h). CONFIGURE_DEPENDS means new ones
# are picked up on the next `cmake --build` without editing this file.
#
# These must be sources of the *executable*, not of funes_core: a static
# library only pulls in .o files that resolve an external symbol reference,
# and nothing calls a generated file's self-registration function directly —
# it's only reached through a function pointer recorded by its own static
# initializer. Linked via a .a, that .o would be silently dropped.
file(GLOB FUNES_GENERATED_TOOLS CONFIGURE_DEPENDS src/core/tools/generated/*.cpp)

add_executable(funes
    src/server/main.cpp
    src/server/api.cpp
    src/core/tools/http_tool_runtime.cpp
    ${FUNES_GENERATED_TOOLS}
)
target_link_libraries(funes PRIVATE funes_core)

# ── tests ─────────────────────────────────────────────────────────────────────
enable_testing()
add_subdirectory(tests)
