cmake_minimum_required(VERSION 3.16)
project(cpp_proxy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(cpp_proxy
    src/main.cpp
    src/network/socket.cpp
    src/http/http_parser.cpp
    src/proxy/client_handler.cpp
    src/proxy/proxy_server.cpp
)

target_include_directories(cpp_proxy PRIVATE include)

if (UNIX)
    target_compile_options(cpp_proxy PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(cpp_proxy PRIVATE pthread)
endif()
