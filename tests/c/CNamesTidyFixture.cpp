// SPDX-License-Identifier: Apache-2.0
//
// Linted by the clang-tidy-c-names test, never compiled into a binary: the C++ that implements the C API
// spells wasm.h's and the C ABI's names as they are mandated, and the one .clang-tidy accepts them there
// (plan P17a, I4) while every other name keeps the house style.

extern "C" int isoline_fixture_add(int isoline_left, int wasm_right) noexcept;

namespace
{

struct wasm_fixture_t
{
    int isoline_count;
};

} // namespace

extern "C" int isoline_fixture_add(int isoline_left, int wasm_right) noexcept
{
    auto const fixture = wasm_fixture_t { .isoline_count = isoline_left };
    return fixture.isoline_count + wasm_right;
}
