# SPDX-License-Identifier: Apache-2.0
#
# Applies Contour's WAMR patches (patches.lock) to the WAMR source tree in the working directory, in the
# lock's order, each once: the PATCH_COMMAND of the wamr-contour source (CMakeLists.txt). A patch whose
# SHA-256 is not the one the lock pins is refused before anything is applied, so the runner never builds a
# WAMR other than Contour's. A patch already applied (a re-run over the same tree) is left as it is.
#
#   cmake -D GIT_EXECUTABLE=<git> -D PATCHES_LOCK=<patches.lock> -P ApplyPatches.cmake
cmake_minimum_required(VERSION 3.25)
foreach(required IN ITEMS GIT_EXECUTABLE PATCHES_LOCK)
    if(NOT ${required})
        message(FATAL_ERROR "ApplyPatches.cmake needs -D ${required}=...")
    endif()
endforeach()

get_filename_component(patchDirectory "${PATCHES_LOCK}" DIRECTORY)
set(patchDirectory "${patchDirectory}/patches")
file(STRINGS "${PATCHES_LOCK}" rows REGEX "^[a-z]")
set(patches "")
foreach(row IN LISTS rows)
    if(row MATCHES "^patches = (.+)$")
        string(REPLACE " " ";" patches "${CMAKE_MATCH_1}")
    elseif(row MATCHES "^sha256\\.([^ ]+) = ([0-9a-f]+)$")
        set("pinned_${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}")
    endif()
endforeach()
if(NOT patches)
    message(FATAL_ERROR "${PATCHES_LOCK} names no patches")
endif()

# Every patch is checked before the first is applied.
foreach(patch IN LISTS patches)
    if(NOT DEFINED "pinned_${patch}")
        message(FATAL_ERROR "${PATCHES_LOCK} pins no SHA-256 for ${patch}")
    endif()
    file(SHA256 "${patchDirectory}/${patch}" actual)
    if(NOT actual STREQUAL "${pinned_${patch}}")
        message(FATAL_ERROR "${patch} is not the patch ${PATCHES_LOCK} pins: its SHA-256 is ${actual}, not "
                            "${pinned_${patch}}")
    endif()
endforeach()

# Through `git apply`, which asks nothing; GIT_CEILING_DIRECTORIES keeps git from taking an enclosing
# repository's paths (the build tree's) for the patch's.
get_filename_component(tree "." ABSOLUTE)
get_filename_component(ceiling "${tree}" DIRECTORY)
function(gitApply result patch)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${ceiling}"
                            "${GIT_EXECUTABLE}" apply ${ARGN} "${patch}"
                    RESULT_VARIABLE status OUTPUT_QUIET ERROR_QUIET)
    set(${result} ${status} PARENT_SCOPE)
endfunction()
foreach(patch IN LISTS patches)
    set(file "${patchDirectory}/${patch}")
    gitApply(applied "${file}" --check --reverse)
    if(applied EQUAL 0)
        message(STATUS "Already applied: ${patch}")
        continue()
    endif()
    gitApply(patched "${file}")
    if(NOT patched EQUAL 0)
        message(FATAL_ERROR "Could not apply ${patch} to ${tree}")
    endif()
    message(STATUS "Applied: ${patch}")
endforeach()
