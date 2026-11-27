// SPDX-License-Identifier: Apache-2.0
#include <cstdlib>

#include "ChildAbort.hpp"

namespace isoline::differential::testing
{

void endAbnormally()
{
    std::abort();
}

} // namespace isoline::differential::testing
