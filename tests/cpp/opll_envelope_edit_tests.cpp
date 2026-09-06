// SPDX-License-Identifier: AGPL-3.0-only
#include "../../src/app/juce/opll_envelope_edit.hpp"
#include "mgstc/engine/opll_envelope_trace.hpp"
#include <iostream>
#include <stdexcept>

namespace {
using namespace OpllEnvelopeEditing;
using H = OpllEnvelopeHandle;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool near(float a, float b) { return std::abs(a - b) < 0.001F; }

void geometry() {
    for (float scale : {0.75F, 1.0F, 1.25F}) {
        const Bounds b{26 * scale, 42 * scale, 480 * scale, 130 * scale};
        for (bool mod : {false, true}) for (bool eg : {false, true})
        for (int ar = 0; ar <= 15; ++ar) for (int sl = 0; sl <= 15; ++sl) {
            OpllEnvelopeValues v{std::uint8_t(ar), 0, std::uint8_t(sl), 8};
            auto g = layout(b, v, mod, eg);
            require(near(g.handles[0].y, g.handles[1].y)
                        && g.handles[1].x > g.handles[0].x, "DR0 must be beside AR");
            auto prev = g.handles[1];
            for (int dr = 1; dr <= 15; ++dr) {
                v.decay_rate = std::uint8_t(dr);
                g = layout(b, v, mod, eg);
                require(g.handles[1].x <= prev.x && g.handles[1].y >= prev.y,
                        "DR placement must be continuous and monotonic");
                prev = g.handles[1];
            }
            require(near(g.handles[1].x, g.handles[0].x)
                        && near(g.handles[1].y, g.sl.y), "DR15 must reach SL below AR");
            require(near(g.handles[2].y, b.top + b.height * 8 / 15),
                    "RR location must not depend on SL/EG/operator");
            if (ar < 15) {
                v.attack_rate++;
                const auto next = layout(b, v, mod, eg);
                require(near(next.handles[1].x - g.handles[1].x,
                             next.handles[0].x - g.handles[0].x), "DR follows AR rigidly");
            }
        }
    }
    const Bounds b{26, 42, 480, 130};
    const auto overlap = layout(b, {15, 15, 0, 0}, false, true);
    const auto expanded = handlePositions(overlap, true, 36);
    require(hitTest(expanded[0], expanded, 16) == H::Attack, "overlap: AR selectable");
    require(hitTest(expanded[1], expanded, 16) == H::DecaySustain, "overlap: DR selectable");
    require(!hitTest({300, 190}, expanded, 16), "background is not editable");
}

void gestures() {
    const Bounds b{0, 0, 500, 150};
    const OpllEnvelopeValues initial{8, 0, 8, 8};
    Drag drag;
    for (auto h : {H::Attack, H::DecaySustain, H::Release}) {
        drag.begin(h, initial, {100, 100}, {100, 100}, b, 4);
        require(!drag.move({100, 100}, false), "click must not change values");
        require(!drag.move({102, 101}, false), "small jitter must not change values");
        require(!drag.finish(), "no-op must not commit");
    }
    drag.begin(H::Attack, initial, {100, 100}, {100, 100}, b, 4);
    require(drag.move({45, 150}, false), "AR edit");
    require(drag.values().decay_rate == 0 && drag.values().sustain_level == 8
                && drag.values().release_rate == 8, "AR modifies only AR");
    require(drag.cancel() && drag.values() == initial && !drag.active(), "Esc restores all values");
    require(!drag.move({0, 0}, false), "cancelled drag ignores later events");

    drag.begin(H::DecaySustain, initial, {100, 100}, {100, 0}, b, 4);
    require(drag.move({101, 130}, false), "SL editable with DR0");
    require(drag.values().decay_rate == 0 && drag.values().sustain_level == 11,
            "vertical DR0 drag changes SL only");
    require(near(drag.visual().y, 30), "grabbed proxy follows vertical movement");
    require(near(layout(b, drag.values(), false, true).handles[1].y, 0),
            "DR0 return target remains horizontal");

    drag.begin(H::DecaySustain, initial, {100, 100}, {100, 0}, b, 4);
    drag.move({45, 101}, false);
    require(drag.values().decay_rate > 0 && drag.values().sustain_level == 8,
            "horizontal DR0 drag changes DR without jittering SL");
    drag.begin(H::DecaySustain, initial, {100, 100}, {100, 0}, b, 4);
    drag.move({70, 130}, false);
    require(drag.values().decay_rate > 0 && drag.values().sustain_level > 8,
            "diagonal drag edits DR and SL together");

    for (auto h : {H::DecaySustain, H::Release}) {
        drag.begin(h, initial, {100, 100}, {100, 0}, b, 4);
        drag.move({100, -10000}, false);
        require((h == H::Release ? drag.values().release_rate : drag.values().sustain_level) == 0,
                "above plot clamps to zero without unsigned wrap");
        drag.move({100, -9990}, false);
        require((h == H::Release ? drag.values().release_rate : drag.values().sustain_level) == 1,
                "reversal after overshoot immediately changes value");
        drag.move({100, 10000}, false);
        require((h == H::Release ? drag.values().release_rate : drag.values().sustain_level) == 15,
                "below plot reaches maximum");
    }
    drag.begin(H::Release, initial, {100, 100}, {100, 80}, b, 4);
    drag.move({100, 150}, true);
    require(drag.values().release_rate == 9, "Shift reduces sensitivity to one fifth");
    require(!drag.move({100, 150}, false), "changing Shift alone does not jump values");
    drag.move({100, 160}, false);
    require(drag.values().release_rate == 10, "normal sensitivity resumes incrementally");
    require(drag.finish(), "changed drag commits once");

    drag.begin(H::Release, initial, {100, 100}, {100, 80}, b, 4);
    drag.move({100, 130}, false);
    drag.move({100, 100}, false);
    require(!drag.finish(), "returning to start is not a history entry");

    for (float scale : {0.75F, 1.0F, 1.25F}) for (int sl = 0; sl <= 15; ++sl) {
        auto v = initial;
        v.sustain_level = std::uint8_t(sl);
        v.release_rate = 0;
        const Bounds scaled{0, 0, 500 * scale, 150 * scale};
        drag.begin(H::Release, v, {0, 0}, {0, 0}, scaled, 4 * scale);
        drag.move({0, 150 * scale}, false);
        require(drag.values().release_rate == 15 && drag.values().sustain_level == sl,
                "RR full range independent of SL and UI scale");
    }
}

void chipBehaviour() {
    using namespace mgstc::engine;
    OpllPatchParameters patch{};
    for (auto* op : {&patch.modulator, &patch.carrier}) {
        op->attack_rate = 15;
        op->decay_rate = 0;
        op->sustain_level = 1;
        op->release_rate = 8;
        op->multiplier = 1;
    }
    constexpr auto before_ko = OpllEnvelopeTrace::kPointCount * 3 / 8;
    constexpr auto end = OpllEnvelopeTrace::kPointCount - 1;
    for (bool eg : {false, true}) {
        patch.modulator.sustained_tone = patch.carrier.sustained_tone = eg;
        const auto held = traceOpllEnvelope(patch, 60);
        require(held.valid && held.modulator[before_ko] > 0.9F
                    && held.carrier[before_ko] > 0.9F,
                "DR0 SL>0 blocks RR before key-off in both modes");
        require(near(held.modulator[before_ko], held.modulator[end]),
                "MOD freezes after key-off");
        require(held.carrier[end] < held.carrier[before_ko], "CAR releases after key-off");
    }
    patch.modulator.sustained_tone = patch.carrier.sustained_tone = false;
    patch.modulator.sustain_level = patch.carrier.sustain_level = 0;
    auto decay = traceOpllEnvelope(patch, 60);
    require(decay.modulator[before_ko] < 0.1F && decay.carrier[before_ko] < 0.1F,
            "EG0 DR0 SL0 must enter RR decay for BOTH operators");
    patch.modulator.release_rate = patch.carrier.release_rate = 0;
    auto rr_zero = traceOpllEnvelope(patch, 60);
    require(rr_zero.modulator[before_ko] > 0.9F && rr_zero.carrier[before_ko] > 0.9F,
            "RR0 does not introduce artificial decay");
    require(rr_zero.carrier[end] < 0.1F, "CAR EG0 key-off uses fixed rate, not RR0");
    patch.modulator.sustained_tone = patch.carrier.sustained_tone = true;
    patch.modulator.release_rate = patch.carrier.release_rate = 15;
    auto eg_one = traceOpllEnvelope(patch, 60);
    require(eg_one.modulator[end] > 0.9F && eg_one.carrier[before_ko] > 0.9F,
            "EG1 holds before key-off; MOD RR is inactive");
    patch.carrier.attack_rate = 0;
    auto off = traceOpllEnvelope(patch, 60);
    require(*std::max_element(off.carrier.begin(), off.carrier.end()) == 0,
            "AR0 fresh preview is off");
}
} // namespace

int main() {
    try {
        geometry(); std::cout << "[PASS] Geometry, overlap selection and UI scales\n";
        gestures(); std::cout << "[PASS] Drag isolation, SL proxy, clamp, Shift and cancel\n";
        chipBehaviour(); std::cout << "[PASS] emu2413 MOD/CAR EG0/1 boundary behaviour\n";
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << '\n';
        return 1;
    }
}
