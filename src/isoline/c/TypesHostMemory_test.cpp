// SPDX-License-Identifier: Apache-2.0
//
// Types out of host memory, under an allocator that refuses (tests/memory/AllocationCounter.cpp, the whole
// program's, hence the binary of its own): a type made or copied is NULL, an isoline_ function returns the
// immortal HOST_MEMORY_EXHAUSTED error, nothing leaks, and what a refused function was given stays the
// caller's (isoline.h). Each refusal is scoped tightly, and nothing is asserted inside it: Catch2 allocates.
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <new>

#include "AllocationCounter.hpp"
#include <isoline/binary/Types.hpp>
#include <isoline/c/detail/Convert.hpp>
#include <isoline/c/isoline.h>

using isoline::testing::AllocationLimit;
using isoline::testing::AllocationScope;

TEST_CASE("Types.outOfMemoryIsNull", "[c-api][host-memory][types]")
{
    auto* const original = wasm_functype_new_2_1(
        wasm_valtype_new(WASM_I32), wasm_valtype_new(ISOLINE_VALKIND_V128), wasm_valtype_new(WASM_FUNCREF));
    REQUIRE(original != nullptr);
    auto params = wasm_valtype_vec_t {};
    auto results = wasm_valtype_vec_t {};
    wasm_valtype_vec_new_uninitialized(&params, 1);
    wasm_valtype_vec_new_empty(&results);
    REQUIRE(params.size == 1);
    params.data[0] = wasm_valtype_new(WASM_F32);
    auto* const tableElement = wasm_valtype_new(WASM_EXTERNREF);
    auto* const globalContent = wasm_valtype_new(WASM_I64);
    auto* const tagPayload = wasm_functype_new_0_0();
    auto* const exportedType = wasm_functype_as_externtype(wasm_functype_new_0_0());
    auto name = wasm_name_t {};
    wasm_name_new_from_string(&name, "f");
    auto importModule = wasm_name_t {};
    auto importName = wasm_name_t {};
    wasm_name_new_from_string(&importModule, "env");
    wasm_name_new_from_string(&importName, "g");
    auto* const importedType =
        wasm_globaltype_as_externtype(wasm_globaltype_new(wasm_valtype_new(WASM_I32), WASM_VAR));
    // The arguments of an inline helper of wasm.h, made before the helper runs as a caller's temporaries are.
    auto* const param = wasm_valtype_new(WASM_I32);
    auto* const result = wasm_valtype_new(ISOLINE_VALKIND_EXNREF);
    auto const limits = wasm_limits_t { .min = 1, .max = 2 };

    wasm_functype_t* copy = nullptr;
    wasm_functype_t* made = nullptr;
    wasm_functype_t* helped = nullptr;
    wasm_valtype_t* valtype = nullptr;
    wasm_tabletype_t* table = nullptr;
    wasm_globaltype_t* global = nullptr;
    wasm_tagtype_t* tag = nullptr;
    wasm_exporttype_t* exported = nullptr;
    wasm_importtype_t* imported = nullptr;
    auto const liveBefore = isoline::testing::allocationCounts().liveBytes;
    {
        // Each call under a budget of its own: the deletions of one refusal would otherwise make room for the
        // allocations of the next.
        {
            auto const refuseAll = AllocationLimit { 0 };
            copy = wasm_functype_copy(original);
        }
        {
            auto const refuseAll = AllocationLimit { 0 };
            made = wasm_functype_new(&params, &results);
        }
        {
            auto const refuseAll = AllocationLimit { 0 };
            helped = wasm_functype_new_1_1(param, result);
        }
        {
            auto const refuseAll = AllocationLimit { 0 };
            valtype = wasm_valtype_new(WASM_I64);
        }
        {
            auto const refuseAll = AllocationLimit { 0 };
            table = wasm_tabletype_new(tableElement, &limits);
        }
        {
            auto const refuseAll = AllocationLimit { 0 };
            global = wasm_globaltype_new(globalContent, WASM_CONST);
        }
        {
            auto const refuseAll = AllocationLimit { 0 };
            tag = wasm_tagtype_new(tagPayload);
        }
        {
            auto const refuseAll = AllocationLimit { 0 };
            exported = wasm_exporttype_new(&name, exportedType);
        }
        {
            auto const refuseAll = AllocationLimit { 0 };
            imported = wasm_importtype_new(&importModule, &importName, importedType);
        }
    }
    auto const liveAfter = isoline::testing::allocationCounts().liveBytes;
    CHECK(copy == nullptr);
    CHECK(made == nullptr);
    CHECK(helped == nullptr);
    CHECK(valtype == nullptr);
    CHECK(table == nullptr);
    CHECK(global == nullptr);
    CHECK(tag == nullptr);
    CHECK(exported == nullptr);
    CHECK(imported == nullptr);
    CHECK(importModule.size == 0);
    CHECK(importName.size == 0);
    // Refused for want of memory, every constructor took what it was given, and deleted it: the caller's
    // vectors are empty, and what they held, the elements, the payload, the type and the name are freed
    // (LeakSanitizer holds the binary to it; the caller deletes none of them).
    CHECK(params.size == 0);
    CHECK(params.data == nullptr);
    CHECK(name.size == 0);
    CHECK(liveAfter < liveBefore);

    // The process carries on, and with memory types are made as usual.
    made = wasm_functype_new_1_0(wasm_valtype_new(WASM_F32));
    CHECK(made != nullptr);
    wasm_functype_delete(made);
    wasm_functype_delete(original);
}

TEST_CASE("Types.isolineOutOfMemoryIsTheImmortalError", "[c-api][host-memory][types]")
{
    auto limits = isoline_limits64_t {};
    limits.struct_size = sizeof limits;
    limits.minimum = uint64_t { 1 } << 33;
    auto* const element = wasm_valtype_new(WASM_FUNCREF);
    wasm_memorytype_t* memory = nullptr;
    wasm_tabletype_t* table = nullptr;
    isoline_error_t* memoryError = nullptr;
    isoline_error_t* tableError = nullptr;
    auto retained = std::size_t { 1 };
    {
        auto const scope = AllocationScope {};
        {
            auto const refuseAll = AllocationLimit { 0 };
            memoryError = isoline_memorytype_new(&limits, 64, 16, &memory);
            tableError = isoline_tabletype_new(element, &limits, 64, &table);
        }
        retained = scope.retainedBytes();
    }
    CHECK(retained == 0);
    CHECK(isoline_error_code(memoryError) == ISOLINE_ERROR_HOST_MEMORY_EXHAUSTED);
    CHECK(isoline_error_code(tableError) == ISOLINE_ERROR_HOST_MEMORY_EXHAUSTED);
    CHECK(memory == nullptr);
    CHECK(table == nullptr);
    isoline_error_delete(memoryError);
    isoline_error_delete(tableError);
    CHECK(wasm_valtype_kind(element) == WASM_FUNCREF);
    wasm_valtype_delete(element);
}

TEST_CASE("Types.copyLosingAVectorIsNull", "[c-api][host-memory][types]")
{
    // What the function type object alone takes, measured: the copy of one of no parameters or results
    // allocates it and nothing else.
    auto* const bare = wasm_functype_new_0_0();
    REQUIRE(bare != nullptr);
    auto objectBytes = std::size_t { 0 };
    {
        auto const scope = AllocationScope {};
        wasm_functype_delete(wasm_functype_copy(bare));
        objectBytes = scope.allocatedBytes();
    }
    wasm_functype_delete(bare);
    REQUIRE(objectBytes > 0);

    // Enough for the copy's object, not for its parameters' array: the copy is NULL, not a function type that
    // lost its parameters.
    auto* const original = wasm_functype_new_1_0(wasm_valtype_new(WASM_I32));
    REQUIRE(original != nullptr);
    wasm_functype_t* copy = nullptr;
    auto retained = std::size_t { 1 };
    {
        auto const scope = AllocationScope {};
        {
            auto const objectOnly = AllocationLimit { objectBytes };
            copy = wasm_functype_copy(original);
        }
        retained = scope.retainedBytes();
    }
    CHECK(retained == 0);
    CHECK(copy == nullptr);
    wasm_functype_delete(copy);
    wasm_functype_delete(original);
}

TEST_CASE("Types.conversionOutOfMemoryThrows", "[c-api][host-memory][types]")
{
    // detail::toCType() of a function type throws std::bad_alloc when its vectors cannot be allocated, as its
    // callers' boundary() expects, and leaves nothing behind.
    auto const type = isoline::binary::FuncType { .params = { isoline::binary::I32 }, .results = {} };
    auto threw = false;
    auto retained = std::size_t { 1 };
    {
        auto const scope = AllocationScope {};
        {
            auto const refuseAll = AllocationLimit { 0 };
            try
            {
                static_cast<void>(isoline::c::detail::toCType(type));
            }
            catch (std::bad_alloc const&)
            {
                threw = true;
            }
        }
        retained = scope.retainedBytes();
    }
    CHECK(threw);
    CHECK(retained == 0);
}
