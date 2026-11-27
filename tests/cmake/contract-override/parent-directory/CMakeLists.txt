# SPDX-License-Identifier: Apache-2.0
#
# The intermediate directory of the contract-override fixture's parent-directory case: its flags reach
# Isoline's directory, and are invisible from the top-level one.

string(APPEND CMAKE_CXX_FLAGS " -fcontract-evaluation-semantic=observe")
add_subdirectory("${ISOLINE_SOURCE_DIR}" isoline)
