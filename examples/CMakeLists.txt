add_executable(hello_world hello_world.cpp)
target_link_libraries(hello_world PRIVATE draco::draco)

# coro_json: beast_json을 예제에서 직접 사용 (프레임워크 코어와 무관)
# 버그 발견 시 CONTRIBUTING.md § "beast_json 버그 리포팅" 참고
add_executable(coro_json coro_json.cpp)
target_link_libraries(coro_json PRIVATE draco::draco beast_json::beast_json)

add_executable(async_timer async_timer.cpp)
target_link_libraries(async_timer PRIVATE draco::draco)
