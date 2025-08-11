cmake_minimum_required(VERSION 3.22.1)
project(server)

set(CMAKE_CXX_STANDARD 20)

add_executable(server main.cpp
        coroutine.cpp
        server.cpp
        coroutine.h
        server.h
        server.cpp
        logger.h
        memory_pool.h)
