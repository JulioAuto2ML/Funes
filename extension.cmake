# funes-julio — Funes extension: Julio's own agents, tools and pipelines.
#
# Included by the Funes core build when configured with
#     cmake -B build -DFUNES_EXTENSIONS=/path/to/funes-julio
# with `funes` (the executable) and `funes_core` already defined, and
# FUNES_EXTENSION_DIR set to this directory. See core/src/core/extension.h.

# Sources of the *executable*, not of funes_core: register.cpp reaches the
# binary only through its static initializer, which a static library would
# drop. (Same reason the core's generated HTTP tools are executable sources.)
file(GLOB FUNES_JULIO_SOURCES CONFIGURE_DEPENDS ${FUNES_EXTENSION_DIR}/src/*.cpp)
target_sources(funes PRIVATE ${FUNES_JULIO_SOURCES})
target_include_directories(funes PRIVATE
    ${FUNES_EXTENSION_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core/tools)   # fs_guard.h, page_text.h, tavily.h, process_runner.h
target_compile_definitions(funes PRIVATE FUNES_JULIO_ROOT="${FUNES_EXTENSION_DIR}")

# The extension's own tests, under the same ctest.
if(EXISTS ${FUNES_EXTENSION_DIR}/tests/CMakeLists.txt)
    add_subdirectory(${FUNES_EXTENSION_DIR}/tests ${CMAKE_BINARY_DIR}/funes-julio-tests)
endif()
