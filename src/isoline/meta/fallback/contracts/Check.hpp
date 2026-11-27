// SPDX-License-Identifier: Apache-2.0
//
// The contracts seam, fallback: what <isoline/meta/Check.hpp> forwards to where contracts take the
// fallback path (cmake/IsolineLanguageFeatures.cmake; docs/language-features.md). Private: never
// installed, and included by implementation files only. meta/contracts/Check.hpp is the native path, and
// defines the same macros; here each check is the Require() it was before contracts existed.
#pragma once

#include <core/Assert.hpp>

/// Checks an entry condition of a noexcept function: among its first statements, reading only its
/// parameters and `*this`. The condition only reads.
#define ISOLINE_PRECONDITION(condition) Require(condition)

/// Checks any other condition of a noexcept function. The condition only reads.
#define ISOLINE_INVARIANT(condition) Require(condition)
