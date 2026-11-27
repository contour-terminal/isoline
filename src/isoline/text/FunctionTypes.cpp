// SPDX-License-Identifier: Apache-2.0
#include <isoline/text/FunctionTypes.hpp>

#include <cstdint>
#include <utility>
#include <vector>

#include <isoline/binary/Types.hpp>

namespace isoline::text
{

FunctionTypes::FunctionTypes(std::vector<binary::FuncType>& types):
    _types { &types }, _firstIndices { Less { .types = &types, .comparisons = &_comparisons } }
{
}

uint32_t FunctionTypes::define(binary::FuncType type)
{
    _types->push_back(std::move(type));
    auto const index = static_cast<uint32_t>(_types->size() - 1);
    _firstIndices.insert(index);
    return index;
}

uint32_t FunctionTypes::use(binary::FuncType type)
{
    _types->push_back(std::move(type));
    auto const candidate = static_cast<uint32_t>(_types->size() - 1);
    auto const [found, isNew] = _firstIndices.insert(candidate);
    if (!isNew)
        _types->pop_back();
    return *found;
}

} // namespace isoline::text
