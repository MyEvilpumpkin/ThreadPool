cmake_minimum_required(VERSION 3.8)

project(ThreadPool)

set(CMAKE_CXX_STANDARD 20)

SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")

file(GLOB sources "${CMAKE_CURRENT_SOURCE_DIR}/ThreadPool/*.cpp")
file(GLOB headers "${CMAKE_CURRENT_SOURCE_DIR}/ThreadPool/*.h")

add_executable(${PROJECT_NAME} ${sources} ${headers})
