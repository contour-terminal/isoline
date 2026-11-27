// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>

#include <isoline/binary/MemoryAccess.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/fastinterpreter/Code.hpp>
#include <isoline/fastinterpreter/LoweringTable.hpp>

/// Which suite runs which operations: the vector operations, the v128 accesses and wide arithmetic are
/// fastinterpreter/Simd_test.cpp's, every other FastInterpreter.everyOperationAgreesWithTheReference's.
namespace isoline::fastinterpreter::testing
{

/// Whether Simd_test.cpp runs @p id against the reference: an operation of the Simd family, v128.const's,
/// a v128 load's or store's, or a wide-arithmetic operation.
/// @param id The operation.
/// @return Whether it is one of those.
[[nodiscard]] inline bool isVectorOperation(OperationId id)
{
    if (operationRow(id).family == OperationFamily::Simd || id == OperationId::Const128)
        return true;
    if (std::ranges::find(WideArithmeticTable, id, &WideArithmeticRow::operation)
        != WideArithmeticTable.end())
        return true;
    return std::ranges::any_of(LoweringRows, [id](LoweringRow const& row) {
        return (row.form == OperationForm::Load || row.form == OperationForm::Store)
               && (row.operation == id || row.memory64 == id)
               && binary::memoryAccessOf(row.opcode)->value == binary::ValueKind::V128;
    });
}

} // namespace isoline::fastinterpreter::testing
