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
#include <string_view>
#include <type_traits>

#include "AllocationCounter.hpp"
#include <isoline/c/isoline.h>
#include <isoline/c/testing/TypeElements.hpp>

using isoline::testing::AllocationLimit;
using isoline::testing::AllocationScope;

namespace
{

/// The copy of a vector of two elements of the family @p Vector, given memory for its array and not for the
/// first element's copy: the empty vector, with nothing retained, and the original as it was.
template <typename Vector, auto New, auto Copy, auto Delete>
void checkCopyOutOfMemory(std::string_view family)
{
    INFO("wasm_" << family << "_vec_t");
    using Element = std::remove_pointer_t<decltype(Vector::data)>;
    auto const elements = std::array { isoline::c::testing::makeElement<Element>(),
                                       isoline::c::testing::makeElement<Element>() };
    auto original = Vector {};
    New(&original, elements.size(), elements.data());
    REQUIRE(original.size == 2);
    auto copy = Vector { .size = 7, .data = nullptr };
    auto retained = std::size_t { 1 };
    {
        auto const scope = AllocationScope {};
        {
            auto const arrayOnly = AllocationLimit { 2 * sizeof(Element) };
            Copy(&copy, &original);
        }
        retained = scope.retainedBytes();
    }
    CHECK(retained == 0);
    CHECK(copy.size == 0);
    CHECK(copy.data == nullptr);
    CHECK(original.size == 2);
    Delete(&original);
}

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
        element = wasm_valtype_new(WASM_I32);

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

TEST_CASE("Vectors.copyFailingMidwayLeavesNothing", "[c-api][host-memory][vectors]")
{
    auto original = wasm_valtype_vec_t {};
    wasm_valtype_vec_new_uninitialized(&original, 3);
    REQUIRE(original.size == 3);
    for (auto*& element: std::span { original.data, original.size })
        element = wasm_valtype_new(WASM_F32);

    // What the copy's array and one element's copy take, measured rather than derived from sizeof: the copy
    // of a vector of one element allocates both.
    auto one = wasm_valtype_vec_t {};
    wasm_valtype_vec_new(&one, 1, std::array { wasm_valtype_new(WASM_F32) }.data());
    auto measured = wasm_valtype_vec_t {};
    auto oneCopyBytes = std::size_t { 0 };
    {
        auto const scope = AllocationScope {};
        wasm_valtype_vec_copy(&measured, &one);
        oneCopyBytes = scope.retainedBytes();
    }
    REQUIRE(measured.size == 1);
    wasm_valtype_vec_delete(&measured);
    wasm_valtype_vec_delete(&one);
    auto const elementBytes = oneCopyBytes - sizeof(wasm_valtype_t*);

    // Enough for the array of three and the first element's copy, not the second's: the first is deleted
    // again, the copy is the empty vector, and the original is as it was.
    auto copy = wasm_valtype_vec_t { .size = 7, .data = nullptr };
    auto retained = std::size_t { 1 };
    {
        auto const scope = AllocationScope {};
        {
            auto const arrayAndOne = AllocationLimit { (3 * sizeof(wasm_valtype_t*)) + elementBytes };
            wasm_valtype_vec_copy(&copy, &original);
        }
        retained = scope.retainedBytes();
    }
    CHECK(retained == 0);
    CHECK(copy.size == 0);
    CHECK(copy.data == nullptr);
    REQUIRE(original.size == 3);
    CHECK(wasm_valtype_kind(original.data[2]) == WASM_F32);
    wasm_valtype_vec_delete(&original);
}

TEST_CASE("Vectors.everyTypeFamilyCopyOutOfMemoryIsEmpty", "[c-api][host-memory][vectors]")
{
    // Each type family's copy, whose element's copy runs out of memory: the boundary() of every family's
    // wasm_<name>_vec_copy() makes the copy the empty vector.
#define ISOLINE_CHECK_FAMILY(name)                \
    checkCopyOutOfMemory<wasm_##name##_vec_t,     \
                         &wasm_##name##_vec_new,  \
                         &wasm_##name##_vec_copy, \
                         &wasm_##name##_vec_delete>(#name)
    ISOLINE_CHECK_FAMILY(valtype);
    ISOLINE_CHECK_FAMILY(functype);
    ISOLINE_CHECK_FAMILY(globaltype);
    ISOLINE_CHECK_FAMILY(tabletype);
    ISOLINE_CHECK_FAMILY(memorytype);
    ISOLINE_CHECK_FAMILY(tagtype);
    ISOLINE_CHECK_FAMILY(externtype);
    ISOLINE_CHECK_FAMILY(importtype);
    ISOLINE_CHECK_FAMILY(exporttype);
#undef ISOLINE_CHECK_FAMILY
}
