# Copy JUCE 8.0.12 Windows painting sources onto FetchContent JUCE 8.0.15.
# This is not third_party/patches/juce/direct2d.patch and must not be mixed
# with that patch.

set(MGSTC_JUCE_WINDOWS_PAINT_OVERLAY_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/juce/windows-paint-8.0.12")

function(mgstc_apply_juce_8012_windows_paint_overlay juce_src_dir)
    if(NOT WIN32)
        return()
    endif()
    if(NOT EXISTS "${juce_src_dir}")
        message(FATAL_ERROR
            "JUCE source directory missing: ${juce_src_dir}")
    endif()

    set(_native "${juce_src_dir}/modules/juce_gui_basics/native")
    set(_files
        juce_Direct2DHwndContext_windows.cpp
        juce_Direct2DHwndContext_windows.h
        juce_Windowing_windows.cpp)

    foreach(_file IN LISTS _files)
        set(_src "${MGSTC_JUCE_WINDOWS_PAINT_OVERLAY_DIR}/${_file}")
        set(_dst "${_native}/${_file}")
        if(NOT EXISTS "${_src}")
            message(FATAL_ERROR
                "JUCE 8.0.12 Windows painting overlay missing: ${_src}")
        endif()
        configure_file("${_src}" "${_dst}" COPYONLY)
    endforeach()

    # JUCE 8.0.15 Displays reads logicalBounds / userBounds / physicalBounds.
    # 8.0.12 Windowing only wrote the deprecated totalArea / userArea, which
    # updateDeprecatedFields() then overwrites from empty logicalBounds.
    # This is a field-name adaptation, not a painting / Present change.
    set(_windowing "${_native}/juce_Windowing_windows.cpp")
    file(READ "${_windowing}" _windowing_text)
    set(_old_assign_crlf
        "        d.totalArea = D2DUtilities::toRectangle (monitor.totalAreaRect);\r\n        d.userArea  = D2DUtilities::toRectangle (monitor.workAreaRect);")
    set(_old_assign_lf
        "        d.totalArea = D2DUtilities::toRectangle (monitor.totalAreaRect);\n        d.userArea  = D2DUtilities::toRectangle (monitor.workAreaRect);")
    set(_new_assign_crlf
        "        d.physicalBounds = D2DUtilities::toRectangle (monitor.totalAreaRect);\r\n        d.logicalBounds = d.physicalBounds.toFloat();\r\n        d.userBounds  = D2DUtilities::toRectangle (monitor.workAreaRect).toFloat();")
    set(_new_assign_lf
        "        d.physicalBounds = D2DUtilities::toRectangle (monitor.totalAreaRect);\n        d.logicalBounds = d.physicalBounds.toFloat();\n        d.userBounds  = D2DUtilities::toRectangle (monitor.workAreaRect).toFloat();")
    string(FIND "${_windowing_text}" "${_old_assign_crlf}" _found_assign)
    if(_found_assign GREATER_EQUAL 0)
        string(REPLACE "${_old_assign_crlf}" "${_new_assign_crlf}"
            _windowing_text "${_windowing_text}")
    else()
        string(FIND "${_windowing_text}" "${_old_assign_lf}" _found_assign)
        if(_found_assign LESS 0)
            message(FATAL_ERROR
                "JUCE 8.0.12 overlay: findDisplays assignment block not found")
        endif()
        string(REPLACE "${_old_assign_lf}" "${_new_assign_lf}"
            _windowing_text "${_windowing_text}")
    endif()
    string(REPLACE
        "            d.totalArea /= masterScale;\r\n            d.userArea  /= masterScale;"
        "            d.logicalBounds /= masterScale;\r\n            d.userBounds    /= masterScale;"
        _windowing_text "${_windowing_text}")
    string(REPLACE
        "            d.totalArea /= masterScale;\n            d.userArea  /= masterScale;"
        "            d.logicalBounds /= masterScale;\n            d.userBounds    /= masterScale;"
        _windowing_text "${_windowing_text}")
    file(WRITE "${_windowing}" "${_windowing_text}")

    message(STATUS
        "MGSTC: overlaid JUCE 8.0.12 Windows painting files onto ${juce_src_dir}")
    message(STATUS
        "MGSTC: adapted overlay findDisplays to JUCE 8.0.15 Displays fields")
endfunction()
