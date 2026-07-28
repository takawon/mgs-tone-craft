#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include "mgstc/engine/register_write.hpp"

namespace mgstc::engine {

class Ym2149Adapter {
public:
    Ym2149Adapter();
    ~Ym2149Adapter();
    Ym2149Adapter(Ym2149Adapter&&) noexcept;
    Ym2149Adapter& operator=(Ym2149Adapter&&) noexcept;
    Ym2149Adapter(const Ym2149Adapter&) = delete;
    Ym2149Adapter& operator=(const Ym2149Adapter&) = delete;

    [[nodiscard]] bool valid() const noexcept;
    void reset() noexcept;
    [[nodiscard]] bool write(
        std::uint8_t address,
        std::uint8_t value) noexcept;
    [[nodiscard]] float renderSample() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class SccAdapter {
public:
    SccAdapter();
    ~SccAdapter();
    SccAdapter(SccAdapter&&) noexcept;
    SccAdapter& operator=(SccAdapter&&) noexcept;
    SccAdapter(const SccAdapter&) = delete;
    SccAdapter& operator=(const SccAdapter&) = delete;

    [[nodiscard]] bool valid() const noexcept;
    void reset() noexcept;
    [[nodiscard]] bool write(
        std::uint8_t port,
        std::uint8_t address,
        std::uint8_t value) noexcept;
    [[nodiscard]] float renderSample() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Ym2413Adapter {
public:
    Ym2413Adapter();
    ~Ym2413Adapter();
    Ym2413Adapter(Ym2413Adapter&&) noexcept;
    Ym2413Adapter& operator=(Ym2413Adapter&&) noexcept;
    Ym2413Adapter(const Ym2413Adapter&) = delete;
    Ym2413Adapter& operator=(const Ym2413Adapter&) = delete;

    [[nodiscard]] bool valid() const noexcept;
    void reset() noexcept;
    [[nodiscard]] bool write(
        std::uint8_t address,
        std::uint8_t value) noexcept;
    [[nodiscard]] float renderSample() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct ChipSamples {
    float psg{};
    float scc{};
    float opll{};
};

class ChipRack {
public:
    [[nodiscard]] bool valid() const noexcept;
    void reset() noexcept;
    [[nodiscard]] bool apply(const RegisterWrite& write) noexcept;
    [[nodiscard]] bool apply(
        std::span<const RegisterWrite> writes) noexcept;
    [[nodiscard]] ChipSamples renderSample() noexcept;

private:
    Ym2149Adapter psg_;
    SccAdapter scc_;
    Ym2413Adapter opll_;
};

}  // namespace mgstc::engine
