// SPDX-License-Identifier: Apache-2.0
//
// module-import-then-include's probe (D7): `import isoline;`, then an Isoline header, in one TU. Both
// orders conform, but this one is measured rather than promised: the test expects what its compiler's row
// of ISOLINE_MODULE_IMPORT_THEN_INCLUDE says (tests/modules/CMakeLists.txt), and docs/modules.md shows the
// table.

import isoline;

#include <isoline/api/Store.hpp>

// Names that both the module and the header declare, used once both are visible.
static_assert(isoline::FeatureCount == isoline::FeatureTable.size());
