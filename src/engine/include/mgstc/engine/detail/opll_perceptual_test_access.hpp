// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "mgstc/engine/wave_import.hpp"

namespace mgstc::engine::detail {

OpllFullScoreBreakdown compareOpllFullPcmForRegression(std::span<const float> reference,
    std::span<const float> candidate, float fundamental_hz, std::size_t key_off);

OpllPublicationDecision checkOpllPerceptualGateForRegression(
    const OpllFullScoreBreakdown& previous, const OpllFullScoreBreakdown& candidate);

bool checkOpllPublicationHistoryForRegression();

std::vector<float> renderOpllFullPcmForRegression(const OpllPatchParameters& patch,
    float fundamental_hz, std::size_t frames, std::size_t key_off);

} // namespace mgstc::engine::detail
