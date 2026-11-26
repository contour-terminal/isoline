// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <span>

#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::runtime
{

/// Whether @p values are of @p types in @p store: what the host boundary checks of arguments, host
/// results and exception payloads, so that a value of the wrong kind, a reference outside its type's
/// heap, or an address the store does not hold is refused rather than reaching a precondition.
///
/// A count mismatch is reported first, at the first position the shorter list lacks: values that
/// do not pair up with their types are not compared one by one. Otherwise the first value that is
/// not of its kind, or a reference outside its type's heap or naming what the store does not hold,
/// is reported at its position. A non-null reference is accepted by its hierarchy's top, and by a
/// concrete type below it whose TypeId is its function's; never by the bottom.
/// A reference carries no store identity, so a reference of another store whose address happens to
/// exist in this one is not told apart; it is still within this store, so using it stays safe.
///
/// Precondition: every reference type in @p types is in store form; its heap is not bot (checked).
/// @param store The store the values are for, whose functions and exceptions references name.
/// @param values The values.
/// @param types The types they must be of, in store form (concrete heaps name TypeIds).
/// @return Nothing, or the first mismatch.
[[nodiscard]] std::expected<void, ValueMismatch> checkValues(
    Store const& store, std::span<Value const> values, std::span<binary::ValueType const> types) noexcept;

} // namespace isoline::runtime
