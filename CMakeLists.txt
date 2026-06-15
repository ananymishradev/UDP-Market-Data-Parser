cmake_minimum_required(VERSION 3.14)
project(market_data_parser LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_executable(market_data_consumer
    src/market_data_consumer.cpp
    src/main.cpp
)
target_include_directories(market_data_consumer PRIVATE include)
target_link_libraries(market_data_consumer PRIVATE Threads::Threads)
target_compile_definitions(market_data_consumer PRIVATE _GNU_SOURCE)

add_executable(mock_feed_generator
    src/mock_feed_generator.cpp
)
target_include_directories(mock_feed_generator PRIVATE include)
target_link_libraries(mock_feed_generator PRIVATE Threads::Threads)
