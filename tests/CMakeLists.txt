add_executable(test_memory test_memory.cpp)
target_link_libraries(test_memory PRIVATE funes_core)
add_test(NAME memory COMMAND test_memory)

add_executable(test_tools test_tools.cpp)
target_link_libraries(test_tools PRIVATE funes_core)
add_test(NAME tools COMMAND test_tools)

add_executable(test_agent_config test_agent_config.cpp)
target_link_libraries(test_agent_config PRIVATE funes_core)
# The shipped agents/ directory too: every YAML in it must parse (7.6 shipped
# two that did not — `[${HOME}/x]` is a flow mapping, not a path).
add_test(NAME agent_config COMMAND test_agent_config ${CMAKE_SOURCE_DIR}/agents)

add_executable(test_mcp_http_client test_mcp_http_client.cpp)
target_link_libraries(test_mcp_http_client PRIVATE funes_core)
add_test(NAME mcp_http_client COMMAND test_mcp_http_client)

add_executable(test_context_compressor test_context_compressor.cpp)
target_link_libraries(test_context_compressor PRIVATE funes_core)
add_test(NAME context_compressor COMMAND test_context_compressor)

# http_tool_runtime.cpp is a source of the funes *executable*, not of
# funes_core (see the root CMakeLists for why the generated tools have to be),
# so a test that calls make_http_template_tool has to compile it in directly.
add_executable(test_meta_tools test_meta_tools.cpp
    ${CMAKE_SOURCE_DIR}/src/core/tools/http_tool_runtime.cpp)
target_link_libraries(test_meta_tools PRIVATE funes_core)
add_test(NAME meta_tools COMMAND test_meta_tools)

add_executable(test_auto_memory test_auto_memory.cpp)
target_link_libraries(test_auto_memory PRIVATE funes_core)
add_test(NAME auto_memory COMMAND test_auto_memory)

add_executable(test_memory_provenance test_memory_provenance.cpp)
target_link_libraries(test_memory_provenance PRIVATE funes_core)
add_test(NAME memory_provenance COMMAND test_memory_provenance)

add_executable(test_fact_extraction test_fact_extraction.cpp)
target_link_libraries(test_fact_extraction PRIVATE funes_core)
add_test(NAME fact_extraction COMMAND test_fact_extraction)

add_executable(test_citation test_citation.cpp)
target_link_libraries(test_citation PRIVATE funes_core)
add_test(NAME citation COMMAND test_citation)

add_executable(test_text_utils test_text_utils.cpp)
target_link_libraries(test_text_utils PRIVATE funes_core)
add_test(NAME text_utils COMMAND test_text_utils)

add_executable(test_file_tools test_file_tools.cpp)
target_link_libraries(test_file_tools PRIVATE funes_core)
add_test(NAME file_tools COMMAND test_file_tools)

add_executable(test_shell_tool test_shell_tool.cpp)
target_link_libraries(test_shell_tool PRIVATE funes_core)
add_test(NAME shell_tool COMMAND test_shell_tool)

# Runs from the project root: its last case loads agents/operator.yaml and
# checks every script that agent is granted is actually installed in scriptlib/.
add_executable(test_script_library test_script_library.cpp)
target_link_libraries(test_script_library PRIVATE funes_core)
add_test(NAME script_library COMMAND test_script_library
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_web_fetch test_web_fetch.cpp)
target_link_libraries(test_web_fetch PRIVATE funes_core)
add_test(NAME web_fetch COMMAND test_web_fetch)

add_executable(test_base64 test_base64.cpp)
target_link_libraries(test_base64 PRIVATE funes_core)
add_test(NAME base64 COMMAND test_base64)

add_executable(test_password test_password.cpp)
target_link_libraries(test_password PRIVATE funes_core)
add_test(NAME password COMMAND test_password)

add_executable(test_users test_users.cpp)
target_link_libraries(test_users PRIVATE funes_core)
add_test(NAME users COMMAND test_users)

add_executable(test_llm_client_images test_llm_client_images.cpp)
target_link_libraries(test_llm_client_images PRIVATE funes_core)
add_test(NAME llm_client_images COMMAND test_llm_client_images)

add_executable(test_tool_call_recovery test_tool_call_recovery.cpp)
target_link_libraries(test_tool_call_recovery PRIVATE funes_core)
add_test(NAME tool_call_recovery COMMAND test_tool_call_recovery)

add_executable(test_context_budget test_context_budget.cpp)
target_link_libraries(test_context_budget PRIVATE funes_core)
add_test(NAME context_budget COMMAND test_context_budget)

add_executable(test_label_probs test_label_probs.cpp)
target_link_libraries(test_label_probs PRIVATE funes_core)
add_test(NAME label_probs COMMAND test_label_probs)

add_executable(test_classify_decision test_classify_decision.cpp)
target_link_libraries(test_classify_decision PRIVATE funes_core)
add_test(NAME classify_decision COMMAND test_classify_decision)

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


add_executable(test_tool_budget test_tool_budget.cpp)
target_link_libraries(test_tool_budget PRIVATE funes_core)
add_test(NAME tool_budget COMMAND test_tool_budget)

add_executable(test_tool_log test_tool_log.cpp)
target_link_libraries(test_tool_log PRIVATE funes_core)
add_test(NAME tool_log COMMAND test_tool_log)

add_executable(test_cron_schedule test_cron_schedule.cpp)
target_link_libraries(test_cron_schedule PRIVATE funes_core)
add_test(NAME cron_schedule COMMAND test_cron_schedule)

add_executable(test_cron_tool test_cron_tool.cpp)
target_link_libraries(test_cron_tool PRIVATE funes_core)
add_test(NAME cron_tool COMMAND test_cron_tool)

add_executable(test_agent_roster test_agent_roster.cpp)
target_link_libraries(test_agent_roster PRIVATE funes_core)
add_test(NAME agent_roster COMMAND test_agent_roster)

add_executable(test_lang_detect test_lang_detect.cpp)
target_link_libraries(test_lang_detect PRIVATE funes_core)
add_test(NAME lang_detect COMMAND test_lang_detect)

add_executable(test_memory_links test_memory_links.cpp)
target_link_libraries(test_memory_links PRIVATE funes_core)
add_test(NAME memory_links COMMAND test_memory_links)

add_executable(test_user_isolation test_user_isolation.cpp)
target_link_libraries(test_user_isolation PRIVATE funes_core)
add_test(NAME user_isolation COMMAND test_user_isolation)

add_executable(test_permissions test_permissions.cpp)
target_link_libraries(test_permissions PRIVATE funes_core)
add_test(NAME permissions COMMAND test_permissions)

add_executable(test_migration test_migration.cpp)
target_link_libraries(test_migration PRIVATE funes_core)
add_test(NAME migration COMMAND test_migration)

add_executable(test_web_search test_web_search.cpp)
target_link_libraries(test_web_search PRIVATE funes_core)
add_test(NAME web_search COMMAND test_web_search)

add_executable(test_parallel_search test_parallel_search.cpp)
target_link_libraries(test_parallel_search PRIVATE funes_core)
add_test(NAME parallel_search COMMAND test_parallel_search)

# src/julio and the Python beside it.
add_subdirectory(julio)

# OS plan phase 0: systemd readiness/watchdog and the background-work gate.
add_executable(test_host_signals test_host_signals.cpp)
target_link_libraries(test_host_signals PRIVATE funes_core)
add_test(NAME host_signals COMMAND test_host_signals)

# An input longer than the embedding model's context is shortened and retried,
# not mistaken for the endpoint being down (yoda, memory 537, 2026-11-04).
add_executable(test_embedding_client test_embedding_client.cpp)
target_link_libraries(test_embedding_client PRIVATE funes_core)
add_test(NAME embedding_client COMMAND test_embedding_client)

# Recall evaluation (docs/memory-plan.md, phase 1). A report, not a test: run
# it by hand before and after a memory-model change, against a real embedding
# endpoint (FUNES_EMBED_URL or --embed-url). The ctest entry runs it
# keyword-only and checks only that it completes.
add_executable(memory_eval memory_eval/memory_eval.cpp)
target_link_libraries(memory_eval PRIVATE funes_core)
target_compile_definitions(memory_eval PRIVATE
    FUNES_EVAL_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/memory_eval/fixture.json")
add_test(NAME memory_eval_runs COMMAND memory_eval --no-embed)

# What a phase-6 migration does to a real pool: top-k before and after, on a
# copy of a memory.db. A tool for reading, not a test — see the file header.
add_executable(memory_probe memory_eval/memory_probe.cpp)
target_link_libraries(memory_probe PRIVATE funes_core)

# Needs a delegated cgroup, which only systemd can hand out to an
# unprivileged process: run inside a transient scope with Delegate=yes where
# a user manager is reachable. Without one the test prints SKIP — it does not
# pass by having checked nothing (the two pure formatting cases still run).
add_executable(test_cgroup test_cgroup.cpp)
target_link_libraries(test_cgroup PRIVATE funes_core)
find_program(SYSTEMD_RUN systemd-run)
if(SYSTEMD_RUN)
    # Named, so the machine collector (host_collector.h) can tell the OOM kills
    # this test causes on purpose from real ones: every yoda deploy runs it.
    add_test(NAME cgroup COMMAND ${SYSTEMD_RUN} --user --scope -p Delegate=yes --quiet --collect
             --unit=funes-test-cgroup $<TARGET_FILE:test_cgroup>)
else()
    add_test(NAME cgroup COMMAND test_cgroup)
endif()

# Landlock needs no delegation — any unprivileged process can confine its own
# children — so this runs plainly. Prints SKIP on a kernel without Landlock.
add_executable(test_sandbox test_sandbox.cpp)
target_link_libraries(test_sandbox PRIVATE funes_core)
add_test(NAME sandbox COMMAND test_sandbox)

add_executable(test_host test_host.cpp)
target_link_libraries(test_host PRIVATE funes_core)
add_test(NAME host COMMAND test_host)

add_executable(test_local test_local.cpp)
target_link_libraries(test_local PRIVATE funes_core)
add_test(NAME local COMMAND test_local)

add_executable(test_observe test_observe.cpp)
target_link_libraries(test_observe PRIVATE funes_core)
add_test(NAME observe COMMAND test_observe)
