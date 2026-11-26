# SPDX-License-Identifier: Apache-2.0

# isoline::numerics: the typed scalar kernels every executor evaluates numeric instructions with.
# Header-only; the float kernels rely on the floating-point contraction row that
# isoline_apply_target_policy() gives every Isoline target (cmake/IsolinePedantic.cmake).
target_sources(isoline_core
    PUBLIC FILE_SET HEADERS
        BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
        FILES
            CanonicalNan.hpp
            Float.hpp
            Integer.hpp)
