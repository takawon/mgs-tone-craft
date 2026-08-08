cmake_minimum_required(VERSION 3.25)

include(FetchContent)

project(
    MgsToneCraft
    VERSION 0.1.0
    DESCRIPTION "MGSDRV-compatible compound timbre editor"
    LANGUAGES C CXX
)

option(MGSTC_BUILD_TESTS "Build engine tests" ON)

# About shows MGSTC_DOC_VERSION. Prefer local SPECIFICATION.md when present
# (private working tree). Public checkouts use the tracked VERSION file.
set(MGSTC_SPECIFICATION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/SPECIFICATION.md")
set(MGSTC_VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
set(MGSTC_DOC_VERSION "${PROJECT_VERSION}")
if(EXISTS "${MGSTC_SPECIFICATION_FILE}")
    file(
        STRINGS "${MGSTC_SPECIFICATION_FILE}" _mgstc_doc_version_lines
        REGEX "^- [^:]+: [0-9]+\\.[0-9]+[ \t]*$"
        LIMIT_COUNT 1
        ENCODING UTF-8
    )
    if(_mgstc_doc_version_lines)
        string(
            REGEX REPLACE "^.*: ([0-9]+\\.[0-9]+)[ \t]*$" "\\1"
            MGSTC_DOC_VERSION "${_mgstc_doc_version_lines}"
        )
    endif()
    set_property(
        DIRECTORY
        APPEND
        PROPERTY CMAKE_CONFIGURE_DEPENDS "${MGSTC_SPECIFICATION_FILE}"
    )
elseif(EXISTS "${MGSTC_VERSION_FILE}")
    file(STRINGS "${MGSTC_VERSION_FILE}" _mgstc_version_lines LIMIT_COUNT 1)
    if(_mgstc_version_lines)
        string(STRIP "${_mgstc_version_lines}" MGSTC_DOC_VERSION)
    endif()
    set_property(
        DIRECTORY
        APPEND
        PROPERTY CMAKE_CONFIGURE_DEPENDS "${MGSTC_VERSION_FILE}"
    )
endif()
message(STATUS "MGSTC document version: ${MGSTC_DOC_VERSION}")

# MAmidiMEmo chip_server speaks msgpack-RPC (rpclib). Pin a published
# release; protocol is compatible with the copy bundled in MAmidiMEmo.
# Modern MSVC removed stdext::checked_array_iterator — apply the same
# guard used by vcpkg (ports/rpclib/avoid-stdext-checked-array-iterator).
set(RPCLIB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(RPCLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(RPCLIB_ENABLE_LOGGING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    rpclib
    GIT_REPOSITORY https://github.com/rpclib/rpclib.git
    GIT_TAG v2.3.0
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
)
FetchContent_GetProperties(rpclib)
if(NOT rpclib_POPULATED)
    FetchContent_Populate(rpclib)

    # Modern MSVC removed stdext::checked_array_iterator (vcpkg same fix).
    set(_mgstc_rpclib_format_h
        "${rpclib_SOURCE_DIR}/dependencies/include/format.h")
    file(READ "${_mgstc_rpclib_format_h}" _mgstc_rpclib_format)
    string(REGEX REPLACE
        "#ifdef _SECURE_SCL\r?\n# define FMT_SECURE_SCL _SECURE_SCL"
        "#if defined(_SECURE_SCL) && defined(_MSC_VER) && _MSC_VER < 1951\n# define FMT_SECURE_SCL _SECURE_SCL"
        _mgstc_rpclib_format
        "${_mgstc_rpclib_format}")
    file(WRITE "${_mgstc_rpclib_format_h}" "${_mgstc_rpclib_format}")
    unset(_mgstc_rpclib_format)
    unset(_mgstc_rpclib_format_h)

    # rpclib's CMakeLists sets CMAKE_CXX_STANDARD globally; keep ours.
    set(_mgstc_saved_cxx_standard "${CMAKE_CXX_STANDARD}")
    add_subdirectory(
        "${rpclib_SOURCE_DIR}"
        "${rpclib_BINARY_DIR}"
        EXCLUDE_FROM_ALL)
    if(_mgstc_saved_cxx_standard STREQUAL "")
        unset(CMAKE_CXX_STANDARD)
    else()
        set(CMAKE_CXX_STANDARD "${_mgstc_saved_cxx_standard}")
    endif()
    unset(_mgstc_saved_cxx_standard)
endif()
if(TARGET rpc AND MSVC)
    target_compile_options(rpc PRIVATE /W0)
endif()

add_library(emu2149 STATIC third_party/emu2149/emu2149.c)
target_include_directories(
    emu2149 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/emu2149
)

add_library(emu2212 STATIC third_party/emu2212/emu2212.c)
target_include_directories(
    emu2212 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/emu2212
)

add_library(emu2413 STATIC third_party/emu2413/emu2413.c)
target_include_directories(
    emu2413 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/emu2413
)

if(MSVC)
    target_compile_options(emu2149 PRIVATE /W0)
    target_compile_options(emu2212 PRIVATE /W0)
    target_compile_options(emu2413 PRIVATE /W0)
else()
    target_compile_options(emu2149 PRIVATE -Wall -Wextra)
    target_compile_options(emu2212 PRIVATE -Wall -Wextra)
    target_compile_options(emu2413 PRIVATE -Wall -Wextra)
endif()

add_library(mgstc_engine STATIC
    src/engine/chip_rack.cpp
    src/engine/composite_timbre.cpp
    src/engine/composite_timbre_library.cpp
    src/engine/emulator_sound_output.cpp
    src/engine/engine_core.cpp
    src/engine/envelope_rate.cpp
    src/engine/envelope_sequence.cpp
    src/engine/mamidi_memo_sound_output.cpp
    src/engine/mamidi_register_map.cpp
    src/engine/mamidi_rpc_client.cpp
    src/engine/mgs_envelope_io.cpp
    src/engine/mgs_timbre_io.cpp
    src/engine/note_pitch.cpp
    src/engine/opll_envelope_trace.cpp
    src/engine/opll_patch.cpp
    src/engine/pc_keyboard.cpp
    src/engine/realtime_engine_host.cpp
    src/engine/register_mapper.cpp
    src/engine/runtime_session.cpp
    src/engine/scc_waveform.cpp
    src/engine/shared_state.cpp
    src/engine/sinc_rate_conv.cpp
    src/engine/timbre_library.cpp
    src/engine/track_runtime.cpp
    src/engine/voice_allocator.cpp
    src/engine/wave_import.cpp
)

find_package(Threads REQUIRED)
target_link_libraries(
    mgstc_engine
    PRIVATE
        emu2149
        emu2212
        emu2413
        rpc
        Threads::Threads
)
if(WIN32)
    target_link_libraries(mgstc_engine PRIVATE ws2_32)
endif()
# rpclib headers are only needed by mamidi_rpc_client.cpp; keep them
# out of the public interface of mgstc_engine.
target_include_directories(
    mgstc_engine
    SYSTEM PRIVATE
        $<TARGET_PROPERTY:rpc,INTERFACE_INCLUDE_DIRECTORIES>
)

target_include_directories(mgstc_engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/include
)

target_compile_features(mgstc_engine PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(mgstc_engine PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(mgstc_engine PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    add_library(mgstc_windows_audio STATIC
        src/app/windows/audio_import_bridge.cpp
        src/engine/wasapi_audio_sink.cpp
    )
    target_link_libraries(
        mgstc_windows_audio
        PUBLIC mgstc_engine
        PRIVATE ole32 uuid
    )
    target_include_directories(
        mgstc_windows_audio
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src/app/windows
    )
    target_compile_features(mgstc_windows_audio PUBLIC cxx_std_20)
    target_compile_definitions(
        mgstc_windows_audio PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    if(MSVC)
        target_compile_options(
            mgstc_windows_audio PRIVATE
                /W4 /permissive- /EHsc /utf-8
        )
    endif()

    if(WIN32)
        FetchContent_Declare(
            JUCE
            GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
            GIT_TAG 91ad83ae34a81e0833b1a2b0866f54846370ae53
            GIT_SHALLOW TRUE
            GIT_PROGRESS TRUE
        )
        FetchContent_MakeAvailable(JUCE)

        juce_add_gui_app(
            mgstc
            PRODUCT_NAME "MGS Tone Craft"
            COMPANY_NAME "MGS Tone Craft"
            BUNDLE_ID "com.mgstonecraft.mgstc"
        )
        set_target_properties(
            mgstc
            PROPERTIES
                OUTPUT_NAME "mgstc"
        )
        if(CMAKE_RUNTIME_OUTPUT_DIRECTORY)
            set_target_properties(
                mgstc
                PROPERTIES
                    RUNTIME_OUTPUT_DIRECTORY
                        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
                    RUNTIME_OUTPUT_DIRECTORY_DEBUG
                        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
                    RUNTIME_OUTPUT_DIRECTORY_RELEASE
                        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
                    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO
                        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
                    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL
                        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
            )
        endif()
        juce_add_binary_data(
            mgstc_binary_data
            HEADER_NAME BinaryData.h
            NAMESPACE BinaryData
            SOURCES
                assets/MGSTC_logo.png
                assets/mgstc_spin.gif
        )

        target_sources(
            mgstc PRIVATE
                src/app/juce/main.cpp
        )
        target_link_libraries(
            mgstc
            PRIVATE
                mgstc_binary_data
                mgstc_windows_audio
                juce::juce_audio_utils
                juce::juce_gui_extra
                gdiplus
                windowscodecs
        )
        target_compile_features(mgstc PRIVATE cxx_std_20)
        target_compile_definitions(
            mgstc
            PRIVATE
                JUCE_WEB_BROWSER=0
                JUCE_USE_CURL=0
                JUCE_MODAL_LOOPS_PERMITTED=1
                MGSTC_DOC_VERSION="${MGSTC_DOC_VERSION}"
        )
        if(MSVC)
            target_compile_options(
                mgstc PRIVATE /W4 /permissive- /EHsc /utf-8
            )
        endif()
    endif()
endif()

if(MGSTC_BUILD_TESTS)
    enable_testing()

    add_executable(mgstc_engine_tests
        tests/cpp/engine_tests.cpp
    )
    target_link_libraries(mgstc_engine_tests PRIVATE mgstc_engine)
    if(WIN32)
        target_link_libraries(
            mgstc_engine_tests PRIVATE mgstc_windows_audio
        )
    endif()
    # Mock chip_server in engine tests needs rpclib server headers.
    target_link_libraries(mgstc_engine_tests PRIVATE rpc)
    if(WIN32)
        target_link_libraries(mgstc_engine_tests PRIVATE ws2_32)
    endif()
    target_include_directories(
        mgstc_engine_tests
        SYSTEM PRIVATE
            $<TARGET_PROPERTY:rpc,INTERFACE_INCLUDE_DIRECTORIES>
    )
    target_compile_definitions(
        mgstc_engine_tests
        PRIVATE MGSTC_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_features(mgstc_engine_tests PRIVATE cxx_std_20)

    if(MSVC)
        target_compile_options(
            mgstc_engine_tests PRIVATE /W4 /permissive- /EHsc
        )
    else()
        target_compile_options(
            mgstc_engine_tests PRIVATE -Wall -Wextra -Wpedantic
        )
    endif()

    add_test(NAME mgstc_engine_tests COMMAND mgstc_engine_tests)

    add_executable(
        mgstc_chip_level_probe
        tests/cpp/chip_level_probe.cpp
    )
    target_link_libraries(
        mgstc_chip_level_probe PRIVATE mgstc_engine
    )
    target_compile_features(
        mgstc_chip_level_probe PRIVATE cxx_std_20
    )

    if(MSVC)
        target_compile_options(
            mgstc_chip_level_probe PRIVATE /W4 /permissive- /EHsc
        )
    else()
        target_compile_options(
            mgstc_chip_level_probe PRIVATE -Wall -Wextra -Wpedantic
        )
    endif()
endif()
