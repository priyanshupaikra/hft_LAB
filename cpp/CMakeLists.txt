cmake_minimum_required(VERSION 3.16)
project(hft_lab CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)

add_library(hft_core
  engine/order_book.cpp
  engine/matching_engine.cpp
  gateway/trading_core.cpp
  gateway/server.cpp
  gateway/md_publisher.cpp
  common/counting_new.cpp
)
target_include_directories(hft_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hft_core PUBLIC Threads::Threads)

add_executable(hft_gateway apps/gateway_main.cpp)
target_link_libraries(hft_gateway PRIVATE hft_core)

add_executable(hft_bench apps/bench_engine.cpp)
target_link_libraries(hft_bench PRIVATE hft_core)

add_executable(hft_bench_gw apps/bench_gateway.cpp)
target_link_libraries(hft_bench_gw PRIVATE hft_core)

add_executable(hft_tests tests/test_main.cpp tests/unit_tests.cpp)
target_link_libraries(hft_tests PRIVATE hft_core)
