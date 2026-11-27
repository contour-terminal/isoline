# SPDX-License-Identifier: Apache-2.0
#
# Runs one case of the contract-override fixture (tests/CMakeLists.txt), in script mode: configures the
# fixture with FIXTURE_OVERRIDE, and where that passes, builds isoline_contract_flags_check, the
# build-time check of the compile commands, alone (it compiles nothing). Both outputs are printed, for the
# test's regular expressions; "contract-override: configured and checked" follows a check that passed.
# The edited-response-file case writes its response file clean before the configure, and after a check
# that passed adds a refused flag to it and runs the check again, which must see the edit.

cmake_minimum_required(VERSION 3.25)

set(responseFile "${FIXTURE_BINARY_DIR}/flags.rsp")
if(FIXTURE_OVERRIDE STREQUAL "edited-response-file")
    file(WRITE "${responseFile}" "-O0\n")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${FIXTURE_SOURCE_DIR}" -B "${FIXTURE_BINARY_DIR}" -G "${FIXTURE_GENERATOR}"
            "-DCMAKE_CXX_COMPILER=${FIXTURE_CXX_COMPILER}" -DCMAKE_BUILD_TYPE=Debug
            "-DISOLINE_SOURCE_DIR=${ISOLINE_SOURCE_DIR}" "-DISOLINE_FIXTURE_OVERRIDE=${FIXTURE_OVERRIDE}"
            -DISOLINE_REFLECTION=ON -DISOLINE_ANNOTATIONS=OFF -DISOLINE_CONTRACTS=ON
    OUTPUT_VARIABLE output ERROR_VARIABLE output RESULT_VARIABLE configured)
message("${output}")
if(NOT configured EQUAL 0)
    return()
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${FIXTURE_BINARY_DIR}" --target isoline_contract_flags_check
    OUTPUT_VARIABLE output ERROR_VARIABLE output RESULT_VARIABLE checked)
message("${output}")
if(NOT checked EQUAL 0)
    return()
endif()
message("contract-override: configured and checked")
if(FIXTURE_OVERRIDE STREQUAL "edited-response-file")
    file(WRITE "${responseFile}" "-O0 -fcontract-evaluation-semantic=ignore\n")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${FIXTURE_BINARY_DIR}" --target isoline_contract_flags_check
        OUTPUT_VARIABLE output ERROR_VARIABLE output)
    message("${output}")
endif()
