// SPDX-License-Identifier: Apache-2.0
// A public header, beside the set's own, that the library's header set forgot: the module would not
// cover it, and nothing else would notice.
#pragma once

namespace isoline::api
{

/// Forgotten by the header set.
struct Forgotten
{
};

} // namespace isoline::api
