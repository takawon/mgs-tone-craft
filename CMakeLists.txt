cmake_minimum_required(VERSION 3.25)

include(FetchContent)

# MSVC needs VsDevCmd (INCLUDE/LIB/PATH). Plain PowerShell + cmake --build
# otherwise fails with misleading C1083 on <array>. Check before project() so
# configure fails fast; build.ps1 imports VsDevCmd first.
if(WIN32)
    if(NOT DEFINED ENV{INCLUDE} OR "$ENV{INCLUDE}" STREQUAL "")
        message(FATAL_ERROR
            "Visual Studio C++ environment is not active (INCLUDE is unset).\n"
            "Run: .\\tools\\build.cmd\n"
            "Or open Developer PowerShell for VS before running CMake.")
    endif()
endif()

project(
    MgsToneCraft
    VERSION 0.1.0
    DESCRIPTION "MGSDRV-compatible compound timbre editor"
    LANGUAGES C CXX
)

option(MGSTC_BUILD_TESTS "Build engine tests" ON)
option(MGSTC_BUILD_BENCHMARKS "Build explicit performance benchmarks" OFF)

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

add_library(sqlite3 STATIC third_party/sqlite/sqlite3.c)
target_include_directories(
    sqlite3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite
)
target_compile_definitions(
    sqlite3
    PRIVATE
        SQLITE_DQS=0
        SQLITE_OMIT_LOAD_EXTENSION
        SQLITE_THREADSAFE=1
)

if(MSVC)
    target_compile_options(emu2149 PRIVATE /W0)
    target_compile_options(emu2212 PRIVATE /W0)
    target_compile_options(emu2413 PRIVATE /W0)
    target_compile_options(sqlite3 PRIVATE /W0)
else()
    target_compile_options(emu2149 PRIVATE -Wall -Wextra)
    target_compile_options(emu2212 PRIVATE -Wall -Wextra)
    target_compile_options(emu2413 PRIVATE -Wall -Wextra)
    target_compile_options(sqlite3 PRIVATE -w)
endif()

add_library(mgstc_engine STATIC
    src/engine/chip_rack.cpp
    src/engine/composite_envelope_compile.cpp
    src/engine/composite_program_compiler.cpp
    src/engine/composite_timbre.cpp
    src/engine/composite_timbre_library.cpp
    src/engine/emulator_sound_output.cpp
    src/engine/engine_core.cpp
    src/engine/gzip_inflate.cpp
    src/engine/envelope_rate.cpp
    src/engine/envelope_sequence.cpp
    src/engine/mamidi_memo_sound_output.cpp
    src/engine/mamidi_register_map.cpp
    src/engine/mamidi_rpc_client.cpp
    src/engine/mgs_composite_io.cpp
    src/engine/mgs_envelope_io.cpp
    src/engine/mgs_timbre_io.cpp
    src/engine/note_pitch.cpp
    src/engine/opll_envelope_trace.cpp
    src/engine/opll_patch.cpp
    src/engine/opll_register_auto.cpp
    src/engine/pc_keyboard.cpp
    src/engine/realtime_engine_host.cpp
    src/engine/register_mapper.cpp
    src/engine/runtime_session.cpp
    src/engine/scc_waveform.cpp
    src/engine/shared_state.cpp
    src/engine/sinc_rate_conv.cpp
    src/engine/stereo_sample_rate_converter.cpp
    src/engine/timbre_library.cpp
    src/engine/timbre_tags.cpp
    src/engine/tone_library_store.cpp
    src/engine/tone_import.cpp
    src/engine/tone_import_mgs.cpp
    src/engine/tone_import_sng.cpp
    src/engine/tone_import_vcd.cpp
    src/engine/tone_import_vgm.cpp
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
        sqlite3
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
    target_compile_options(
        mgstc_engine PRIVATE /W4 /permissive- /EHsc /utf-8)
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
                assets/MGSTC_icon.rc
                src/app/juce/asio_audio_sink.cpp
                src/app/juce/asio_audio_sink.hpp
                src/app/juce/spectrogram_window.cpp
                src/app/juce/spectrogram_window.hpp
                src/app/juce/ui_hang_watchdog.cpp
                src/app/juce/ui_hang_watchdog.hpp
                src/app/juce/ui_layout.cpp
                src/app/juce/ui_paint.cpp
                src/app/juce/rate_envelope_trace.cpp
                src/app/juce/rate_envelope_trace.hpp
                src/app/juce/composite_timeline.cpp
                src/app/juce/composite_timeline.hpp
                src/app/juce/tone_library_session.cpp
                src/app/juce/tone_library_session.hpp
                src/app/juce/main.cpp
                src/app/juce/mgstc_editor_view.cpp
        )
        target_link_libraries(
            mgstc
            PRIVATE
                mgstc_binary_data
                mgstc_windows_audio
                juce::juce_audio_utils
                juce::juce_dsp
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
                JUCE_ASIO=1
                JUCE_MODAL_LOOPS_PERMITTED=1
                MGSTC_DOC_VERSION="${MGSTC_DOC_VERSION}"
        )
        if(MSVC)
            target_compile_options(
                mgstc PRIVATE /W4 /permissive- /EHsc /utf-8
            )
        endif()

        # VST3 Instrument. Do not link WASAPI/ASIO/WinMM hosts.
        set(MGSTC_VST3_DIAG_MODE "OFF" CACHE STRING
            "VST3 Stage D.2 diagnostic: OFF, NON48_NULL, NON48_NULL_LAT0, NON48_ENGINE_ONLY, NON48_SRC_ZERO, NORMAL_NO_LATENCY_NOTIFY"
            FORCE)
        set_property(
            CACHE MGSTC_VST3_DIAG_MODE
            PROPERTY STRINGS
                OFF
                NON48_NULL
                NON48_NULL_LAT0
                NON48_ENGINE_ONLY
                NON48_SRC_ZERO
                NORMAL_NO_LATENCY_NOTIFY)

        function(mgstc_vst3_diag_mode_id out_var mode)
            if(mode STREQUAL "NON48_NULL")
                set(${out_var} 1 PARENT_SCOPE)
            elseif(mode STREQUAL "NON48_NULL_LAT0")
                set(${out_var} 2 PARENT_SCOPE)
            elseif(mode STREQUAL "NON48_ENGINE_ONLY")
                set(${out_var} 3 PARENT_SCOPE)
            elseif(mode STREQUAL "NON48_SRC_ZERO")
                set(${out_var} 4 PARENT_SCOPE)
            elseif(mode STREQUAL "NORMAL_NO_LATENCY_NOTIFY")
                set(${out_var} 5 PARENT_SCOPE)
            else()
                set(${out_var} 0 PARENT_SCOPE)
            endif()
        endfunction()

        function(mgstc_configure_vst3_target target_name diag_mode_id)
            target_sources(
                ${target_name} PRIVATE
                    src/plugin/vst3/host_engine_timeline.hpp
                    src/plugin/vst3/layer_delay_scheduler.hpp
                    src/plugin/vst3/layer_delay_scheduler.cpp
                    src/plugin/vst3/plugin_processor.hpp
                    src/plugin/vst3/plugin_processor.cpp
                    src/plugin/vst3/plugin_state.hpp
                    src/plugin/vst3/plugin_state.cpp
                    src/plugin/vst3/plugin_editor_context.hpp
                    src/plugin/vst3/plugin_editor.hpp
                    src/plugin/vst3/plugin_editor.cpp
                    src/plugin/vst3/vst3_diagnostic.hpp
                    src/app/juce/mgstc_editor_view.cpp
                    src/app/juce/composite_timeline.cpp
                    src/app/juce/tone_library_session.cpp
                    src/app/juce/ui_hang_watchdog.cpp
                    src/app/juce/ui_layout.cpp
                    src/app/juce/ui_paint.cpp
                    src/app/juce/rate_envelope_trace.cpp
                    src/app/windows/audio_import_bridge.cpp
            )
            target_include_directories(
                ${target_name} PRIVATE
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/plugin/vst3
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/juce
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/windows
            )
            target_link_libraries(
                ${target_name}
                PRIVATE
                    mgstc_engine
                    mgstc_binary_data
                    juce::juce_audio_processors
                    juce::juce_audio_utils
                    juce::juce_gui_extra
                    gdiplus
                    windowscodecs
            )
            target_compile_features(${target_name} PRIVATE cxx_std_20)
            target_compile_definitions(
                ${target_name}
                PUBLIC
                    JUCE_WEB_BROWSER=0
                    JUCE_USE_CURL=0
                    JUCE_VST3_CAN_REPLACE_VST2=0
                    JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS=0
                    JUCE_MODAL_LOOPS_PERMITTED=1
                PRIVATE
                    MGSTC_DOC_VERSION="${MGSTC_DOC_VERSION}"
                    MGSTC_VST3_DIAG_MODE=${diag_mode_id}
            )
            if(MSVC)
                target_compile_options(
                    ${target_name} PRIVATE /W4 /permissive- /EHsc /utf-8
                )
            endif()
        endfunction()

        juce_add_plugin(
            mgstc_vst3
            COMPANY_NAME "MGS Tone Craft"
            BUNDLE_ID "com.mgstonecraft.mgstc.instrument"
            IS_SYNTH TRUE
            NEEDS_MIDI_INPUT TRUE
            NEEDS_MIDI_OUTPUT FALSE
            IS_MIDI_EFFECT FALSE
            EDITOR_WANTS_KEYBOARD_FOCUS FALSE
            COPY_PLUGIN_AFTER_BUILD FALSE
            PLUGIN_MANUFACTURER_CODE Mgsc
            PLUGIN_CODE Mgti
            FORMATS VST3
            PRODUCT_NAME "MGS Tone Craft"
            PLUGIN_NAME "MGS Tone Craft"
            DESCRIPTION "MGSDRV compound timbre instrument"
            VST3_CATEGORIES "Instrument Synth"
            PLUGIN_CHANNEL_CONFIGURATIONS "{0, 2}"
        )
        mgstc_configure_vst3_target(mgstc_vst3 0)

        mgstc_vst3_diag_mode_id(_mgstc_vst3_diag_id "${MGSTC_VST3_DIAG_MODE}")
        if(_mgstc_vst3_diag_id GREATER 0)
            juce_add_plugin(
                mgstc_vst3_diag
                COMPANY_NAME "MGS Tone Craft"
                BUNDLE_ID "com.mgstonecraft.mgstc.instrument"
                IS_SYNTH TRUE
                NEEDS_MIDI_INPUT TRUE
                NEEDS_MIDI_OUTPUT FALSE
                IS_MIDI_EFFECT FALSE
                EDITOR_WANTS_KEYBOARD_FOCUS FALSE
                COPY_PLUGIN_AFTER_BUILD FALSE
                PLUGIN_MANUFACTURER_CODE Mgsc
                PLUGIN_CODE Mgti
                FORMATS VST3
                PRODUCT_NAME "MGS Tone Craft"
                PLUGIN_NAME "MGS Tone Craft"
                DESCRIPTION "MGSTC Stage D.2 diagnostic instrument"
                VST3_CATEGORIES "Instrument Synth"
                PLUGIN_CHANNEL_CONFIGURATIONS "{0, 2}"
            )
            mgstc_configure_vst3_target(
                mgstc_vst3_diag ${_mgstc_vst3_diag_id}
            )
        endif()
    endif()
endif()

if(MGSTC_BUILD_TESTS)
    enable_testing()

    add_executable(mgstc_engine_tests
        tests/cpp/engine_tests.cpp
    )
    target_link_libraries(mgstc_engine_tests PRIVATE mgstc_engine sqlite3)
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
    set(
        MGSTC_PRIVATE_MGSDRV_FIXTURE
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mgsdrv_selftest/TEST_BOUNDARY.VGM"
    )
    if(EXISTS "${MGSTC_PRIVATE_MGSDRV_FIXTURE}")
        target_compile_definitions(
            mgstc_engine_tests PRIVATE MGSTC_HAS_PRIVATE_MGSDRV_FIXTURE=1
        )
    else()
        target_compile_definitions(
            mgstc_engine_tests PRIVATE MGSTC_HAS_PRIVATE_MGSDRV_FIXTURE=0
        )
    endif()
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

    add_executable(mgstc_opll_envelope_edit_tests tests/cpp/opll_envelope_edit_tests.cpp)
    target_link_libraries(mgstc_opll_envelope_edit_tests PRIVATE mgstc_engine)
    target_compile_features(mgstc_opll_envelope_edit_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(mgstc_opll_envelope_edit_tests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(mgstc_opll_envelope_edit_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME mgstc_opll_envelope_edit_tests COMMAND mgstc_opll_envelope_edit_tests)

    add_executable(
        mgstc_spectrogram_emphasis_tests
        tests/cpp/spectrogram_emphasis_tests.cpp
    )
    target_compile_features(
        mgstc_spectrogram_emphasis_tests PRIVATE cxx_std_20
    )
    if(MSVC)
        target_compile_options(
            mgstc_spectrogram_emphasis_tests PRIVATE /W4 /permissive- /EHsc
        )
    else()
        target_compile_options(
            mgstc_spectrogram_emphasis_tests PRIVATE -Wall -Wextra -Wpedantic
        )
    endif()
    add_test(
        NAME mgstc_spectrogram_emphasis_tests
        COMMAND mgstc_spectrogram_emphasis_tests
    )

    if(WIN32 AND TARGET juce::juce_gui_basics)
        add_executable(mgstc_spectrum_tests tests/cpp/spectrum_tests.cpp src/app/juce/ui_layout.cpp)
        target_link_libraries(mgstc_spectrum_tests PRIVATE mgstc_engine juce::juce_dsp juce::juce_gui_extra)
        target_compile_features(mgstc_spectrum_tests PRIVATE cxx_std_20)
        target_compile_definitions(mgstc_spectrum_tests PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
        if(MSVC)
            target_compile_options(mgstc_spectrum_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
        endif()
        add_test(NAME mgstc_spectrum_tests COMMAND mgstc_spectrum_tests)

        add_executable(mgstc_opll_envelope_graph_tests tests/cpp/opll_envelope_graph_tests.cpp)
        target_link_libraries(mgstc_opll_envelope_graph_tests PRIVATE mgstc_engine juce::juce_gui_basics)
        target_compile_features(mgstc_opll_envelope_graph_tests PRIVATE cxx_std_20)
        target_compile_definitions(mgstc_opll_envelope_graph_tests PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
        if(MSVC)
            target_compile_options(mgstc_opll_envelope_graph_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
        endif()
        add_test(NAME mgstc_opll_envelope_graph_tests COMMAND mgstc_opll_envelope_graph_tests)
    endif()

    add_executable(
        mgstc_composite_envelope_compile_tests
        tests/cpp/composite_envelope_compile_tests.cpp)
    target_link_libraries(
        mgstc_composite_envelope_compile_tests
        PRIVATE mgstc_engine)
    target_compile_features(
        mgstc_composite_envelope_compile_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(
            mgstc_composite_envelope_compile_tests PRIVATE
                /W4 /permissive- /EHsc /utf-8)
    else()
        target_compile_options(
            mgstc_composite_envelope_compile_tests PRIVATE
                -Wall -Wextra -Wpedantic)
    endif()
    add_test(
        NAME mgstc_composite_envelope_compile_tests
        COMMAND mgstc_composite_envelope_compile_tests)

    add_executable(
        mgstc_realtime_engine_host_tests
        tests/cpp/realtime_engine_host_tests.cpp)
    target_link_libraries(
        mgstc_realtime_engine_host_tests
        PRIVATE mgstc_engine)
    target_compile_features(
        mgstc_realtime_engine_host_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(
            mgstc_realtime_engine_host_tests PRIVATE
                /W4 /permissive- /EHsc /utf-8)
    else()
        target_compile_options(
            mgstc_realtime_engine_host_tests PRIVATE
                -Wall -Wextra -Wpedantic)
    endif()
    add_test(
        NAME mgstc_realtime_engine_host_tests
        COMMAND mgstc_realtime_engine_host_tests)

    add_executable(
        mgstc_layer_delay_scheduler_tests
        tests/cpp/layer_delay_scheduler_tests.cpp
        src/plugin/vst3/layer_delay_scheduler.cpp)
    target_include_directories(
        mgstc_layer_delay_scheduler_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/plugin/vst3)
    target_compile_features(
        mgstc_layer_delay_scheduler_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(
            mgstc_layer_delay_scheduler_tests PRIVATE
                /W4 /permissive- /EHsc /utf-8)
    else()
        target_compile_options(
            mgstc_layer_delay_scheduler_tests PRIVATE
                -Wall -Wextra -Wpedantic)
    endif()
    add_test(
        NAME mgstc_layer_delay_scheduler_tests
        COMMAND mgstc_layer_delay_scheduler_tests)

    if(WIN32 AND TARGET juce::juce_audio_processors)
        add_executable(
            mgstc_vst3_processor_tests
            tests/cpp/vst3_processor_tests.cpp
            src/plugin/vst3/layer_delay_scheduler.cpp
            src/plugin/vst3/plugin_processor.cpp
            src/plugin/vst3/plugin_state.cpp
            src/plugin/vst3/plugin_editor.cpp
            src/app/juce/mgstc_editor_view.cpp
            src/app/juce/composite_timeline.cpp
            src/app/juce/tone_library_session.cpp
            src/app/juce/ui_hang_watchdog.cpp
            src/app/juce/ui_layout.cpp
            src/app/juce/ui_paint.cpp
            src/app/juce/rate_envelope_trace.cpp
            src/app/windows/audio_import_bridge.cpp)
        target_include_directories(
            mgstc_vst3_processor_tests PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src/plugin/vst3
                ${CMAKE_CURRENT_SOURCE_DIR}/src/app/juce
                ${CMAKE_CURRENT_SOURCE_DIR}/src/app/windows)
        target_link_libraries(
            mgstc_vst3_processor_tests
            PRIVATE
                mgstc_engine
                mgstc_binary_data
                juce::juce_audio_processors
                juce::juce_audio_utils
                juce::juce_gui_extra
                gdiplus
                windowscodecs)
        target_compile_features(
            mgstc_vst3_processor_tests PRIVATE cxx_std_20)
        target_compile_definitions(
            mgstc_vst3_processor_tests
            PRIVATE
                JUCE_WEB_BROWSER=0
                JUCE_USE_CURL=0
                JUCE_VST3_CAN_REPLACE_VST2=0
                JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS=0
                JUCE_MODAL_LOOPS_PERMITTED=1
                MGSTC_VST3_DIAG_MODE=0)
        if(MSVC)
            target_compile_options(
                mgstc_vst3_processor_tests PRIVATE
                    /W4 /permissive- /EHsc /utf-8)
        endif()
        add_test(
            NAME mgstc_vst3_processor_tests
            COMMAND mgstc_vst3_processor_tests)

        add_executable(
            mgstc_vst3_diag_null_tests
            tests/cpp/vst3_diag_null_tests.cpp
            src/plugin/vst3/layer_delay_scheduler.cpp
            src/plugin/vst3/plugin_processor.cpp
            src/plugin/vst3/plugin_state.cpp
            src/plugin/vst3/plugin_editor.cpp
            src/app/juce/mgstc_editor_view.cpp
            src/app/juce/composite_timeline.cpp
            src/app/juce/tone_library_session.cpp
            src/app/juce/ui_hang_watchdog.cpp
            src/app/juce/ui_layout.cpp
            src/app/juce/ui_paint.cpp
            src/app/juce/rate_envelope_trace.cpp
            src/app/windows/audio_import_bridge.cpp)
        target_include_directories(
            mgstc_vst3_diag_null_tests PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src/plugin/vst3
                ${CMAKE_CURRENT_SOURCE_DIR}/src/app/juce
                ${CMAKE_CURRENT_SOURCE_DIR}/src/app/windows)
        target_link_libraries(
            mgstc_vst3_diag_null_tests
            PRIVATE
                mgstc_engine
                mgstc_binary_data
                juce::juce_audio_processors
                juce::juce_audio_utils
                juce::juce_gui_extra
                gdiplus
                windowscodecs)
        target_compile_features(
            mgstc_vst3_diag_null_tests PRIVATE cxx_std_20)
        target_compile_definitions(
            mgstc_vst3_diag_null_tests
            PRIVATE
                JUCE_WEB_BROWSER=0
                JUCE_USE_CURL=0
                JUCE_VST3_CAN_REPLACE_VST2=0
                JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS=0
                JUCE_MODAL_LOOPS_PERMITTED=1
                MGSTC_VST3_DIAG_MODE=1)
        if(MSVC)
            target_compile_options(
                mgstc_vst3_diag_null_tests PRIVATE
                    /W4 /permissive- /EHsc /utf-8)
        endif()
        add_test(
            NAME mgstc_vst3_diag_null_tests
            COMMAND mgstc_vst3_diag_null_tests)

        add_executable(
            mgstc_vst3_diag_lat0_tests
            tests/cpp/vst3_diag_lat0_tests.cpp
            src/plugin/vst3/layer_delay_scheduler.cpp
            src/plugin/vst3/plugin_processor.cpp
            src/plugin/vst3/plugin_state.cpp
            src/plugin/vst3/plugin_editor.cpp
            src/app/juce/mgstc_editor_view.cpp
            src/app/juce/composite_timeline.cpp
            src/app/juce/tone_library_session.cpp
            src/app/juce/ui_hang_watchdog.cpp
            src/app/juce/ui_layout.cpp
            src/app/juce/ui_paint.cpp
            src/app/juce/rate_envelope_trace.cpp
            src/app/windows/audio_import_bridge.cpp)
        target_include_directories(
            mgstc_vst3_diag_lat0_tests PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src/plugin/vst3
                ${CMAKE_CURRENT_SOURCE_DIR}/src/app/juce
                ${CMAKE_CURRENT_SOURCE_DIR}/src/app/windows)
        target_link_libraries(
            mgstc_vst3_diag_lat0_tests
            PRIVATE
                mgstc_engine
                mgstc_binary_data
                juce::juce_audio_processors
                juce::juce_audio_utils
                juce::juce_gui_extra
                gdiplus
                windowscodecs)
        target_compile_features(
            mgstc_vst3_diag_lat0_tests PRIVATE cxx_std_20)
        target_compile_definitions(
            mgstc_vst3_diag_lat0_tests
            PRIVATE
                JUCE_WEB_BROWSER=0
                JUCE_USE_CURL=0
                JUCE_VST3_CAN_REPLACE_VST2=0
                JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS=0
                JUCE_MODAL_LOOPS_PERMITTED=1
                MGSTC_VST3_DIAG_MODE=2)
        if(MSVC)
            target_compile_options(
                mgstc_vst3_diag_lat0_tests PRIVATE
                    /W4 /permissive- /EHsc /utf-8)
        endif()
        add_test(
            NAME mgstc_vst3_diag_lat0_tests
            COMMAND mgstc_vst3_diag_lat0_tests)
    endif()

    add_executable(
        mgstc_stereo_sample_rate_converter_tests
        tests/cpp/stereo_sample_rate_converter_tests.cpp
    )
    target_link_libraries(
        mgstc_stereo_sample_rate_converter_tests PRIVATE mgstc_engine
    )
    target_compile_features(
        mgstc_stereo_sample_rate_converter_tests PRIVATE cxx_std_20
    )
    if(MSVC)
        target_compile_options(
            mgstc_stereo_sample_rate_converter_tests PRIVATE
                /W4 /permissive- /EHsc
        )
    else()
        target_compile_options(
            mgstc_stereo_sample_rate_converter_tests PRIVATE
                -Wall -Wextra -Wpedantic
        )
    endif()
    add_test(
        NAME mgstc_stereo_sample_rate_converter_tests
        COMMAND mgstc_stereo_sample_rate_converter_tests
    )

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

if(MGSTC_BUILD_BENCHMARKS)
    add_executable(
        mgstc_wave_import_benchmark
        tests/cpp/wave_import_benchmark.cpp
    )
    target_link_libraries(
        mgstc_wave_import_benchmark PRIVATE mgstc_engine
    )
    target_compile_features(
        mgstc_wave_import_benchmark PRIVATE cxx_std_20
    )
    if(MSVC)
        target_compile_options(
            mgstc_wave_import_benchmark PRIVATE /W4 /permissive- /EHsc
        )
    else()
        target_compile_options(
            mgstc_wave_import_benchmark PRIVATE -Wall -Wextra -Wpedantic
        )
    endif()
endif()

# CMAKE_*_COMPILER_LAUNCHER as a CMake list breaks JUCE juceaide
# sub-configure. A custom target plus add_dependencies serializes the
# INCLUDE check before any compile, including third_party / JUCE objects
# that would otherwise race and fail with C1083.
if(WIN32 AND MSVC)
    set(_MGSTC_CHECK_MSVC_ENV_PS1
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/check-msvc-env.ps1")
    set(_MGSTC_POWERSHELL
        "$ENV{WINDIR}/System32/WindowsPowerShell/v1.0/powershell.exe")

    add_custom_target(
        mgstc_msvc_env_check
        COMMAND
            "${_MGSTC_POWERSHELL}"
            -NoProfile
            -ExecutionPolicy
            Bypass
            -File
            "${_MGSTC_CHECK_MSVC_ENV_PS1}"
        VERBATIM
        COMMENT "Checking Visual Studio C++ environment (INCLUDE)"
    )

    function(mgstc_attach_msvc_env_check directory)
        get_property(_mgstc_targets DIRECTORY "${directory}"
            PROPERTY BUILDSYSTEM_TARGETS)
        foreach(_mgstc_target IN LISTS _mgstc_targets)
            if(_mgstc_target STREQUAL "mgstc_msvc_env_check")
                continue()
            endif()
            if(NOT TARGET "${_mgstc_target}")
                continue()
            endif()
            get_target_property(
                _mgstc_aliased "${_mgstc_target}" ALIASED_TARGET)
            if(_mgstc_aliased)
                continue()
            endif()
            get_target_property(_mgstc_imported "${_mgstc_target}" IMPORTED)
            if(_mgstc_imported)
                continue()
            endif()
            get_target_property(_mgstc_type "${_mgstc_target}" TYPE)
            if(_mgstc_type STREQUAL "INTERFACE_LIBRARY")
                continue()
            endif()
            add_dependencies("${_mgstc_target}" mgstc_msvc_env_check)
        endforeach()
        get_property(_mgstc_subdirs DIRECTORY "${directory}"
            PROPERTY SUBDIRECTORIES)
        foreach(_mgstc_subdir IN LISTS _mgstc_subdirs)
            mgstc_attach_msvc_env_check("${_mgstc_subdir}")
        endforeach()
    endfunction()

    mgstc_attach_msvc_env_check("${CMAKE_CURRENT_SOURCE_DIR}")

    # CTest records absolute exe paths at generate time. Keep every
    # executable next to mgstc.exe when build.ps1 sets this directory.
    if(CMAKE_RUNTIME_OUTPUT_DIRECTORY)
        get_property(_mgstc_exe_targets DIRECTORY
            PROPERTY BUILDSYSTEM_TARGETS)
        foreach(_mgstc_target IN LISTS _mgstc_exe_targets)
            if(NOT TARGET "${_mgstc_target}")
                continue()
            endif()
            get_target_property(_mgstc_type "${_mgstc_target}" TYPE)
            if(NOT _mgstc_type STREQUAL "EXECUTABLE")
                continue()
            endif()
            set_target_properties(
                "${_mgstc_target}"
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
        endforeach()
    endif()
endif()
