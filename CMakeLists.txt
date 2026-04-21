cmake_minimum_required(VERSION 3.20)
project(SerialBridge CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

if(WIN32)
    add_executable(serial_bridge host/main.cpp)
    target_include_directories(serial_bridge PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(serial_bridge PRIVATE nlohmann_json::nlohmann_json ws2_32)
    if(MSVC)
        target_compile_options(serial_bridge PRIVATE /W4)
    else()
        target_compile_options(serial_bridge PRIVATE -Wall -Wextra)
    endif()
endif()

if(UNIX)
    add_executable(serial_bridge_remote remote/main.cpp)
    target_include_directories(serial_bridge_remote PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(serial_bridge_remote PRIVATE nlohmann_json::nlohmann_json pthread)
    target_compile_options(serial_bridge_remote PRIVATE -Wall -Wextra -O2)
endif()
