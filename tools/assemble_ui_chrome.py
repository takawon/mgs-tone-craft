#!/usr/bin/env python3
"""Assemble UI chrome files and patch main.cpp (one-shot migration helper)."""
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
JUCE = ROOT / "src" / "app" / "juce"
MAIN = JUCE / "main.cpp"

SPDX = "// SPDX-License-Identifier: AGPL-3.0-only\n\n"

def read_extract(name: str) -> str:
    return (JUCE / f"_extract_{name}.txt").read_text(encoding="utf-8")


def write(path: Path, content: str) -> None:
    path.write_text(content, encoding="utf-8")
    print(f"wrote {path.name}")


def main() -> None:
    ui_scale_body = read_extract("ui_scale_body").replace(
        "applicationDataDirectory()",
        "mgstcApplicationDataDirectory()",
    )
    ui_fonts_body = read_extract("ui_fonts_body")
    ui_layout_body = read_extract("ui_layout_body")
    # Drop static init; lives in ui_layout.cpp
    ui_layout_body = ui_layout_body.replace(
        "struct UiLayoutInit final {\n"
        "    UiLayoutInit() { applyScale(1.0F); }\n"
        "};\n"
        "inline UiLayoutInit ui_layout_init{};\n",
        "",
    )

    write(
        JUCE / "ui_chrome_constants.hpp",
        SPDX
        + "#pragma once\n\n"
        + "#include <juce_gui_basics/juce_gui_basics.h>\n\n"
        + "// Editor Open/Paste/Settings icon over; shared UI hover accent.\n"
        + "constexpr juce::uint32 kUiHoverAccent = 0xFF53E3A6;\n",
    )

    write(
        JUCE / "ui_paths.hpp",
        SPDX
        + "#pragma once\n\n"
        + "#include <juce_core/juce_core.h>\n\n"
        + "[[nodiscard]] inline juce::File mgstcApplicationDataDirectory() {\n"
        + "    return juce::File::getSpecialLocation(\n"
        + "               juce::File::userApplicationDataDirectory)\n"
        + "        .getChildFile(\"MgsToneCraft\");\n"
        + "}\n",
    )

    write(
        JUCE / "ui_scale.hpp",
        SPDX
        + "#pragma once\n\n"
        + "#include <array>\n"
        + "#include <cmath>\n"
        + "#include <functional>\n"
        + "#include <optional>\n"
        + "#include <vector>\n\n"
        + "#define WIN32_LEAN_AND_MEAN\n"
        + "#define NOMINMAX\n"
        + "#include <windows.h>\n\n"
        + "#include <juce_gui_basics/juce_gui_basics.h>\n\n"
        + "#include \"ui_paths.hpp\"\n\n"
        + "namespace UiLayout {\n"
        + "void applyScale(float factor);\n"
        + "}\n\n"
        + "// Discrete UI scale (View setting + optional per-editor session override).\n"
        + ui_scale_body,
    )

    write(
        JUCE / "ui_fonts.hpp",
        SPDX
        + "#pragma once\n\n"
        + "#define WIN32_LEAN_AND_MEAN\n"
        + "#define NOMINMAX\n"
        + "#include <windows.h>\n\n"
        + "#include <juce_gui_basics/juce_gui_basics.h>\n\n"
        + "#include \"ui_scale.hpp\"\n\n"
        + ui_fonts_body,
    )

    write(
        JUCE / "ui_layout.hpp",
        SPDX
        + "#pragma once\n\n"
        + "#include <juce_gui_basics/juce_gui_basics.h>\n\n"
        + "#include \"ui_fonts.hpp\"\n"
        + "#include \"ui_scale.hpp\"\n\n"
        + ui_layout_body,
    )

    write(
        JUCE / "ui_layout.cpp",
        SPDX
        + '#include "ui_layout.hpp"\n\n'
        + "namespace {\n"
        + "struct UiLayoutInit final {\n"
        + "    UiLayoutInit() { UiLayout::applyScale(1.0F); }\n"
        + "};\n"
        + "const UiLayoutInit ui_layout_init{};\n"
        + "} // namespace\n\n"
        + read_extract("apply_layout_metrics"),
    )

    write(
        JUCE / "switch_look_and_feel.hpp",
        SPDX
        + "#pragma once\n\n"
        + "#include <juce_gui_basics/juce_gui_basics.h>\n\n"
        + "#include \"ui_chrome_constants.hpp\"\n"
        + "#include \"ui_fonts.hpp\"\n"
        + "#include \"ui_layout.hpp\"\n"
        + "#include \"ui_scale.hpp\"\n\n"
        + read_extract("switch_laf_body"),
    )

    write(
        JUCE / "mgstc_look_and_feel.hpp",
        SPDX
        + "#pragma once\n\n"
        + "#include <juce_gui_basics/juce_gui_basics.h>\n\n"
        + "#include \"ui_chrome_constants.hpp\"\n"
        + "#include \"ui_fonts.hpp\"\n"
        + "#include \"ui_layout.hpp\"\n\n"
        + read_extract("mgstc_laf_body"),
    )

    write(
        JUCE / "ui_paint.hpp",
        SPDX
        + "#pragma once\n\n"
        + "#include <juce_gui_basics/juce_gui_basics.h>\n\n"
        + "#include \"ui_layout.hpp\"\n\n"
        + "void clampWindowToDisplayWorkArea(juce::ResizableWindow& window);\n\n"
        + "void applyScaledContentSize(\n"
        + "    juce::Component& content,\n"
        + "    int preferred_w,\n"
        + "    int preferred_h,\n"
        + "    bool clamp_host_window);\n\n"
        + "void layoutEditorTopRightChrome(\n"
        + "    int host_width,\n"
        + "    juce::DrawableButton& settings,\n"
        + "    juce::Slider& master_volume,\n"
        + "    juce::DrawableButton* immediate_audition = nullptr);\n\n"
        + "void paintPageBackground(\n"
        + "    juce::Graphics& graphics,\n"
        + "    juce::Rectangle<int> bounds);\n\n"
        + "void fillRoundedPanelFrame(\n"
        + "    juce::Graphics& graphics,\n"
        + "    juce::Rectangle<int> bounds);\n\n"
        + "void strokeRoundedPanelFrame(\n"
        + "    juce::Graphics& graphics,\n"
        + "    juce::Rectangle<int> bounds);\n\n"
        + "void paintRoundedPanelFrame(\n"
        + "    juce::Graphics& graphics,\n"
        + "    juce::Rectangle<int> bounds);\n",
    )

    write(
        JUCE / "ui_paint.cpp",
        SPDX
        + '#include "ui_paint.hpp"\n\n'
        + read_extract("apply_scaled")
        + "\n"
        + read_extract("layout_chrome")
        + "\n"
        + read_extract("paint_body"),
    )

    about_body = read_extract("about_panel_body").replace(
        "        logo_ = loadImageMemory(\n"
        "            BinaryData::MGSTC_logo_png,\n"
        "            static_cast<std::size_t>(BinaryData::MGSTC_logo_pngSize));",
        "        logo_ = juce::ImageFileFormat::loadFrom(\n"
        "            BinaryData::MGSTC_logo_png,\n"
        "            BinaryData::MGSTC_logo_pngSize);",
    )
    write(
        JUCE / "about_panel.hpp",
        SPDX
        + "#pragma once\n\n"
        + "#ifndef MGSTC_DOC_VERSION\n"
        + '#define MGSTC_DOC_VERSION "0.0"\n'
        + "#endif\n\n"
        + "#include <juce_gui_basics/juce_gui_basics.h>\n\n"
        + "#include \"BinaryData.h\"\n"
        + "#include \"ui_fonts.hpp\"\n"
        + "#include \"ui_layout.hpp\"\n"
        + "#include \"ui_scale.hpp\"\n\n"
        + about_body,
    )

    # Patch main.cpp
    lines = MAIN.read_text(encoding="utf-8").splitlines(keepends=True)
    delete_ranges = [
        (7000, 7529),
        (5097, 5142),
        (5072, 5094),
        (5045, 5068),
        (5041, 5042),
        (5037, 5039),
        (4798, 5035),
        (2808, 2952),
        (1531, 1617),
        (1371, 1528),
        (1154, 1364),
    ]
    for start, end in sorted(delete_ranges, reverse=True):
        del lines[start - 1 : end]

    text = "".join(lines)
    text = text.replace(
        "// Editor Open/Paste/Settings icon over; shared UI hover accent.\n"
        "constexpr juce::uint32 kUiHoverAccent = 0xFF53E3A6;\n",
        "",
    )
    includes = (
        '#include "ui_chrome_constants.hpp"\n'
        '#include "ui_paths.hpp"\n'
        '#include "ui_scale.hpp"\n'
        '#include "ui_fonts.hpp"\n'
        '#include "ui_layout.hpp"\n'
        '#include "ui_paint.hpp"\n'
        '#include "switch_look_and_feel.hpp"\n'
        '#include "mgstc_look_and_feel.hpp"\n'
        '#include "about_panel.hpp"\n'
    )
    needle = '#include "ui_hang_watchdog.hpp"\n'
    if needle not in text:
        raise SystemExit("ui_hang_watchdog include not found")
    text = text.replace(needle, needle + includes)

    # applicationDataDirectory in main still used - redirect to ui_paths
    text = text.replace(
        "[[nodiscard]] juce::File applicationDataDirectory() {\n"
        "    return juce::File::getSpecialLocation(\n"
        "               juce::File::userApplicationDataDirectory)\n"
        "        .getChildFile(\"MgsToneCraft\");\n"
        "}",
        "[[nodiscard]] juce::File applicationDataDirectory() {\n"
        "    return mgstcApplicationDataDirectory();\n"
        "}",
    )

    MAIN.write_text(text, encoding="utf-8")
    print(f"patched main.cpp ({len(lines)} lines after deletion)")

    # CMakeLists
    cmake = ROOT / "CMakeLists.txt"
    cmake_text = cmake.read_text(encoding="utf-8")
    additions = [
        "                src/app/juce/ui_layout.cpp",
        "                src/app/juce/ui_paint.cpp",
    ]
    for item in additions:
        if item not in cmake_text:
            cmake_text = cmake_text.replace(
                "                src/app/juce/main.cpp",
                "                src/app/juce/ui_layout.cpp\n"
                "                src/app/juce/ui_paint.cpp\n"
                "                src/app/juce/main.cpp",
                1,
            )
    cmake.write_text(cmake_text, encoding="utf-8")
    print("updated CMakeLists.txt")


if __name__ == "__main__":
    main()
