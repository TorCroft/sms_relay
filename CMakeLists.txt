cmake_minimum_required(VERSION 3.20)

project(SMSRelay
    VERSION 0.1.0
    DESCRIPTION "SMS forwarding service"
    LANGUAGES CXX
)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for IDE support (VSCode, clangd, etc.)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

include(FetchContent)

set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_deps" CACHE PATH "Dependency cache directory")
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-34-2
    GIT_SHALLOW TRUE
)

FetchContent_GetProperties(asio)
if(NOT asio_POPULATED)
    FetchContent_Populate(asio)
endif()

add_library(asio INTERFACE)
add_library(asio::asio ALIAS asio)
target_include_directories(asio INTERFACE "${asio_SOURCE_DIR}/asio/include")
target_compile_definitions(asio INTERFACE ASIO_STANDALONE)

if(WIN32)
    target_compile_definitions(asio INTERFACE _WIN32_WINNT=0x0601)
endif()

# yaml-cpp
find_package(yaml-cpp QUIET)

if(NOT yaml-cpp_FOUND)
    FetchContent_Declare(
        yaml-cpp
        GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
        GIT_TAG master
        GIT_SHALLOW TRUE
    )

    FetchContent_GetProperties(yaml-cpp)
    if(NOT yaml-cpp_POPULATED)
        FetchContent_Populate(yaml-cpp)
    endif()

    # Use yaml-cpp's own CMake build system
    set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(YAML_INSTALL_NO_EXAMPLES ON CACHE BOOL "" FORCE)

    add_subdirectory(${yaml-cpp_SOURCE_DIR} ${yaml-cpp_BINARY_DIR})
endif()

if(WIN32)
    target_compile_definitions(asio INTERFACE _WIN32_WINNT=0x0601)
endif()

# nlohmann/json
find_package(nlohmann_json 3.12.0 QUIET)

if(NOT nlohmann_json_FOUND)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.12.0
        GIT_SHALLOW TRUE
    )

    FetchContent_GetProperties(nlohmann_json)
    if(NOT nlohmann_json_POPULATED)
        FetchContent_Populate(nlohmann_json)
    endif()

    add_subdirectory(${nlohmann_json_SOURCE_DIR} ${nlohmann_json_BINARY_DIR})
endif()

# ============================================================================
# sms_relay - Main service
# ============================================================================
add_executable(sms_relay
    src/sms_relay/main.cpp
    src/common/pdu/pdu.cpp
    src/common/config_loader.cpp
    src/sms_relay/transport/serial_transport.cpp
    src/sms_relay/at/response_builder.cpp
    src/sms_relay/at/command_queue.cpp
    src/sms_relay/at/dispatcher.cpp
    src/sms_relay/at/at_session.cpp
    src/sms_relay/sms/sms_service.cpp
    src/sms_relay/ipc/ipc_server.cpp
    src/sms_relay/http/http_client.cpp
    src/sms_relay/forward/template_renderer.cpp
    src/sms_relay/forward/forward_service.cpp
    src/sms_relay/forward/targets/bark_target.cpp
)

target_include_directories(sms_relay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(sms_relay PRIVATE
    asio::asio
    yaml-cpp
    nlohmann_json::nlohmann_json
)

if(MSVC)
    target_compile_options(sms_relay PRIVATE /W4 /permissive-)
    target_compile_definitions(sms_relay PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(sms_relay PRIVATE
        -Wall -Wextra -Wpedantic
    )
endif()

if(WIN32)
    target_link_libraries(sms_relay PRIVATE ws2_32)
endif()

# ============================================================================
# sms_relay_cli - CLI tool
# ============================================================================
add_executable(sms_relay_cli
    src/sms_relay_cli/main.cpp
    src/sms_relay_cli/ipc/ipc_client.cpp
)

target_include_directories(sms_relay_cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(sms_relay_cli PRIVATE
    asio::asio
)

if(MSVC)
    target_compile_options(sms_relay_cli PRIVATE /W4 /permissive-)
    target_compile_definitions(sms_relay_cli PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(sms_relay_cli PRIVATE
        -Wall -Wextra -Wpedantic
    )
endif()

if(WIN32)
    target_link_libraries(sms_relay_cli PRIVATE ws2_32)
endif()

message(STATUS "")
message(STATUS "SMSRelay configuration")
message(STATUS "  Compiler:       ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Generator:      ${CMAKE_GENERATOR}")
message(STATUS "  Build type:     ${CMAKE_BUILD_TYPE}")
message(STATUS "  Fetch base dir: ${FETCHCONTENT_BASE_DIR}")
message(STATUS "")
