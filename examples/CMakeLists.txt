add_executable(hello_world hello_world.cpp)
target_link_libraries(hello_world PRIVATE draco::draco)

add_executable(async_timer async_timer.cpp)
target_link_libraries(async_timer PRIVATE draco::draco)
