cmake_minimum_required(VERSION 3.20)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")
endif()

set(SCREENREC_VERSION "0.0.0" CACHE STRING "Agent version x.y.z")
if(NOT SCREENREC_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "SCREENREC_VERSION is '${SCREENREC_VERSION}', expected x.y.z")
endif()

project(ScreenRecordingAgent VERSION ${SCREENREC_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUILD_TESTS "Build the unit tests" ON)

set(LIBOBS_ROOT "" CACHE PATH "Directory holding libobs: macOS 'Frameworks/libobs.framework', Windows 'obs.lib' plus the OBS DLLs")

if(NOT LIBOBS_ROOT)
    message(FATAL_ERROR
        "LIBOBS_ROOT is not set.\n"
        "  macOS:   -DLIBOBS_ROOT=<dir containing Frameworks/libobs.framework>\n"
        "  Windows: -DLIBOBS_ROOT=<dir containing obs.lib and the OBS DLLs>\n"
        "See README.md for where to get libobs 31.0.3.")
endif()

if(APPLE)
    set(LIBOBS_FRAMEWORK "${LIBOBS_ROOT}/Frameworks/libobs.framework")
    if(NOT EXISTS "${LIBOBS_FRAMEWORK}")
        message(FATAL_ERROR "libobs.framework not found at '${LIBOBS_FRAMEWORK}'. LIBOBS_ROOT must be the directory that contains Frameworks/libobs.framework.")
    endif()
    set(LIBOBS_LIBRARY "${LIBOBS_FRAMEWORK}/Versions/A/libobs")
elseif(WIN32)
    set(LIBOBS_LIBRARY "${LIBOBS_ROOT}/obs.lib")
    if(NOT EXISTS "${LIBOBS_LIBRARY}")
        message(FATAL_ERROR "obs.lib not found at '${LIBOBS_LIBRARY}'. LIBOBS_ROOT must be the directory that contains obs.lib.")
    endif()
else()
    message(FATAL_ERROR "Unsupported platform: only Windows and macOS are targeted")
endif()

message(STATUS "Using libobs: ${LIBOBS_LIBRARY}")

if(APPLE)
    enable_language(OBJCXX)
    add_definitions(-DPLATFORM_MACOS)

    execute_process(
        COMMAND uname -m
        OUTPUT_VARIABLE HOST_ARCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(HOST_ARCH STREQUAL "arm64")
        set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Build architecture" FORCE)
    else()
        set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "Build architecture" FORCE)
    endif()
    message(STATUS "Building for ${CMAKE_OSX_ARCHITECTURES}")

    set(AGENT_PLATFORM_SOURCES
        src/platform/macos_permissions.mm
        src/platform/macos_display.mm
        src/platform/macos_status_indicator.mm
    )
    set_source_files_properties(src/platform/macos_status_indicator.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

    set(PLATFORM_LIBS
        "-framework CoreGraphics"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework AVFoundation"
        "-framework Foundation"
        "-framework AppKit"
        "-framework ColorSync"
        pthread
    )
else()
    add_definitions(-DPLATFORM_WINDOWS)
    if(MSVC)
        add_compile_options(/utf-8)
    endif()

    set(AGENT_PLATFORM_SOURCES "")
    set(PLATFORM_LIBS
        d3d11
        dxgi
        mfplat
        mfreadwrite
        mfuuid
        wtsapi32
        userenv
        crypt32
    )
endif()

set(THIRD_PARTY_HEADERS "${CMAKE_SOURCE_DIR}/third_party/headers")

foreach(required_header
        "${THIRD_PARTY_HEADERS}/json.hpp"
        "${THIRD_PARTY_HEADERS}/boost/sml.hpp"
        "${THIRD_PARTY_HEADERS}/obs/obs.h"
        "${THIRD_PARTY_HEADERS}/spdlog/spdlog.h")
    if(NOT EXISTS "${required_header}")
        message(FATAL_ERROR "Bundled header missing: ${required_header}")
    endif()
endforeach()

configure_file(
    ${CMAKE_SOURCE_DIR}/include/version.hpp.in
    ${CMAKE_BINARY_DIR}/generated/version.hpp
    @ONLY
)

include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/generated
    ${THIRD_PARTY_HEADERS}
)

set(IPC_SOURCES
    src/ipc/ipc_message.cpp
    src/ipc/ipc_server_base.cpp
    src/ipc/ipc_factory.cpp
)
if(WIN32)
    list(APPEND IPC_SOURCES src/ipc/named_pipe.cpp)
else()
    list(APPEND IPC_SOURCES src/ipc/unix_socket.cpp)
endif()

set(CAPTURE_SOURCES src/recording/capture_sources_common.cpp)
if(WIN32)
    list(APPEND CAPTURE_SOURCES src/recording/capture_sources_win.cpp)
else()
    list(APPEND CAPTURE_SOURCES src/recording/capture_sources_mac.cpp)
endif()

set(AGENT_SOURCES
    src/core/configuration_manager.cpp
    src/recording/recording_engine_libobs.cpp
    ${CAPTURE_SOURCES}
    src/recording/recording_context.cpp
    src/recording/recording_session.cpp
    src/storage/file_manager.cpp
    src/fsm/fsm_events.cpp
    src/logging/log_service.cpp
    src/logging/file_logger.cpp
    src/platform/process_path.cpp
    src/platform/crash_handler.cpp
    ${IPC_SOURCES}
)

if(WIN32)
    configure_file(${CMAKE_SOURCE_DIR}/resources/agent.rc.in ${CMAKE_BINARY_DIR}/generated/agent.rc @ONLY)
    set(AGENT_RESOURCES ${CMAKE_BINARY_DIR}/generated/agent.rc)

    add_executable(screen_recording_agent WIN32
        src/agent_main.cpp
        ${AGENT_SOURCES}
        ${AGENT_RESOURCES}
    )

    set_target_properties(screen_recording_agent PROPERTIES
        WIN32_EXECUTABLE TRUE
        LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
    )
else()
    add_executable(screen_recording_agent
        src/agent_main.cpp
        ${AGENT_SOURCES}
        ${AGENT_PLATFORM_SOURCES}
    )

    set_target_properties(screen_recording_agent PROPERTIES
        LINK_FLAGS "-Wl,-sectcreate,__TEXT,__info_plist,${CMAKE_SOURCE_DIR}/installer/macos/ScreenRecordingAgent-Info.plist"
        BUILD_RPATH "@executable_path/Frameworks;${LIBOBS_ROOT}/Frameworks"
        INSTALL_RPATH "@executable_path/Frameworks"
        MACOSX_RPATH ON
    )

    add_custom_command(TARGET screen_recording_agent POST_BUILD
        COMMAND codesign --force --sign - --identifier com.3clogic.screenrecorder.agent
            "$<TARGET_FILE:screen_recording_agent>"
        COMMENT "Ad-hoc signing agent (stable Screen Recording permission identity)"
    )
endif()

target_link_libraries(screen_recording_agent
    ${LIBOBS_LIBRARY}
    ${PLATFORM_LIBS}
)

install(TARGETS screen_recording_agent DESTINATION bin)
install(FILES config/config.example.json DESTINATION etc/screen_recording_agent)

if(BUILD_TESTS)
    enable_testing()

    add_executable(ipc_tests
        tests/test_ipc.cpp
        ${IPC_SOURCES}
    )
    if(UNIX)
        target_link_libraries(ipc_tests pthread)
    endif()
    add_test(NAME ipc_tests COMMAND ipc_tests)

    add_executable(config_defaults_tests
        tests/test_config_defaults.cpp
        src/core/configuration_manager.cpp
        src/platform/process_path.cpp
    )
    target_compile_definitions(config_defaults_tests PRIVATE
        SHIPPED_CONFIG_JSON="${CMAKE_SOURCE_DIR}/config/config.example.json"
    )
    if(UNIX)
        target_link_libraries(config_defaults_tests pthread)
    endif()
    add_test(NAME config_defaults_tests COMMAND config_defaults_tests)

    add_executable(recording_fsm_tests
        tests/test_recording_fsm.cpp
        src/recording/recording_context.cpp
        src/fsm/fsm_events.cpp
        src/logging/log_service.cpp
        src/logging/file_logger.cpp
    )
    if(UNIX)
        target_link_libraries(recording_fsm_tests pthread)
    endif()
    add_test(NAME recording_fsm_tests COMMAND recording_fsm_tests)

    foreach(header_only_test
            encoder_selection
            ipc_protocol
            monitor_layout
            output_dimensions
            pause_transition
            stop_reason)
        add_executable(${header_only_test}_tests tests/test_${header_only_test}.cpp)
        add_test(NAME ${header_only_test}_tests COMMAND ${header_only_test}_tests)
    endforeach()
endif()
