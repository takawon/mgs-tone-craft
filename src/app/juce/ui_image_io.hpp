// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <juce_gui_basics/juce_gui_basics.h>
#pragma comment(lib, "gdiplus.lib")

namespace mgstc::app {
struct AnimatedGifSupport final {
    AnimatedGifSupport() {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&token_, &input, nullptr);
    }

    ~AnimatedGifSupport() {
        if (token_ != 0) {
            Gdiplus::GdiplusShutdown(token_);
        }
    }

    AnimatedGifSupport(const AnimatedGifSupport&) = delete;
    AnimatedGifSupport& operator=(const AnimatedGifSupport&) = delete;

    ULONG_PTR token_ = 0;
};

inline [[nodiscard]] AnimatedGifSupport& ensureGdiplusInitialized() {
    static AnimatedGifSupport runtime;
    return runtime;
}

inline [[nodiscard]] juce::Image gdiplusBitmapToImage(
    Gdiplus::Bitmap& bitmap) {
    const auto width = static_cast<int>(bitmap.GetWidth());
    const auto height = static_cast<int>(bitmap.GetHeight());
    if (width <= 0 || height <= 0) {
        return {};
    }

    Gdiplus::BitmapData data{};
    const Gdiplus::Rect rect(0, 0, width, height);
    if (bitmap.LockBits(
            &rect,
            Gdiplus::ImageLockModeRead,
            PixelFormat32bppARGB,
            &data)
        != Gdiplus::Ok) {
        return {};
    }

    juce::Image image(
        juce::Image::ARGB, width, height, true);
    {
        juce::Image::BitmapData dest(
            image,
            juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < height; ++y) {
            const auto* src = static_cast<const std::uint8_t*>(
                data.Scan0)
                + static_cast<std::ptrdiff_t>(y) * data.Stride;
            auto* out = dest.getLinePointer(y);
            for (int x = 0; x < width; ++x) {
                const auto b = src[x * 4 + 0];
                const auto g = src[x * 4 + 1];
                const auto r = src[x * 4 + 2];
                const auto a = src[x * 4 + 3];
                juce::PixelARGB pixel(a, r, g, b);
                pixel.premultiply();
                reinterpret_cast<juce::PixelARGB*>(out)[x] = pixel;
            }
        }
    }
    bitmap.UnlockBits(&data);
    return image;
}

inline [[nodiscard]] juce::Image loadImageMemory(
    const void* data,
    std::size_t size) {
    if (data == nullptr || size == 0) {
        return {};
    }
    if (auto image = juce::ImageFileFormat::loadFrom(data, size);
        image.isValid()) {
        return image;
    }
    ensureGdiplusInitialized();
    const auto memory = ::GlobalAlloc(
        GMEM_MOVEABLE, static_cast<SIZE_T>(size));
    if (memory == nullptr) {
        return {};
    }
    if (auto* locked = ::GlobalLock(memory)) {
        std::memcpy(locked, data, size);
        ::GlobalUnlock(memory);
    } else {
        ::GlobalFree(memory);
        return {};
    }
    IStream* stream = nullptr;
    if (::CreateStreamOnHGlobal(memory, TRUE, &stream) != S_OK
        || stream == nullptr) {
        ::GlobalFree(memory);
        return {};
    }
    juce::Image image;
    {
        Gdiplus::Bitmap bitmap(stream);
        if (bitmap.GetLastStatus() == Gdiplus::Ok) {
            image = gdiplusBitmapToImage(bitmap);
        }
    }
    stream->Release();
    return image;
}

inline [[nodiscard]] juce::Image loadImageFileForBackground(
    const juce::File& file) {
    if (!file.existsAsFile()) {
        return {};
    }
    if (auto image = juce::ImageFileFormat::loadFrom(file);
        image.isValid()) {
        return image;
    }
    ensureGdiplusInitialized();
    Gdiplus::Bitmap bitmap(file.getFullPathName().toWideCharPointer());
    if (bitmap.GetLastStatus() != Gdiplus::Ok) {
        return {};
    }
    return gdiplusBitmapToImage(bitmap);
}

inline [[nodiscard]] juce::Image loadImageBytesForBackground(
    const std::vector<std::uint8_t>& bytes) {
    return loadImageMemory(bytes.data(), bytes.size());
}

inline bool loadAnimatedGifFrames(
    const void* data,
    int size,
    std::vector<juce::Image>& frames,
    std::vector<int>& delays_ms) {
    ensureGdiplusInitialized();
    frames.clear();
    delays_ms.clear();
    if (data == nullptr || size <= 0) {
        return false;
    }

    const auto bytes = static_cast<SIZE_T>(size);
    const auto memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) {
        return false;
    }
    if (auto* locked = ::GlobalLock(memory)) {
        std::memcpy(locked, data, bytes);
        ::GlobalUnlock(memory);
    } else {
        ::GlobalFree(memory);
        return false;
    }

    IStream* stream = nullptr;
    if (FAILED(::CreateStreamOnHGlobal(memory, TRUE, &stream))
        || stream == nullptr) {
        ::GlobalFree(memory);
        return false;
    }

    Gdiplus::Bitmap bitmap(stream);
    stream->Release();
    if (bitmap.GetLastStatus() != Gdiplus::Ok) {
        return false;
    }

    static const GUID dimension = { 0x6aedbd6d, 0x3fb5, 0x418a, { 0x83, 0xa6, 0x7f, 0x45, 0x22, 0x9d, 0xc8, 0x72 } };
    const UINT frame_count = bitmap.GetFrameCount(&dimension);
    if (frame_count == 0) {
        return false;
    }

    // GIFのフレーム遅延はPropertyTagFrameDelayに1/100秒単位で格納される。
    // タグがない、値が0、または読込に失敗したフレームだけ50msへフォールバックする。
    constexpr PROPID frame_delay_property = 0x5100;
    constexpr int fallback_delay_ms = 50;
    std::vector<std::uint64_t> delay_storage;
    const auto delay_property_size =
        bitmap.GetPropertyItemSize(frame_delay_property);
    if (delay_property_size > 0) {
        delay_storage.resize(
            (static_cast<std::size_t>(delay_property_size)
             + sizeof(std::uint64_t) - 1)
            / sizeof(std::uint64_t));
    }
    const Gdiplus::PropertyItem* delay_property = nullptr;
    if (!delay_storage.empty()) {
        auto* item = reinterpret_cast<Gdiplus::PropertyItem*>(
            delay_storage.data());
        if (bitmap.GetPropertyItem(
                frame_delay_property,
                delay_property_size,
                item)
            == Gdiplus::Ok) {
            delay_property = item;
        }
    }
    const auto delayForFrame = [delay_property](UINT frame) {
        if (delay_property == nullptr
            || delay_property->value == nullptr
            || delay_property->type
                != 4) {  // PropertyTagTypeLong
            return fallback_delay_ms;
        }
        const auto delay_count =
            delay_property->length / sizeof(UINT);
        if (frame >= delay_count) {
            return fallback_delay_ms;
        }
        const auto hundredths =
            static_cast<const UINT*>(delay_property->value)[frame];
        if (hundredths == 0) {
            return fallback_delay_ms;
        }
        constexpr auto maximum_hundredths =
            static_cast<UINT>(
                std::numeric_limits<int>::max() / 10);
        return static_cast<int>(
            juce::jmin(hundredths, maximum_hundredths) * 10);
    };

    frames.reserve(frame_count);
    delays_ms.reserve(frame_count);
    for (UINT frame = 0; frame < frame_count; ++frame) {
        if (bitmap.SelectActiveFrame(&dimension, frame)
            != Gdiplus::Ok) {
            continue;
        }
        auto converted = gdiplusBitmapToImage(bitmap);
        if (!converted.isValid()) {
            continue;
        }
        frames.push_back(std::move(converted));
        delays_ms.push_back(delayForFrame(frame));
    }
    return !frames.empty();
}

class AnimatedGifComponent final
    : public juce::Component,
      private juce::Timer {
public:
    void loadFromMemory(const void* data, int size) {
        stopTimer();
        frames_.clear();
        delays_ms_.clear();
        frame_index_ = 0;
        static_cast<void>(
            loadAnimatedGifFrames(data, size, frames_, delays_ms_));
        if (!frames_.empty()) {
            startTimer(delays_ms_.front());
        }
        repaint();
    }

    void setDisplaySize(int width, int height) {
        display_width_ = juce::jmax(1, width);
        display_height_ = juce::jmax(1, height);
        setSize(display_width_, display_height_);
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colours::transparentBlack);
        if (frames_.empty()) {
            return;
        }
        g.drawImageWithin(
            frames_[static_cast<size_t>(frame_index_)],
            0,
            0,
            getWidth(),
            getHeight(),
            juce::RectanglePlacement::centred
                | juce::RectanglePlacement::onlyReduceInSize);
    }

private:
    void timerCallback() override {
        if (frames_.size() <= 1) {
            return;
        }
        frame_index_ =
            (frame_index_ + 1) % static_cast<int>(frames_.size());
        repaint();
        startTimer(
            delays_ms_[static_cast<size_t>(frame_index_)]);
    }

    std::vector<juce::Image> frames_;
    std::vector<int> delays_ms_;
    int frame_index_{0};
    int display_width_{256};
    int display_height_{256};
};


} // namespace mgstc::app
