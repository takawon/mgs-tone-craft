namespace ga_detail {
constexpr std::size_t population_size = 64, elite_count = 8, immigrant_count = 8;
constexpr std::size_t tournament_size = 3, attempts_per_candidate = 32;
constexpr std::uint32_t small_mutation_percent = 70, combination_mutation_percent = 25;
}
// Deterministic initial and continuation search in the engine private namespace.
// Score generation and selection remain independent of worker completion order.
struct GaRandom {
    std::uint32_t state;
    explicit GaRandom(std::uint32_t seed) : state(seed ? seed : 0x6D2B79F5U) {}
    std::uint32_t next() {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return state;
    }
};

template <typename Executor, typename Admit, typename Less>
bool gaResidual(
    bool full, std::size_t budget, std::size_t& evaluations,
    std::vector<ScoredOpllPatch>& scored,
    const std::vector<ScoredOpllPatch>& staged,
    const std::vector<ScoredOpllPatch>& extra_seeds,
    Executor& executor, std::size_t batch_size, std::uint64_t weight,
    OpllApproximationCoordinator* coordinator, Admit admit, Less less) {
    namespace gp = ga_detail;
    if (staged.empty()) throw std::runtime_error("GA requires a scored staged anchor");
    auto& metrics = *coordinator->continuous;
    metrics.ga_used = true;
    const auto anchor = staged.front().registers;
    const std::uint32_t phase_seed = full ? 0x46554C4CU : 0x53484F52U;
    const auto seed = coordinator->continuous->seed ^ phase_seed;
    GaRandom initial{seed ^ 0x13579BDFU};
    GaRandom crossover{seed ^ 0x2468ACE1U};
    GaRandom mutation{seed ^ 0xA5A5C3C3U};
    GaRandom immigrant{seed ^ 0x5A5A3C3CU};
    const auto normalized = [&](std::array<std::uint8_t, 8> registers) {
        if (full) {
            for (int i = 0; i < 4; ++i) registers[i] = anchor[i];
        } else {
            for (int i = 4; i < 8; ++i) registers[i] = anchor[i];
        }
        return encodeOpllPatch(decodeOpllPatch(registers));
    };
    const auto broad = [&](GaRandom& rng) {
        auto registers = anchor;
        const int begin = full ? 4 : 0;
        for (int i = begin; i < begin + 4; ++i)
            registers[i] = static_cast<std::uint8_t>(rng.next() >> 24);
        return normalized(registers);
    };
    std::map<std::array<std::uint8_t, 8>, ScoredOpllPatch> cache;
    for (const auto& entry : scored) cache.emplace(entry.registers, entry);
    std::vector<ScoredOpllPatch> population;
    const auto retain = [&](std::vector<ScoredOpllPatch> items) {
        std::sort(items.begin(), items.end(), less);
        std::set<std::array<std::uint8_t, 8>> unique;
        std::vector<ScoredOpllPatch> output;
        for (const auto& item : items) {
            if (unique.insert(item.registers).second) output.push_back(item);
            if (output.size() == gp::population_size) break;
        }
        return output;
    };
    // Staged winners are protected separately in scored, even when phase
    // normalization means a staged non-anchor does not belong to this GA.
    std::vector<std::array<std::uint8_t, 8>> seeds;
    for (std::size_t i = 0; i < std::min<std::size_t>(32, staged.size()); ++i)
        seeds.push_back(normalized(staged[i].registers));
    for (std::size_t i = 0; i < std::min<std::size_t>(16, extra_seeds.size()); ++i)
        seeds.push_back(normalized(extra_seeds[i].registers));
    while (seeds.size() < gp::population_size) seeds.push_back(broad(initial));
    std::size_t seed_position{};
    std::uint64_t fallback_position{};
    const auto fallback = [&] {
        // Injective enumeration of legal phase genes: 29 timbre bits or
        // 32 envelope bits. At most cache.size()+batch.size() values can be
        // occupied, so finite scan always finds a new point at these budgets.
        const std::uint64_t limit = full ? (1ULL << 32) : (1ULL << 29);
        if (fallback_position >= limit) throw std::runtime_error("GA phase space exhausted");
        auto registers = anchor;
        auto value = fallback_position++;
        if (full) {
            for (int i = 4; i < 8; ++i) { registers[i] = value & 255; value >>= 8; }
        } else {
            for (int i = 0; i < 3; ++i) { registers[i] = value & 255; value >>= 8; }
            // Register 3 bit5 is reserved; KSL occupies bits6/7.
            registers[3] = static_cast<std::uint8_t>((value & 31) | ((value & 96) << 1));
        }
        ++metrics.fallback_candidates;
        return normalized(registers);
    };
    const auto parent = [&]() -> const ScoredOpllPatch& {
        std::size_t best = crossover.next() % population.size();
        for (std::size_t i = 1; i < gp::tournament_size; ++i) {
            const auto challenger = crossover.next() % population.size();
            if (less(population[challenger], population[best])) best = challenger;
        }
        return population[best];
    };
    const auto child = [&] {
        const auto a = parent().registers;
        const auto b = parent().registers;
        auto registers = a;
        if (full) {
            if (crossover.next() & 1) { registers[4] = b[4]; registers[6] = b[6]; }
            if (crossover.next() & 1) { registers[5] = b[5]; registers[7] = b[7]; }
            // EG-TYP/KR are fixed with the stage anchor in this phase.
        } else {
            // MOD MULT/TL/FB/WS and its flags remain a single block.
            if (crossover.next() & 1) {
                registers[0] = b[0]; registers[2] = b[2];
                registers[3] = (registers[3] & 0xD0) | (b[3] & 0x0F);
            }
            // CAR MULT/WS/flags/KSL form the other block.
            if (crossover.next() & 1) {
                registers[1] = b[1];
                registers[3] = (registers[3] & 0x0F) | (b[3] & 0xD0);
            }
        }
        const auto action = mutation.next() % 100;
        if (action < gp::small_mutation_percent) {
            if (full) {
                const auto index = 4 + mutation.next() % 4;
                const bool high = (mutation.next() & 1) != 0;
                const int shift = high ? 4 : 0;
                const int old = (registers[index] >> shift) & 15;
                const int delta = (mutation.next() & 1) ? 1 : -1;
                const int value = std::clamp(old + delta, 0, 15);
                registers[index] = static_cast<std::uint8_t>(
                    (registers[index] & ~(15 << shift)) | (value << shift));
            } else {
                auto patch = decodeOpllPatch(registers);
                const auto gene = mutation.next() % 7;
                const int delta = (mutation.next() & 1) ? 1 : -1;
                const auto change = [&](std::uint8_t& value, int maximum) {
                    value = static_cast<std::uint8_t>(std::clamp(int(value) + delta, 0, maximum));
                };
                switch (gene) {
                case 0: change(patch.modulator.multiplier, 15); break;
                case 1: change(patch.carrier.multiplier, 15); break;
                case 2: change(patch.modulator.total_level, 63); break;
                case 3: change(patch.feedback, 7); break;
                case 4: patch.modulator.waveform = !patch.modulator.waveform; break;
                case 5: patch.carrier.waveform = !patch.carrier.waveform; break;
                default: registers[mutation.next() % 2] ^= 0x10U << (mutation.next() % 4);
                    patch = decodeOpllPatch(registers); break;
                }
                registers = encodeOpllPatch(patch);
            }
        } else if (action < gp::small_mutation_percent + gp::combination_mutation_percent) {
            const auto replacement = broad(mutation);
            const bool mod = (mutation.next() & 1) != 0;
            if (full) {
                const int index = mod ? 4 : 5;
                registers[index] = replacement[index]; registers[index + 2] = replacement[index + 2];
            } else if (mod) {
                registers[0] = replacement[0]; registers[2] = replacement[2];
                registers[3] = (registers[3] & 0xD0) | (replacement[3] & 0x0F);
            } else {
                registers[1] = replacement[1];
                registers[3] = (registers[3] & 0x0F) | (replacement[3] & 0xD0);
            }
        }
        return normalized(registers);
    };
    while (evaluations < budget) {
        if (coordinator->cancelled()) return false;
        std::vector<ScoredOpllPatch> batch;
        std::vector<ScoredOpllPatch> reused;
        const auto needed = std::min<std::size_t>(gp::population_size, budget - evaluations);
        for (std::size_t position = 0; position < needed; ++position) {
            std::array<std::uint8_t, 8> registers{};
            if (coordinator->cancelled()) return false;
            bool novel = false;
            for (std::size_t attempt = 0; attempt < gp::attempts_per_candidate; ++attempt) {
                if (seed_position < seeds.size()) registers = seeds[seed_position++];
                else if (population.empty()) registers = broad(initial);
                else if (position < gp::immigrant_count) registers = broad(immigrant);
                else registers = child();
                const auto found = cache.find(registers);
                if (found != cache.end()) {
                    ++metrics.duplicate_cache_hits;
                    reused.push_back(found->second);
                    continue;
                }
                const auto count = batch.size();
                admit(decodeOpllPatch(registers), batch);
                if (batch.size() != count) { novel = true; break; }
                ++metrics.duplicate_cache_hits;
            }
            while (!novel) {
                if (coordinator->cancelled()) return false;
                registers = fallback();
                const auto found = cache.find(registers);
                if (found != cache.end()) {
                    ++metrics.duplicate_cache_hits; reused.push_back(found->second); continue;
                }
                const auto count = batch.size();
                admit(decodeOpllPatch(registers), batch);
                novel = batch.size() != count;
            }
        }
        if (!scoreOpllSubBatches(batch, executor, batch_size, weight, coordinator)) return false;
        for (const auto& entry : batch) cache.emplace(entry.registers, entry);
        scored.insert(scored.end(), batch.begin(), batch.end());
        // Elitism: the previous best eight always participate; selection
        // sees completed scores only, ordered by (score, encoded registers).
        if (population.size() > gp::elite_count) population.resize(gp::elite_count);
        population.insert(population.end(), reused.begin(), reused.end());
        population.insert(population.end(), batch.begin(), batch.end());
        population = retain(std::move(population));
        ++metrics.generations;
    }
    return true;
}
