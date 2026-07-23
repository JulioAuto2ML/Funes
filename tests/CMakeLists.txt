add_executable(test_memory test_memory.cpp)
target_link_libraries(test_memory PRIVATE funes_core)
add_test(NAME memory COMMAND test_memory)

add_executable(test_tools test_tools.cpp)
target_link_libraries(test_tools PRIVATE funes_core)
add_test(NAME tools COMMAND test_tools)

add_executable(test_agent_config test_agent_config.cpp)
target_link_libraries(test_agent_config PRIVATE funes_core)
add_test(NAME agent_config COMMAND test_agent_config)
