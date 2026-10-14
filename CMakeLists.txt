cmake_minimum_required(VERSION 3.16)
project(cpp_proxy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    web/index.html web/style.css web/app.js config/default_domains.txt)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html" DASHBOARD_HTML)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/web/style.css" DASHBOARD_CSS)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/web/app.js" DASHBOARD_JS)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/config/default_domains.txt" DEFAULT_DOMAINS)
configure_file(web/assets.hpp.in generated/dashboard_assets.hpp @ONLY)

add_executable(cpp_proxy
    src/main.cpp
    src/network/socket.cpp
    src/http/http_parser.cpp
    src/logging/logger.cpp
    src/logging/statistics.cpp
    src/proxy/work_queue.cpp
    src/proxy/cache.cpp
    src/proxy/privacy.cpp
    src/proxy/dashboard.cpp
    src/proxy/client_handler.cpp
    src/proxy/proxy_server.cpp
)

target_include_directories(cpp_proxy PRIVATE include "${CMAKE_CURRENT_BINARY_DIR}/generated")

if (UNIX)
    target_compile_options(cpp_proxy PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(cpp_proxy PRIVATE pthread)
endif()
