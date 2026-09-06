// SPDX-License-Identifier: AGPL-3.0-only
#include "../../src/app/juce/opll_envelope_graph.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
juce::Image render(OpllEnvelopeGraph& graph) {
    juce::Image image(juce::Image::ARGB, graph.getWidth(), graph.getHeight(), true);
    juce::Graphics g(image);
    graph.paint(g);
    return image;
}
// Discover controls in the actual raster instead of duplicating layout formulas.
std::vector<juce::Point<float>> redHandles(const juce::Image& image) {
    std::vector<juce::Rectangle<int>> blobs;
    for (int y = 0; y < image.getHeight(); ++y) for (int x = 0; x < image.getWidth(); ++x) {
        if (image.getPixelAt(x, y) != juce::Colour(0xFFFF514D)) continue;
        bool found = false;
        for (auto& box : blobs) if (box.expanded(2).contains(x, y)) {
            box = box.getUnion({x, y, 1, 1});
            found = true;
            break;
        }
        if (!found) blobs.emplace_back(x, y, 1, 1);
    }
    std::vector<juce::Point<float>> result;
    for (auto box : blobs) if (box.getWidth() > 3 && box.getHeight() > 3)
        result.push_back(box.toFloat().getCentre());
    std::sort(result.begin(), result.end(), [](auto a, auto b) { return a.x < b.x; });
    return result;
}
juce::MouseEvent event(OpllEnvelopeGraph& graph, juce::Point<float> pos,
                       juce::Point<float> down, bool pressed, bool dragged = false,
                       bool shift = false) {
    return {juce::Desktop::getInstance().getMainMouseSource(), pos,
            juce::ModifierKeys((pressed ? juce::ModifierKeys::leftButtonModifier : 0)
                              | (shift ? juce::ModifierKeys::shiftModifier : 0)),
            1, 0, 0, 0, 0, &graph, &graph, juce::Time::getCurrentTime(), down,
            juce::Time::getCurrentTime(), 1, dragged};
}
void writeImage(const juce::Image& image, const juce::File& file) {
    require(file.getParentDirectory().createDirectory().wasOk(), "snapshot directory");
    const auto stream = file.createOutputStream();
    require(stream && stream->openedOk(), "snapshot output");
    stream->setPosition(0);
    stream->truncate();
    require(juce::PNGImageFormat().writeImageToStream(image, *stream), "PNG encode");
}
void verify(float scale, const juce::File& out) {
    UiScale::active_factor = scale;
    UiLayout::applyScale(scale);
    const int w = juce::roundToInt(550 * scale);
    const int h = juce::roundToInt(196 * scale);
    juce::Image sheet(juce::Image::RGB, w * 2, (h + UiLayout::xl) * 5, true);
    {
    juce::Graphics sheet_g(sheet);
    for (int row = 0; row < 5; ++row) for (bool mod : {true, false}) {
        bool eg = row == 0 || row == 2;
        OpllEnvelopeValues v{10, 0, 8, 8};
        if (row >= 2) v.decay_rate = 12;
        if (row == 4) { v.attack_rate = 0; v.decay_rate = 15; v.sustain_level = 0; }
        OpllEnvelopeGraph graph(mod ? juce::Colour(0xFF64A7FF) : juce::Colour(0xFFFFA75E), mod);
        graph.setSize(w, h);
        graph.getValues = [&] { return v; };
        graph.getSustainedTone = [&] { return eg; };
        mgstc::engine::OpllPatchParameters patch{};
        for (auto* op : {&patch.modulator, &patch.carrier}) {
            op->attack_rate = v.attack_rate;
            op->decay_rate = v.decay_rate;
            op->sustain_level = v.sustain_level;
            op->release_rate = v.release_rate;
            op->multiplier = 1;
            op->sustained_tone = eg;
        }
        const auto trace = mgstc::engine::traceOpllEnvelope(patch, 60);
        graph.setTrace(mod ? trace.modulator : trace.carrier, trace.valid);
        int edits = 0, commits = 0;
        graph.onEdit = [&](auto next, bool commit) {
            require(!commit, "live and final callbacks must be separate");
            v = next;
            ++edits;
        };
        graph.onCommit = [&] { ++commits; };
        auto image = render(graph);
        if (row == 4) {
            const auto merged = redHandles(image);
            require(!merged.empty(), "OFF still has editable controls");
            graph.mouseMove(event(graph, merged.front(), merged.front(), false));
            image = render(graph);
        }
        const int x = mod ? 0 : w;
        const int y = row * (h + UiLayout::xl);
        sheet_g.setColour(juce::Colours::white);
        sheet_g.setFont(UiFonts::dense());
        sheet_g.drawText(juce::String(mod ? "MOD" : "CAR") + (eg ? " EG1" : " EG0")
            + "  AR " + juce::String(v.attack_rate) + " DR " + juce::String(v.decay_rate)
            + " SL " + juce::String(v.sustain_level) + " RR " + juce::String(v.release_rate),
            x + UiLayout::xs, y, w, UiLayout::xl, juce::Justification::centredLeft);
        sheet_g.drawImageAt(image, x, y + UiLayout::xl);
        auto points = redHandles(image);
        require(points.size() == 3, "all modes expose exactly three red controls");
        if (row >= 2) continue;
        require(std::abs(points[0].y - points[1].y) < 1, "rendered DR0 must be horizontal");
        const auto start = v;
        for (auto p : points) {
            graph.mouseDown(event(graph, p, p, true));
            graph.mouseUp(event(graph, p, p, false));
        }
        require(edits == 0 && commits == 0 && v == start, "clicks must be no-ops");
        auto down = points[1];
        auto moved = down.translated(0, 20 * scale);
        graph.mouseDown(event(graph, down, down, true));
        graph.mouseDrag(event(graph, moved, down, true, true));
        require(v.decay_rate == 0 && v.sustain_level > start.sustain_level,
                "actual DR0 mouse event changes only SL");
        if (scale == 1 && mod && row == 0 && out != juce::File{})
            writeImage(render(graph), out.getChildFile("sl-drag.png"));
        require(graph.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)), "Esc handled");
        graph.mouseUp(event(graph, moved, down, false, true));
        require(v == start && commits == 0, "Esc restores values without history commit");

        down = points[2];
        moved = down.translated(0, 30 * scale);
        graph.mouseDown(event(graph, down, down, true));
        graph.mouseDrag(event(graph, moved, down, true, true));
        const int before_up = edits;
        graph.mouseUp(event(graph, moved, down, false, true));
        require(v.release_rate > start.release_rate && v.sustain_level == start.sustain_level,
                "actual RR event isolated from SL");
        require(commits == 1 && edits == before_up, "mouse up must not repeat preview");
        const auto after = v;
        graph.mouseDrag(event(graph, moved.translated(0, 15), moved, true, true));
        graph.mouseWheelMove(event(graph, down, down, false), juce::MouseWheelDetails{});
        require(v == after, "unstarted drag and wheel are ignored");
        graph.mouseDown(event(graph, down, down, true));
        graph.mouseDrag(event(graph, moved, down, true, true));
        graph.focusLost(juce::Component::focusChangedDirectly);
        require(v == after && commits == 1, "focus loss cancels without an extra commit");
        eg = !eg;
        const auto switched = redHandles(render(graph));
        require(switched.size() == 3, "EG switch keeps all controls");
    }
    } // Flush the graphics context before encoding the contact sheet.
    // Degenerate AR=15, DR=15, SL=0: both controls must be discoverable by hover.
    OpllEnvelopeValues v{15, 15, 0, 8};
    OpllEnvelopeGraph overlap(juce::Colours::orange, false);
    overlap.setSize(w, h);
    overlap.getValues = [&] { return v; };
    overlap.onEdit = [&](auto next, bool) { v = next; };
    auto merged = redHandles(render(overlap));
    require(merged.size() == 2, "degenerate pair actually overlaps before hover");
    const auto ar = merged.front();
    overlap.mouseMove(event(overlap, ar, ar, false));
    auto split = redHandles(render(overlap));
    require(split.size() == 3, "hover separates the overlapping pair");
    const auto dr = split[1];
    overlap.mouseMove(event(overlap, dr, dr, false));
    overlap.mouseDown(event(overlap, dr, dr, true));
    const auto sl = dr.translated(0, 25 * scale);
    overlap.mouseDrag(event(overlap, sl, dr, true, true));
    overlap.mouseUp(event(overlap, sl, dr, false, true));
    require(v.attack_rate == 15 && v.sustain_level > 0, "separated DR point remains selectable");
    v = {0, 15, 0, 8};
    merged = redHandles(render(overlap));
    const auto off_ar = merged.front();
    overlap.mouseMove(event(overlap, off_ar, off_ar, false));
    overlap.mouseDown(event(overlap, off_ar, off_ar, true));
    const auto enabled_ar = off_ar.translated(-20 * scale, 0);
    overlap.mouseDrag(event(overlap, enabled_ar, off_ar, true, true));
    overlap.mouseUp(event(overlap, enabled_ar, off_ar, false, true));
    require(v.attack_rate > 0 && v.decay_rate == 15 && v.sustain_level == 0,
            "AR0 can be re-enabled even with an overlapping DR point");
    if (out != juce::File{})
        writeImage(sheet, out.getChildFile("envelopes-" + juce::String(juce::roundToInt(scale * 100)) + ".png"));
}
} // namespace
int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI init;
    try {
        const auto out = argc > 1 ? juce::File(juce::String::fromUTF8(argv[1])) : juce::File{};
        for (float scale : {0.75F, 1.0F, 1.25F}) verify(scale, out);
        std::cout << "[PASS] Actual component paint/mouse/cancel/commit/overlap at 75/100/125%\n";
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << '\n';
        return 1;
    }
}
