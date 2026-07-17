cmake_minimum_required(VERSION 3.16)
project(cloud_gateway
    VERSION 0.1.0
    DESCRIPTION "ConvAI Cloud Gateway (TCP) - reference implementation"
    LANGUAGES CXX)

# Threads
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ----------------------------------------------------------------------------
#  Build options
# ----------------------------------------------------------------------------
option(GATEWAY_BUILD_TESTS  "Build test client and unit tests" ON)
option(GATEWAY_ENABLE_LOG   "Enable verbose logging"       ON)
option(GATEWAY_USE_PCH      "Use precompiled headers"      OFF)
option(GATEWAY_WARN_AS_ERR  "Treat warnings as errors"    OFF)

# ----------------------------------------------------------------------------
#  Global compile settings
# ----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
    -Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual
    -Wformat=2 -Wmisleading-indentation
    -fno-omit-frame-pointer
)

if(GATEWAY_WARN_AS_ERR)
    add_compile_options(-Werror)
endif()

if(GATEWAY_ENABLE_LOG)
    add_compile_definitions(GATEWAY_ENABLE_LOG=1)
endif()

# ----------------------------------------------------------------------------
#  Core library
# ----------------------------------------------------------------------------
set(GATEWAY_SOURCES
    src/config.cpp
    src/protocol.cpp
    src/util/json_helper.cpp
    src/util/base64.cpp
    src/util/sha1.cpp
    src/util/event_loop.cpp
    src/ws/frame.cpp
    src/ws/handshake.cpp
    src/ws/server.cpp
    src/server/thread_pool.cpp
    src/codec/envelope.cpp
    src/session/session.cpp
    src/session/session_manager.cpp
    src/upstream/asr_backend.cpp
    src/upstream/llm_backend.cpp
    src/upstream/tts_backend.cpp
)

add_library(gateway_core STATIC ${GATEWAY_SOURCES})

target_include_directories(gateway_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(gateway_core
    PUBLIC
        Threads::Threads
)

# On Linux/macOS link pthread explicitly via Threads::Threads
if(UNIX)
    target_link_libraries(gateway_core PUBLIC rt)
endif()

# On Windows link Winsock
if(WIN32)
    target_link_libraries(gateway_core PUBLIC ws2_32)
    add_compile_definitions(_WIN32_WINNT=0x0601)
endif()

# ----------------------------------------------------------------------------
#  Daemon binary
# ----------------------------------------------------------------------------
add_executable(cloud_gateway src/main.cpp)
target_link_libraries(cloud_gateway PRIVATE gateway_core)

# ----------------------------------------------------------------------------
#  Test client
# ----------------------------------------------------------------------------
if(GATEWAY_BUILD_TESTS)
    add_executable(gateway_test_client test/test_client.cpp)
    target_link_libraries(gateway_test_client PRIVATE gateway_core)
    target_include_directories(gateway_test_client PRIVATE ${CMAKE_SOURCE_DIR}/src)

    # Unit tests
    add_executable(gateway_unit_tests test/test_main.cpp)
    target_link_libraries(gateway_unit_tests PRIVATE gateway_core)
    target_include_directories(gateway_unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
endif()

# ----------------------------------------------------------------------------
#  Install
# ----------------------------------------------------------------------------
install(TARGETS cloud_gateway RUNTIME DESTINATION bin)
if(GATEWAY_BUILD_TESTS)
    install(TARGETS gateway_test_client RUNTIME DESTINATION bin)
    install(TARGETS gateway_unit_tests RUNTIME DESTINATION bin)
endif()