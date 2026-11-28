# SPDX-License-Identifier: Apache-2.0
#
# Script mode (cmake -P): the wamr-classic options of Contour's committed rows turn the fast interpreter
# off; with CASE=missing, rows without the fast interpreter's row are refused (the test expects the error).
include("${CMAKE_CURRENT_LIST_DIR}/ClassicOptions.cmake")
if(CASE STREQUAL "missing")
    isoline_wamr_classic_options(classic "WAMR_BUILD_INTERP 1" "WAMR_BUILD_FAST_INTERPRETER 1")
    message(FATAL_ERROR "wamr-classic-options: rows without WAMR_BUILD_FAST_INTERP 1 were accepted")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/ContourOptions.cmake")
if(NOT "WAMR_BUILD_FAST_INTERP 0" IN_LIST ISOLINE_WAMR_CLASSIC_OPTIONS
   OR "WAMR_BUILD_FAST_INTERP 1" IN_LIST ISOLINE_WAMR_CLASSIC_OPTIONS)
    message(FATAL_ERROR "wamr-classic-options: the classic rows still run the fast interpreter: "
                        "${ISOLINE_WAMR_CLASSIC_OPTIONS}")
endif()
list(LENGTH ISOLINE_WAMR_CONTOUR_OPTIONS contourRows)
list(LENGTH ISOLINE_WAMR_CLASSIC_OPTIONS classicRows)
if(NOT contourRows EQUAL classicRows)
    message(FATAL_ERROR "wamr-classic-options: ${classicRows} rows, not Contour's ${contourRows}")
endif()
message(STATUS "wamr-classic-options: ok")
