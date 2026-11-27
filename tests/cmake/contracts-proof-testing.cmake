# SPDX-License-Identifier: Apache-2.0
#
# language-features-contracts-proof-reads-testing: configures tests/cmake/contracts-proof-testing with
# contracts native, then builds its proof, whose output the test's PASS_REGULAR_EXPRESSION reads.
#
#   cmake -DSOURCE_DIR=<isoline> -DBINARY_DIR=<scratch> -DGENERATOR=<g> -DCXX_COMPILER=<c++> -P contracts-proof-testing.cmake
cmake_minimum_required(VERSION 3.25)
foreach(required IN ITEMS SOURCE_DIR BINARY_DIR GENERATOR CXX_COMPILER)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "contracts-proof-testing: -D${required}=... is required")
    endif()
endforeach()
file(REMOVE_RECURSE "${BINARY_DIR}")
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${SOURCE_DIR}/tests/cmake/contracts-proof-testing" -B "${BINARY_DIR}"
                        -G "${GENERATOR}" "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}" "-DISOLINE_SOURCE_DIR=${SOURCE_DIR}"
                        -DISOLINE_REFLECTION=ON -DISOLINE_ANNOTATIONS=ON -DISOLINE_CONTRACTS=ON
                RESULT_VARIABLE configured OUTPUT_VARIABLE output ERROR_VARIABLE output)
if(NOT configured EQUAL 0)
    message(FATAL_ERROR "contracts-proof-testing: the fixture did not configure:\n${output}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BINARY_DIR}" --target fixture_proof
                RESULT_VARIABLE built OUTPUT_VARIABLE output ERROR_VARIABLE output)
message("${output}")
if(built EQUAL 0)
    message(FATAL_ERROR "contracts-proof-testing: the proof built, so it did not read the testing/ header")
endif()
