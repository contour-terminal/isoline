# SPDX-License-Identifier: Apache-2.0

# The x86-64 encoding oracle's samples: every row of src/isoline/compiler/x86_64/Encodings.def, encoded and
# rendered as the text it is meant to be, one `hex<TAB>intended<TAB>row` line per sample.
# scripts/check-encodings.py has llvm-mc disassemble the hex and compares its text with the intended one;
# x86_64.golden is the cached copy of that agreement, which Encoder.matchesTheCachedAgreement checks offline.
add_executable(isoline_encoding_samples
    EncodingSamplesMain.cpp
    "${PROJECT_SOURCE_DIR}/src/isoline/compiler/x86_64/EncodingSamples.cpp")
target_link_libraries(isoline_encoding_samples PRIVATE isoline::core)
isoline_apply_target_policy(isoline_encoding_samples TEST)

# The oracle itself, where an llvm-mc is given (-DISOLINE_LLVM_MC=<path>, the LLVM release fetch-tools.py
# pins): never part of an offline run. A cross build's samples run under its emulator, so the oracle runs on
# native trees only.
set(ISOLINE_LLVM_MC "" CACHE FILEPATH "llvm-mc for the x86-64 encoding oracle (tests/encoding)")
find_package(Python3 COMPONENTS Interpreter)
if(ISOLINE_LLVM_MC AND Python3_Interpreter_FOUND AND NOT CMAKE_CROSSCOMPILING)
    add_test(NAME encoding-oracle
             COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/scripts/check-encodings.py"
                     --samples "$<TARGET_FILE:isoline_encoding_samples>"
                     --llvm-mc "${ISOLINE_LLVM_MC}"
                     --golden "${CMAKE_CURRENT_SOURCE_DIR}/x86_64.golden")
    set_tests_properties(encoding-oracle PROPERTIES LABELS oracle)
endif()
