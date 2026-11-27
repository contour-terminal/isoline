# SPDX-License-Identifier: Apache-2.0
#
# The in-tree consumers of module isoline: they import it, and include no Isoline header.

add_executable(isoline_module_test ImportConsumer_test.cpp)
# The add module's bytes, which a consumer that only imports may include (tests/consumer/AddModule.hpp).
target_include_directories(isoline_module_test PRIVATE "${PROJECT_SOURCE_DIR}/tests/consumer")
target_link_libraries(isoline_module_test PRIVATE Catch2::Catch2WithMain)
isoline_add_module_consumer(isoline_module_test isoline)
isoline_apply_target_policy(isoline_module_test TEST)
# E2 (D13), row import-only of ISOLINE_MODULE_TIDY_FALSE_POSITIVES: readability-redundant-declaration.
isoline_lift_module_tidy_checks(isoline_module_test import-only)
add_test(NAME isoline_module_test COMMAND isoline_module_test)
set_tests_properties(isoline_module_test PROPERTIES LABELS "unit;modules" TIMEOUT 300)

# clang-tidy-module-tripwire's import-only source (E2): compiled, so that its module map exists, and linted
# as its kind's row says.
add_library(isoline_module_tidy_tripwire OBJECT tidy-tripwire/ImportOnly.cpp)
isoline_add_module_consumer(isoline_module_tidy_tripwire isoline)
isoline_apply_target_policy(isoline_module_tidy_tripwire)
# E2 (D13), row import-only of ISOLINE_MODULE_TIDY_FALSE_POSITIVES: readability-redundant-declaration.
isoline_lift_module_tidy_checks(isoline_module_tidy_tripwire import-only)

# The tripwires of E1 and E2: one source of each kind ISOLINE_MODULE_TIDY_FALSE_POSITIVES names.
get_property(isolineInterfaceUnit GLOBAL PROPERTY ISOLINE_MODULE_isoline_SOURCE)
isoline_add_module_exception_tripwires(
    TIDY_SOURCES
        "interface-unit" "${isolineInterfaceUnit}"
        "import-only" "${CMAKE_CURRENT_SOURCE_DIR}/tidy-tripwire/ImportOnly.cpp")
