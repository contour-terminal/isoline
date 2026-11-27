// SPDX-License-Identifier: Apache-2.0
//
// The header-check-sealed fixture's one installed header.
#pragma once

/// Returns the fixture's answer.
/// @return 42.
[[nodiscard]] auto fixtureAnswer() noexcept -> int;
