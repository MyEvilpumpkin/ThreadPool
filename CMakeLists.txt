cmake_minimum_required(VERSION 3.8)

project(ThreadPool)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")

add_subdirectory(ThreadPool)
add_subdirectory(ThreadPoolSample)
