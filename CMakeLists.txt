cmake_minimum_required(VERSION 3.25)

project(
    MgsToneCraft
    VERSION 0.1.0
    DESCRIPTION "MGSDRV-compatible compound timbre editor"
    LANGUAGES C CXX
)

option(MGSTC_BUILD_TESTS "Build engine tests" ON)

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
    src/engine/engine_core.cpp
    src/engine/envelope_rate.cpp
    src/engine/envelope_sequence.cpp
    src/engine/mgs_timbre_io.cpp
    src/engine/note_pitch.cpp
    src/engine/opll_envelope_trace.cpp
    src/engine/opll_patch.cpp
    src/engine/realtime_engine_host.cpp
    src/engine/register_mapper.cpp
    src/engine/runtime_session.cpp
    src/engine/scc_waveform.cpp
    src/engine/shared_state.cpp
    src/engine/timbre_library.cpp
    src/engine/track_runtime.cpp
    src/engine/wave_import.cpp
)

target_link_libraries(
    mgstc_engine PRIVATE emu2149 emu2212 emu2413
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
        src/engine/wasapi_audio_sink.cpp
    )
    target_link_libraries(
        mgstc_windows_audio
        PUBLIC mgstc_engine
        PRIVATE ole32 uuid
    )
    target_include_directories(
        mgstc_windows_audio
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/include
    )
    target_compile_features(mgstc_windows_audio PUBLIC cxx_std_20)
    target_compile_definitions(
        mgstc_windows_audio PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    if(MSVC)
        target_compile_options(
            mgstc_windows_audio PRIVATE /W4 /permissive- /EHsc
        )
    endif()

    add_executable(mgstc WIN32
        src/app/windows/main.cpp
    )
    target_link_libraries(
        mgstc
        PRIVATE mgstc_windows_audio comctl32 comdlg32 gdi32
                msimg32 ole32 shell32 user32 windowscodecs winmm
    )
    target_compile_features(mgstc PRIVATE cxx_std_20)
    target_compile_definitions(
        mgstc PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    )
    if(MSVC)
        target_compile_options(
            mgstc PRIVATE /W4 /permissive- /EHsc /utf-8
        )
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
