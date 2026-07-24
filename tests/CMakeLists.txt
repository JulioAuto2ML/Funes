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
