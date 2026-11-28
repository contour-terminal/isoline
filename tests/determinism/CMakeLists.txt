# SPDX-License-Identifier: Apache-2.0

# The two-process determinism check: isoline_determinism prints the digest of each module's compiled code, and
# CompareRuns.cmake runs it twice and compares, so that output equal within one process (Compiler.isDeterministic)
# is also equal across two, whose heaps and address-space layouts differ. It compiles for x86-64 on every host:
# a cross build runs it under its emulator.
add_executable(isoline_determinism DeterminismMain.cpp)
target_link_libraries(isoline_determinism PRIVATE isoline::core isoline::text)
isoline_apply_target_policy(isoline_determinism TEST)
set(isolineDeterminismModules
    "${CMAKE_CURRENT_SOURCE_DIR}/calls.wat"
    "${CMAKE_CURRENT_SOURCE_DIR}/memory.wat")
add_test(NAME determinism-two-processes
         COMMAND ${CMAKE_COMMAND} "-DPROGRAM=$<TARGET_FILE:isoline_determinism>"
                 "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
                 "-DMODULES=${isolineDeterminismModules}"
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/CompareRuns.cmake")
set_tests_properties(determinism-two-processes PROPERTIES LABELS unit)
