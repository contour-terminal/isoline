// SPDX-License-Identifier: Apache-2.0
//
// The contracts seam, native: what <isoline/meta/Check.hpp> forwards to where contracts take the native
// path (cmake/IsolineLanguageFeatures.cmake; docs/language-features.md). Private: never installed, and
// included by implementation files only, so that every contract compiles under Isoline's own flags.
// meta/fallback/contracts/Check.hpp is the other path, and defines the same macros.
//
// Each check is a C++26 contract assertion, checked under the pinned `enforce` semantic, in a function
// the compiler proves noexcept: a violation handler that throws, which an embedder may install, then
// ends the process where the check fails, as Require() would. In a function that may throw it would let
// the caller continue past the check, so there the expansion does not compile, and the site keeps
// Require(), which this header brings too.
//
// The checks are written `do { switch (0) default: { … } } while (false)`. GCC 16.2.1 evaluates no
// contract_assert that is the first statement of a `for`, `while` or `do` body, nor one inside
// `if (true) { … }` there or as such a body (measured 2026-11-27; docs/language-features.md).
// Behind `switch (0) default:` it is evaluated in every position the canary pins: a function's first
// statement, the first statement of a for, while, do and range-for body, and a braceless if or else.
#pragma once

#include <core/Assert.hpp>

#include <meta>

/// Checks @p condition as a contract assertion in a noexcept function; a compile error elsewhere.
#define ISOLINE_DETAIL_CONTRACT_ASSERT(condition)                                                    \
    do                                                                                               \
    {                                                                                                \
        switch (0)                                                                                   \
        default: {                                                                                   \
            static_assert(                                                                           \
                std::meta::is_noexcept(std::meta::access_context::current().scope()),                \
                "ISOLINE_PRECONDITION/ISOLINE_INVARIANT need a noexcept function: keep Require and " \
                "add a throwing-site row");                                                          \
            contract_assert(condition);                                                              \
        }                                                                                            \
    } while (false)

/// Checks an entry condition of a noexcept function: among its first statements, reading only its
/// parameters and `*this`. The condition only reads.
#define ISOLINE_PRECONDITION(condition) ISOLINE_DETAIL_CONTRACT_ASSERT(condition)

/// Checks any other condition of a noexcept function. The condition only reads.
#define ISOLINE_INVARIANT(condition) ISOLINE_DETAIL_CONTRACT_ASSERT(condition)
