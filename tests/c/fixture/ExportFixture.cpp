// SPDX-License-Identifier: Apache-2.0
#include "ExportFixture.hpp"

#include <numeric>
#include <vector>

namespace fixture
{

int leak(int value)
{
    auto values = std::vector<int> {};
    values.push_back(value);
    values.push_back(1);
    return std::accumulate(values.begin(), values.end(), 0);
}

} // namespace fixture

extern "C" int isoline_fixture(int value) noexcept
{
    return fixture::leak(value);
}
