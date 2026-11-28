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

# The oracle itself (label oracle, so `ctest -L oracle` runs it alone), registered wherever an llvm-mc is at
# hand: -DISOLINE_LLVM_MC=<path>, or else the pinned one when scripts/fetch-tools.py has fetched it into its
# cache (`fetch-tools.py --tool llvm-mc`; nothing is downloaded at configure time). The offline ctest never
# needs it. An llvm-mc that is given must be usable: a path that names nothing, or no Python to run the
# checker, fails the configure rather than leave a CI lane with no oracle to run. A cross build's samples run
# under its emulator, so the oracle runs on native trees only.
set(ISOLINE_LLVM_MC "" CACHE FILEPATH
    "llvm-mc for the x86-64 encoding oracle (tests/encoding); empty: the pinned one, once fetch-tools.py fetched it")
find_package(Python3 COMPONENTS Interpreter)
set(isolineLlvmMc "${ISOLINE_LLVM_MC}")
if(ISOLINE_LLVM_MC)
    if(NOT EXISTS "${ISOLINE_LLVM_MC}")
        message(FATAL_ERROR "ISOLINE_LLVM_MC=${ISOLINE_LLVM_MC} names no file")
    endif()
    if(NOT Python3_Interpreter_FOUND)
        message(FATAL_ERROR "ISOLINE_LLVM_MC is given, but there is no Python to run scripts/check-encodings.py")
    endif()
    if(CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "ISOLINE_LLVM_MC is given to a cross build, whose samples cannot run on this host")
    endif()
elseif(Python3_Interpreter_FOUND AND NOT CMAKE_CROSSCOMPILING)
    execute_process(COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/scripts/fetch-tools.py"
                            --tool llvm-mc --cached
                    OUTPUT_VARIABLE isolineLlvmMc OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE isolineLlvmMcLookup)
    if(NOT isolineLlvmMcLookup EQUAL 0)
        set(isolineLlvmMc "")
    endif()
endif()
if(isolineLlvmMc)
    message(STATUS "The x86-64 encoding oracle (ctest -L oracle) runs ${isolineLlvmMc}")
    add_test(NAME encoding-oracle
             COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/scripts/check-encodings.py"
                     --samples "$<TARGET_FILE:isoline_encoding_samples>"
                     --llvm-mc "${isolineLlvmMc}"
                     --golden "${CMAKE_CURRENT_SOURCE_DIR}/x86_64.golden")
    set_tests_properties(encoding-oracle PROPERTIES LABELS oracle)
endif()
