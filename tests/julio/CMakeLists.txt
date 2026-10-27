# Tests for src/julio and the Python beside it (publishing/, scripts/). Added
# from tests/CMakeLists.txt, so `cd build && ctest` runs them with the rest.
#
# Each test links funes_core and compiles the extension source it exercises
# directly, the same way the core's test_meta_tools compiles in
# http_tool_runtime.cpp: these sources are not in the library.
set(EXT_SRC ${CMAKE_SOURCE_DIR}/src/julio)
set(EXT_INCLUDES ${EXT_SRC} ${CMAKE_SOURCE_DIR}/src/core/tools)

function(julio_test name)   # julio_test(<name> <extension sources...>)
    add_executable(test_${name} test_${name}.cpp ${ARGN})
    target_include_directories(test_${name} PRIVATE ${EXT_INCLUDES})
    target_link_libraries(test_${name} PRIVATE funes_core)
    # From this repository's root: several cases load publications/*.yaml and
    # pipelines/*.yaml, the configs the live agents actually run on.
    add_test(NAME julio_${name} COMMAND test_${name}
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endfunction()

julio_test(publication      ${EXT_SRC}/publication.cpp)
julio_test(pipeline         ${EXT_SRC}/pipeline.cpp)
julio_test(structured_tools ${EXT_SRC}/pipeline.cpp ${EXT_SRC}/structured.cpp)
julio_test(merge_rankings   ${EXT_SRC}/rankings.cpp)
julio_test(issue            ${EXT_SRC}/issue.cpp ${EXT_SRC}/publication.cpp)
julio_test(harvest          ${EXT_SRC}/harvest.cpp ${EXT_SRC}/publication.cpp)

# publishing/ (Python). publish_newsletter.py decides whether mail goes out, so
# its suite runs under the same ctest; --self-test is its own entry point into
# publishing/tests/, one set of assertions for CI and the sending host alike.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME julio_publishing
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_SOURCE_DIR}/publishing/publish_newsletter.py --self-test)
    # scripts/ (Python). Where whatsapp_autoresponder.py looks for its config
    # is the kind of thing that breaks silently on a move, so it is pinned by
    # tests rather than by a comment.
    add_test(NAME julio_autoresponder
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_SOURCE_DIR}/scripts/whatsapp_autoresponder.py --self-test)
else()
    message(STATUS "python3 not found — skipping the publishing/ tests")
endif()
