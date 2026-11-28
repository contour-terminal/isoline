# SPDX-License-Identifier: Apache-2.0
#
# The options of the wamr-classic runner, derived from Contour's (ContourOptions.cmake): the same rows with
# WAMR_BUILD_FAST_INTERP turned off. ClassicOptions_test.cmake checks both outcomes.

## @brief Sets @p outVar to the option rows that follow it, with "WAMR_BUILD_FAST_INTERP 1" turned into
## "WAMR_BUILD_FAST_INTERP 0"; a fatal error where no row says "WAMR_BUILD_FAST_INTERP 1", since a Contour
## row renamed or dropped would otherwise leave wamr-classic a second fast interpreter under the classic
## one's name.
##
## isoline_wamr_classic_options(<outVar> <row>...)
function(isoline_wamr_classic_options outVar)
    set(rows ${ARGN})
    list(FIND rows "WAMR_BUILD_FAST_INTERP 1" fastRow)
    if(fastRow EQUAL -1)
        message(FATAL_ERROR "wamr-classic: Contour's WAMR options have no \"WAMR_BUILD_FAST_INTERP 1\" row to "
                            "turn off")
    endif()
    list(TRANSFORM rows REPLACE "^WAMR_BUILD_FAST_INTERP 1$" "WAMR_BUILD_FAST_INTERP 0")
    set(${outVar} ${rows} PARENT_SCOPE)
endfunction()
