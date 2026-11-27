// SPDX-License-Identifier: Apache-2.0
//
// Vectors out of host memory: under an allocator that refuses (tests/memory/AllocationCounter.cpp, whose
// replaced operator new is the whole program's, hence this binary of its own), every way of making a vector
// gives wasm.h's empty vector, nothing escapes as an exception, nothing leaks, and the process carries on.
// Each refusal is scoped tightly, and nothing is asserted inside it: Catch2 allocates.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <span>

#include "AllocationCounter.hpp"
#include <isoline/c/detail/Handles.hpp>
#include <isoline/c/isoline.h>

using isoline::testing::AllocationLimit;
using isoline::testing::AllocationScope;

namespace
{

/// A value type that copies itself, as Task 9's will.
class TestValtype final: public wasm_valtype_t
{
  public:
    TestValtype() = default;
    TestValtype(TestValtype const&) = default;
    TestValtype(TestValtype&&) = delete;
    TestValtype& operator=(TestValtype const&) = delete;
    TestValtype& operator=(TestValtype&&) = delete;
    ~TestValtype() override = default;

    [[nodiscard]] auto clone() const -> std::unique_ptr<isoline::c::detail::Object> override
    {
        return std::make_unique<TestValtype>(*this);
    }
};

} // namespace

TEST_CASE("Vectors.allocationFailureIsEmpty", "[c-api][host-memory][vectors]")
{
    auto bytes = wasm_byte_vec_t { .size = 7, .data = nullptr };
    auto values = wasm_val_vec_t { .size = 7, .data = nullptr };
    auto copied = wasm_byte_vec_t { .size = 7, .data = nullptr };
    auto const text = std::array<wasm_byte_t, 4> { 'w', 'a', 's', 'm' };
    auto const source =
        wasm_byte_vec_t { .size = text.size(), .data = const_cast<wasm_byte_t*>(text.data()) };
    auto retained = std::size_t { 1 };
    {
        auto const scope = AllocationScope {};
        {
            auto const refuseAll = AllocationLimit { 0 };
            wasm_byte_vec_new_uninitialized(&bytes, 16);
            wasm_val_vec_new(&values, 1, std::array<wasm_val_t, 1> {}.data());
            wasm_byte_vec_copy(&copied, &source);
        }
        retained = scope.retainedBytes();
    }
    CHECK(retained == 0);
    CHECK(bytes.size == 0);
    CHECK(bytes.data == nullptr);
    CHECK(values.size == 0);
    CHECK(values.data == nullptr);
    CHECK(copied.size == 0);
    CHECK(copied.data == nullptr);

    // The process carries on, and with memory the same vectors are made as usual.
    wasm_byte_vec_copy(&copied, &source);
    CHECK(copied.size == text.size());
    wasm_byte_vec_delete(&copied);
}

TEST_CASE("Vectors.elementCopyOutOfMemoryIsEmpty", "[c-api][host-memory][vectors]")
{
    // The copy's array is allocated, and its first element's copy is not: std::bad_alloc stops at the
    // boundary, the array is freed again, and the copy is the empty vector.
    auto original = wasm_valtype_vec_t {};
    wasm_valtype_vec_new_uninitialized(&original, 3);
    REQUIRE(original.size == 3);
    for (auto*& element: std::span { original.data, original.size })
        element = new TestValtype {};

    auto copy = wasm_valtype_vec_t { .size = 7, .data = nullptr };
    auto retained = std::size_t { 1 };
    {
        auto const scope = AllocationScope {};
        {
            auto const arrayOnly = AllocationLimit { 3 * sizeof(wasm_valtype_t*) };
            wasm_valtype_vec_copy(&copy, &original);
        }
        retained = scope.retainedBytes();
    }
    CHECK(retained == 0);
    CHECK(copy.size == 0);
    CHECK(copy.data == nullptr);
    CHECK(original.size == 3);
    wasm_valtype_vec_delete(&original);
}
