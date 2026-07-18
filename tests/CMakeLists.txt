add_executable(test_task test_task.cpp)
target_link_libraries(test_task PRIVATE async_net)
add_test(NAME test_task COMMAND test_task)

add_executable(test_spawn test_spawn.cpp)
target_link_libraries(test_spawn PRIVATE async_net)
add_test(NAME test_spawn COMMAND test_spawn)

add_executable(test_h2_push test_h2_push.cpp)
target_link_libraries(test_h2_push PRIVATE async_net)
add_test(NAME test_h2_push COMMAND test_h2_push)

add_executable(test_executor test_executor.cpp)
target_link_libraries(test_executor PRIVATE async_net)
add_test(NAME test_executor COMMAND test_executor)

add_executable(test_schedule test_schedule.cpp)
target_link_libraries(test_schedule PRIVATE async_net)
add_test(NAME test_schedule COMMAND test_schedule)
