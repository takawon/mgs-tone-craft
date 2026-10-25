// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <string>
#include <string_view>

namespace mgstc::engine::detail {

// Control/worker exception boundary only. Standard exception messages can use
// the Windows active code page even when the application stores UTF-8 text.
[[nodiscard]] std::string normaliseExceptionTextUtf8(
    std::string_view text, unsigned fallback_code_page = 0);

} // namespace mgstc::engine::detail
