cmake_minimum_required(VERSION 3.3)
project(ftxHttpClient)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")

set(SOURCE_FILES HttpClient.cpp HttpClient.h)

add_library(ftxHttpClient STATIC ${SOURCE_FILES})

target_link_libraries(ftxHttpClient curl)