# SPDX-License-Identifier: Apache-2.0

# Contour's bundled clock extension as a component (plan P11 Task 22), hosted on a test double of the WASI 0.2 its
# preview-1 adapter imports. The double is test code: Isoline ships no WASI 0.2, so neither isoline_component nor
# any other library links it.
add_executable(isoline_component_contour_test ContourClock_test.cpp Wasi02Subset.cpp Wasi02Subset.hpp)
target_link_libraries(isoline_component_contour_test PRIVATE isoline::component isoline::component_text isoline::text
                      Catch2::Catch2WithMain)
isoline_apply_target_policy(isoline_component_contour_test TEST)
# clock.wasm, wasi-subset.wat and README.md are read from the source tree.
target_compile_definitions(isoline_component_contour_test PRIVATE ISOLINE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME isoline_component_contour_test COMMAND isoline_component_contour_test)
set_tests_properties(isoline_component_contour_test PROPERTIES LABELS unit TIMEOUT 600)
