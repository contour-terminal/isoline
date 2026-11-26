// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <type_traits>

#include <isoline/binary/Module.hpp>
#include <isoline/binary/Types.hpp>

using isoline::binary::AbstractHeapType;
using isoline::binary::ExternKind;
using isoline::binary::ExternRef;
using isoline::binary::FuncRef;
using isoline::binary::I32;
using isoline::binary::I64;
using isoline::binary::Import;
using isoline::binary::Instruction;
using isoline::binary::Module;
using isoline::binary::TableType;
using isoline::binary::ValueKind;
using isoline::binary::ValueType;

TEST_CASE("Module.importedCount")
{
    auto wasm = Module {};
    CHECK(wasm.importedCount(ExternKind::Func) == 0);

    wasm.imports.push_back(Import { .module = "env", .name = "f", .kind = ExternKind::Func });
    wasm.imports.push_back(Import { .module = "env", .name = "m", .kind = ExternKind::Memory });
    wasm.imports.push_back(Import { .module = "env", .name = "g", .kind = ExternKind::Func });
    CHECK(wasm.importedCount(ExternKind::Func) == 2);
    CHECK(wasm.importedCount(ExternKind::Memory) == 1);
    CHECK(wasm.importedCount(ExternKind::Table) == 0);
}

TEST_CASE("Module.instructionStaysCompact")
{
    // A decoded body is bounded at 40 bytes per instruction; see the static_assert in Module.hpp.
    CHECK(sizeof(Instruction) == 40);
    CHECK(std::is_trivially_copyable_v<Instruction>);
    CHECK(Instruction {}.lane == 0);
}

TEST_CASE("Types.referenceTypes")
{
    CHECK(FuncRef != ExternRef);
    CHECK(FuncRef.kind == ValueKind::Ref);
    CHECK(FuncRef.heap.kind == AbstractHeapType::Func);
    CHECK(ExternRef.heap.kind == AbstractHeapType::Extern);
    CHECK(I32 != I64);
    CHECK(I32 == ValueType {});
    CHECK(TableType {}.element == FuncRef);
}
