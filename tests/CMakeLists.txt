add_executable(test_memory test_memory.cpp)
target_link_libraries(test_memory PRIVATE funes_core)
add_test(NAME memory COMMAND test_memory)

add_executable(test_tools test_tools.cpp)
target_link_libraries(test_tools PRIVATE funes_core)
add_test(NAME tools COMMAND test_tools)

add_executable(test_agent_config test_agent_config.cpp)
target_link_libraries(test_agent_config PRIVATE funes_core)
add_test(NAME agent_config COMMAND test_agent_config)

add_executable(test_context_compressor test_context_compressor.cpp)
target_link_libraries(test_context_compressor PRIVATE funes_core)
add_test(NAME context_compressor COMMAND test_context_compressor)

add_executable(test_meta_tools test_meta_tools.cpp)
target_link_libraries(test_meta_tools PRIVATE funes_core)
add_test(NAME meta_tools COMMAND test_meta_tools)

add_executable(test_text_utils test_text_utils.cpp)
target_link_libraries(test_text_utils PRIVATE funes_core)
add_test(NAME text_utils COMMAND test_text_utils)

add_executable(test_file_tools test_file_tools.cpp)
target_link_libraries(test_file_tools PRIVATE funes_core)
add_test(NAME file_tools COMMAND test_file_tools)

add_executable(test_shell_tool test_shell_tool.cpp)
target_link_libraries(test_shell_tool PRIVATE funes_core)
add_test(NAME shell_tool COMMAND test_shell_tool)

add_executable(test_web_fetch test_web_fetch.cpp)
target_link_libraries(test_web_fetch PRIVATE funes_core)
add_test(NAME web_fetch COMMAND test_web_fetch)

add_executable(test_base64 test_base64.cpp)
target_link_libraries(test_base64 PRIVATE funes_core)
add_test(NAME base64 COMMAND test_base64)

add_executable(test_llm_client_images test_llm_client_images.cpp)
target_link_libraries(test_llm_client_images PRIVATE funes_core)
add_test(NAME llm_client_images COMMAND test_llm_client_images)

add_executable(test_delegation test_delegation.cpp)
target_link_libraries(test_delegation PRIVATE funes_core)
add_test(NAME delegation COMMAND test_delegation)

add_executable(test_completion_contract test_completion_contract.cpp)
target_link_libraries(test_completion_contract PRIVATE funes_core)
add_test(NAME completion_contract COMMAND test_completion_contract)

add_executable(test_result_store test_result_store.cpp)
target_link_libraries(test_result_store PRIVATE funes_core)
add_test(NAME result_store COMMAND test_result_store)

add_executable(test_answer_schema test_answer_schema.cpp)
target_link_libraries(test_answer_schema PRIVATE funes_core)
add_test(NAME answer_schema COMMAND test_answer_schema)

add_executable(test_run_outcome test_run_outcome.cpp)
target_link_libraries(test_run_outcome PRIVATE funes_core)
add_test(NAME run_outcome COMMAND test_run_outcome)

# Runs from the project root: one of its cases loads publications/ai-pulse.yaml,
# the config the daily newsletter actually goes out on.
add_executable(test_publication test_publication.cpp)
target_link_libraries(test_publication PRIVATE funes_core)
add_test(NAME publication COMMAND test_publication
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_issue test_issue.cpp)
target_link_libraries(test_issue PRIVATE funes_core)
add_test(NAME issue COMMAND test_issue)

add_executable(test_harvest test_harvest.cpp)
target_link_libraries(test_harvest PRIVATE funes_core)
add_test(NAME harvest COMMAND test_harvest)

add_executable(test_tool_budget test_tool_budget.cpp)
target_link_libraries(test_tool_budget PRIVATE funes_core)
add_test(NAME tool_budget COMMAND test_tool_budget)

# ── publishing/ (Python) ──────────────────────────────────────────────────────
# publish_newsletter.py is the component that decides whether mail goes out, so
# its suite runs under the same `ctest` as everything else rather than behind a
# flag someone has to remember. --self-test is the script's own entry point into
# publishing/tests/, so there is one set of assertions rather than a CI copy and
# a deployed copy that drift.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME publishing
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_SOURCE_DIR}/publishing/publish_newsletter.py --self-test)
else()
    message(STATUS "python3 not found — skipping the publishing/ tests")
endif()
