// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

struct OpllEnvelopeValues {
    std::uint8_t attack_rate{}, decay_rate{}, sustain_level{}, release_rate{};
    bool operator==(const OpllEnvelopeValues&) const = default;
};

enum class OpllEnvelopeHandle { Attack, DecaySustain, Release };

// UI-only parameter geometry. These coordinates are deliberately NOT EG times
// or amplitudes; the independently simulated trace is authoritative for sound.
namespace OpllEnvelopeEditing {
struct Point {
    float x{}, y{};
    bool operator==(const Point&) const = default;
};
inline float distance(Point a, Point b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}
struct Bounds {
    float left{}, top{}, width{}, height{};
};
struct Guide {
    std::array<Point, 3> handles;
    Point sl, key_off, end;
};
inline Guide layout(Bounds b, OpllEnvelopeValues v, bool modulator, bool eg) {
    const float span = b.width * 0.22F;
    const float d = v.decay_rate / 15.0F;
    const Point attack{b.left + span * (1.0F - v.attack_rate / 15.0F), b.top};
    const float sl_y = b.top + b.height * v.sustain_level / 15.0F;
    const Point decay{attack.x + span * (1.0F - d),
                      b.top + (sl_y - b.top) * d};
    const float ko_x = b.left + b.width * 0.5F;
    const bool reaches_sl = v.decay_rate != 0 || v.sustain_level == 0;
    const float rr_progress = !eg && reaches_sl
        ? std::clamp((ko_x - decay.x) / span * v.release_rate / 15.0F, 0.0F, 1.0F)
        : 0.0F;
    const float ko_y = decay.y + (b.top + b.height - decay.y) * rr_progress;
    Point end{b.left + b.width, ko_y};
    if (!modulator && (!eg || v.release_rate != 0)) {
        const float rate = eg ? static_cast<float>(v.release_rate) : 7.0F;
        end = {ko_x + b.width * 0.5F * (1.0F - rate / 15.0F), b.top + b.height};
    }
    return {{{attack, decay, {b.left + b.width * 0.94F,
                              b.top + b.height * v.release_rate / 15.0F}}},
            {decay.x, sl_y}, {ko_x, ko_y}, end};
}

// Expand only the colliding AR/DR pair. Stable while the pointer travels from
// the original point to either proxy, including at the edge of the plot.
inline std::array<Point, 3> handlePositions(const Guide& g, bool expanded,
                                           float separation) {
    auto p = g.handles;
    if (expanded && distance(p[0], p[1]) < separation) {
        p[1].x = std::max(p[1].x, p[0].x + separation);
    }
    return p;
}
inline std::optional<OpllEnvelopeHandle> hitTest(
    Point pointer, const std::array<Point, 3>& points, float radius) {
    std::optional<OpllEnvelopeHandle> result;
    float nearest = radius;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const float d = distance(pointer, points[i]);
        if (d <= radius && (!result || d < nearest)) {
            nearest = d;
            result = static_cast<OpllEnvelopeHandle>(i);
        }
    }
    return result;
}

class Drag {
public:
    void begin(OpllEnvelopeHandle h, OpllEnvelopeValues v, Point pointer,
               Point handle, Bounds bounds, float threshold) {
        handle_ = h;
        initial_ = values_ = v;
        rates_ = {float(v.attack_rate), float(v.decay_rate),
                  float(v.sustain_level), float(v.release_rate)};
        start_ = last_ = pointer;
        visual_ = handle;
        bounds_ = bounds;
        threshold_ = threshold;
        active_ = true;
        moved_ = false;
        axis_ = Axis::Both;
        accepted_x_ = accepted_y_ = 0;
    }
    bool move(Point pointer, bool fine) {
        if (!active_) return false;
        if (!moved_) {
            const float x = std::abs(pointer.x - start_.x);
            const float y = std::abs(pointer.y - start_.y);
            if (std::hypot(x, y) < threshold_) return false;
            moved_ = true;
            if (handle_ == OpllEnvelopeHandle::DecaySustain) {
                axis_ = x > y * 2.0F ? Axis::Horizontal
                    : y > x * 2.0F ? Axis::Vertical : Axis::Both;
            }
        }
        float dx = pointer.x - last_.x;
        float dy = pointer.y - last_.y;
        last_ = pointer; // discard overshoot: reversing at a bound reacts immediately
        if (handle_ == OpllEnvelopeHandle::DecaySustain) {
            // An intentionally diagonal continuation can unlock the other axis.
            if (axis_ == Axis::Horizontal) {
                accepted_y_ += dy;
                if (std::abs(accepted_y_) > threshold_ * 3) axis_ = Axis::Both;
                else dy = 0;
            } else if (axis_ == Axis::Vertical) {
                accepted_x_ += dx;
                if (std::abs(accepted_x_) > threshold_ * 3) axis_ = Axis::Both;
                else dx = 0;
            }
        }
        const float scale = fine ? 0.2F : 1.0F;
        const float horizontal = 15.0F / std::max(1.0F, bounds_.width * 0.22F);
        const float vertical = 15.0F / std::max(1.0F, bounds_.height);
        const auto adjust = [&](int index, float delta, float sensitivity) {
            const float before = rates_[index];
            rates_[index] = std::clamp(before + delta * sensitivity * scale, 0.0F, 15.0F);
            return (rates_[index] - before) / sensitivity;
        };
        switch (handle_) {
        case OpllEnvelopeHandle::Attack:
            visual_.x += adjust(0, dx, -horizontal);
            break;
        case OpllEnvelopeHandle::DecaySustain:
            visual_.x += adjust(1, dx, -horizontal);
            visual_.y += adjust(2, dy, vertical);
            break;
        case OpllEnvelopeHandle::Release:
            visual_.y += adjust(3, dy, vertical);
            break;
        }
        const auto before = values_;
        values_ = {quantize(rates_[0]), quantize(rates_[1]),
                   quantize(rates_[2]), quantize(rates_[3])};
        return values_ != before;
    }
    bool finish() { active_ = false; return values_ != initial_; }
    bool cancel() {
        const bool changed = values_ != initial_;
        values_ = initial_;
        active_ = false;
        return changed;
    }
    bool active() const { return active_; }
    bool moved() const { return moved_; }
    OpllEnvelopeHandle handle() const { return handle_; }
    OpllEnvelopeValues values() const { return values_; }
    Point visual() const { return visual_; }
private:
    static std::uint8_t quantize(float v) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0F, 15.0F)));
    }
    enum class Axis { Horizontal, Vertical, Both };
    Axis axis_{Axis::Both};
    OpllEnvelopeHandle handle_{};
    OpllEnvelopeValues initial_{}, values_{};
    std::array<float, 4> rates_{};
    Point start_{}, last_{}, visual_{};
    Bounds bounds_{};
    float threshold_{}, accepted_x_{}, accepted_y_{};
    bool active_{}, moved_{};
};
} // namespace OpllEnvelopeEditing
