# SPDX-License-Identifier: Apache-2.0

# Runs the determinism program twice, in two processes, and fails unless both printed the same digests:
# CompiledCode is byte-identical across processes (P5 Global Constraints, "Deterministic code generation").
# Takes PROGRAM, EMULATOR (empty on a native build) and MODULES (a ;-list).
foreach(run IN ITEMS first second)
    execute_process(COMMAND ${EMULATOR} "${PROGRAM}" ${MODULES}
                    RESULT_VARIABLE status OUTPUT_VARIABLE output_${run} ERROR_VARIABLE errors)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "the ${run} run failed (${status}):\n${errors}")
    endif()
endforeach()
if(output_first STREQUAL "")
    message(FATAL_ERROR "the runs printed no digest")
endif()
if(NOT output_first STREQUAL output_second)
    message(FATAL_ERROR "two processes compiled different code:\n${output_first}\n---\n${output_second}")
endif()
message(STATUS "two processes, the same code:\n${output_first}")
