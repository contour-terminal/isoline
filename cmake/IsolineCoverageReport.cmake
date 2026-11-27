# SPDX-License-Identifier: Apache-2.0
#
# The coverage report and its ratchet, run in script mode by the target isoline_coverage_check
# (cmake/IsolineCoverage.cmake), which passes every variable below:
#     cmake -DPROFDATA=<llvm-profdata> -DCOV=<llvm-cov> -DPYTHON=<python3> -DOBJECTS_FILE=<one binary per line>
#           -DIGNORE_REGEX=<regex> -DPROFILE_DIR=<dir of *.profraw> -DOUTPUT_DIR=<dir> -DSOURCE_DIR=<Isoline>
#           -P IsolineCoverageReport.cmake
# llvm-cov warns "N functions have mismatched data", which is benign: a binary that includes a header's
# inline or constexpr function without emitting it (it calls none, or only in constant evaluation, as the
# spec runner does FeatureSet::all()) carries clang's placeholder record for it, of hash 0 and all counts
# zero, while the profile holds the record of the binaries that do emit it. llvm-cov drops the placeholder
# and keeps the real counts.

cmake_minimum_required(VERSION 3.25)

foreach(variable IN ITEMS PROFDATA COV PYTHON OBJECTS_FILE IGNORE_REGEX PROFILE_DIR OUTPUT_DIR SOURCE_DIR)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "IsolineCoverageReport.cmake: ${variable} is not given")
    endif()
endforeach()

file(GLOB profiles LIST_DIRECTORIES false "${PROFILE_DIR}/*.profraw")
if(NOT profiles)
    message(FATAL_ERROR "[isoline] coverage: ${PROFILE_DIR} holds no profile: run the tests first "
                        "(ctest --preset clang-coverage)")
endif()
file(STRINGS "${OBJECTS_FILE}" binaries)
set(objects "")
foreach(binary IN LISTS binaries)
    if(objects)
        list(APPEND objects -object)
    endif()
    list(APPEND objects "${binary}")
endforeach()

## @brief Runs @p ARGN, a command, and fails the report where it fails.
function(run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        list(JOIN ARGN " " command)
        message(FATAL_ERROR "[isoline] coverage: ${command} failed (${result})")
    endif()
endfunction()

set(profile "${OUTPUT_DIR}/isoline.profdata")
run("${PROFDATA}" merge -sparse ${profiles} -o "${profile}")
set(common ${objects} "-instr-profile=${profile}" "-ignore-filename-regex=${IGNORE_REGEX}")
execute_process(COMMAND "${COV}" report ${common} OUTPUT_FILE "${OUTPUT_DIR}/coverage-summary.txt"
                RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "[isoline] coverage: llvm-cov report failed (${result})")
endif()
file(READ "${OUTPUT_DIR}/coverage-summary.txt" summary)
message("${summary}")
foreach(format IN ITEMS text lcov)
    set(output "${OUTPUT_DIR}/coverage.json")
    if(format STREQUAL "lcov")
        set(output "${OUTPUT_DIR}/coverage.lcov")
    endif()
    execute_process(COMMAND "${COV}" export ${common} "-format=${format}" OUTPUT_FILE "${output}"
                    RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "[isoline] coverage: llvm-cov export -format=${format} failed (${result})")
    endif()
endforeach()
run("${COV}" show ${common} -format=html "-output-dir=${OUTPUT_DIR}/coverage-html")
run("${PYTHON}" -I "${SOURCE_DIR}/scripts/coverage-check.py" "${OUTPUT_DIR}/coverage.json"
    "${SOURCE_DIR}/coverage-baseline.txt")
