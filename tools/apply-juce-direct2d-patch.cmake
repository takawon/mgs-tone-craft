# SPDX-License-Identifier: AGPL-3.0-only
# Restore stock JUCE 8.0.15 Windows painting files (discards leftover 8.0.12
# overlay), then apply third_party/patches/juce/direct2d.patch idempotently.
# Pass -DSOURCE_DIR=... -DPATCH_FILE=...
# Windowing stays 8.0.15 so logicalBounds / userBounds keep Standalone on-screen.

if(NOT DEFINED SOURCE_DIR OR NOT DEFINED PATCH_FILE)
    message(FATAL_ERROR "SOURCE_DIR and PATCH_FILE are required")
endif()
if(NOT EXISTS "${PATCH_FILE}")
    message(FATAL_ERROR "JUCE Direct2D patch is missing: ${PATCH_FILE}")
endif()

execute_process(
    COMMAND
        git checkout --
            modules/juce_gui_basics/native/juce_Direct2DHwndContext_windows.cpp
            modules/juce_gui_basics/native/juce_Direct2DHwndContext_windows.h
            modules/juce_gui_basics/native/juce_Windowing_windows.cpp
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE restore_check
)
if(NOT restore_check EQUAL 0)
    message(FATAL_ERROR
        "Failed to restore JUCE 8.0.15 Windows painting files in ${SOURCE_DIR}")
endif()

execute_process(
    COMMAND git apply --check --whitespace=nowarn "${PATCH_FILE}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE apply_check
    ERROR_QUIET
)
if(apply_check EQUAL 0)
    execute_process(
        COMMAND git apply --whitespace=nowarn "${PATCH_FILE}"
        WORKING_DIRECTORY "${SOURCE_DIR}"
        COMMAND_ERROR_IS_FATAL ANY
    )
    return()
endif()

execute_process(
    COMMAND git apply --reverse --check --whitespace=nowarn "${PATCH_FILE}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE already_applied
    ERROR_QUIET
)
if(NOT already_applied EQUAL 0)
    message(FATAL_ERROR
        "direct2d.patch does not apply to this JUCE source, and is not already applied")
endif()
