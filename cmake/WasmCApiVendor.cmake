# SPDX-License-Identifier: Apache-2.0
#
# The vendored copy of the WebAssembly C API (wasm-c-api) in vendor/wasm-c-api/: it fetches the copy
# from upstream at one commit, and it verifies that the copy is still the bytes it fetched.
#
#   cmake -DMODE=sync -DCOMMIT=<40-character SHA> -DDEST=vendor/wasm-c-api -P cmake/WasmCApiVendor.cmake
#   cmake -DMODE=check -DDEST=vendor/wasm-c-api -P cmake/WasmCApiVendor.cmake
#   cmake -DMODE=selftest -DDEST=vendor/wasm-c-api -DSCRATCH=<empty directory> -P cmake/WasmCApiVendor.cmake
#
# It mirrors the vendored core-cpp's own tool (vendor/core-cpp/cmake/CoreCppVendor.cmake), whose check
# rules it applies to a copy with no tool of its own: wasm-c-api is not a CMake project, so the tool
# lives here and the copy holds the upstream files and the MANIFEST alone.
#
# sync downloads each file of WASM_C_API_VENDOR_FILES from the repository's raw view at COMMIT -- a
# commit, never a branch, which would change under the copy -- into a staging directory, and replaces
# DEST with it only once every download has succeeded, writing the MANIFEST: the repository, the
# commit, the file count and one `<sha256>  <path>` line per file.
#
# check needs no network: it re-hashes every file the MANIFEST lists and refuses a hash mismatch, a
# missing file, a file the MANIFEST does not list, a header line that is missing, and a file count that
# disagrees with the list, so a local edit fails the `vendor-wasm-c-api-manifest` test. Every refusal
# that applies is reported, not only the first.
#
# check also compares the files the MANIFEST lists with WASM_C_API_VENDOR_FILES, the file set, so a file
# dropped from both the copy and its MANIFEST is refused too.
#
# selftest copies DEST into SCRATCH once per way a copy can go wrong, breaks the copy that way, and
# requires check to refuse it (the `vendor-wasm-c-api-manifest-selftest` test).

cmake_minimum_required(VERSION 3.25)

set(WASM_C_API_VENDOR_REPOSITORY "https://github.com/WebAssembly/wasm-c-api")
set(WASM_C_API_VENDOR_RAW "https://raw.githubusercontent.com/WebAssembly/wasm-c-api")

# The files of the copy, by their path in the upstream repository, which is their path in the copy.
set(WASM_C_API_VENDOR_FILES
    LICENSE
    include/wasm.h)

set(WASM_C_API_VENDOR_MANIFEST "MANIFEST")

## @brief Sets @p outVar to the sorted files under @p dir, relative to it, without the MANIFEST.
function(wasm_c_api_vendor_list_files dir outVar)
    get_filename_component(dir "${dir}" ABSOLUTE)
    file(GLOB_RECURSE found LIST_DIRECTORIES false RELATIVE "${dir}" "${dir}/*")
    list(REMOVE_ITEM found "${WASM_C_API_VENDOR_MANIFEST}")
    list(SORT found)
    set(${outVar} "${found}" PARENT_SCOPE)
endfunction()

## @brief Sets @p outVar to the refusals of the copy in @p dir, empty when it matches its MANIFEST.
function(wasm_c_api_vendor_refusals dir outVar)
    set(manifestPath "${dir}/${WASM_C_API_VENDOR_MANIFEST}")
    if(NOT EXISTS "${manifestPath}")
        set(${outVar} "\n  ${WASM_C_API_VENDOR_MANIFEST}: does not exist" PARENT_SCOPE)
        return()
    endif()
    file(STRINGS "${manifestPath}" manifestLines)
    set(refusals "")
    set(listed "")
    set(statedRepository "")
    set(statedCommit "")
    set(statedCount "")
    foreach(line IN LISTS manifestLines)
        if(line MATCHES "^# repository (.+)$")
            set(statedRepository "${CMAKE_MATCH_1}")
        elseif(line MATCHES "^# commit (.+)$")
            set(statedCommit "${CMAKE_MATCH_1}")
        elseif(line MATCHES "^# files (.+)$")
            set(statedCount "${CMAKE_MATCH_1}")
        elseif(line MATCHES "^#")
            continue()
        elseif(line MATCHES "^([0-9a-f]+)  (.+)$")
            set(wanted "${CMAKE_MATCH_1}")
            set(path "${CMAKE_MATCH_2}")
            list(APPEND listed "${path}")
            if(NOT EXISTS "${dir}/${path}")
                string(APPEND refusals "\n  ${path}: is missing")
                continue()
            endif()
            file(SHA256 "${dir}/${path}" got)
            if(NOT got STREQUAL wanted)
                string(APPEND refusals "\n  ${path}: differs from the manifest (expected ${wanted}, found ${got})")
            endif()
        elseif(NOT line STREQUAL "")
            string(APPEND refusals "\n  ${WASM_C_API_VENDOR_MANIFEST}: line '${line}' is neither a header nor '<sha256>  <path>'")
        endif()
    endforeach()

    wasm_c_api_vendor_list_files("${dir}" present)
    foreach(path IN LISTS present)
        if(NOT path IN_LIST listed)
            string(APPEND refusals "\n  ${path}: is not in the manifest")
        endif()
    endforeach()

    # The manifest lists the file set, exactly: a file dropped from the copy and from its manifest
    # together leaves nothing else to disagree about.
    foreach(path IN LISTS WASM_C_API_VENDOR_FILES)
        if(NOT path IN_LIST listed)
            string(APPEND refusals "\n  ${path}: is in the file set but not in the manifest")
        endif()
    endforeach()
    foreach(path IN LISTS listed)
        if(NOT path IN_LIST WASM_C_API_VENDOR_FILES)
            string(APPEND refusals "\n  ${path}: is in the manifest but not in the file set")
        endif()
    endforeach()

    # A manifest that says nothing would list no file to disagree about, and pass over an empty copy.
    if(statedRepository STREQUAL "")
        string(APPEND refusals "\n  ${WASM_C_API_VENDOR_MANIFEST}: has no '# repository <url>' header line")
    endif()
    string(LENGTH "${statedCommit}" commitLength)
    if(NOT commitLength EQUAL 40 OR NOT statedCommit MATCHES "^[0-9a-f]+$")
        string(APPEND refusals "\n  ${WASM_C_API_VENDOR_MANIFEST}: has no '# commit <40-character SHA>' header line (it says '${statedCommit}')")
    endif()
    list(LENGTH listed listedCount)
    if(NOT statedCount MATCHES "^[1-9][0-9]*$")
        string(APPEND refusals "\n  ${WASM_C_API_VENDOR_MANIFEST}: has no '# files <count>' header line with a nonzero count (it says '${statedCount}')")
    elseif(NOT statedCount EQUAL listedCount)
        string(APPEND refusals "\n  ${WASM_C_API_VENDOR_MANIFEST}: says '# files ${statedCount}' but lists ${listedCount}")
    endif()
    set(${outVar} "${refusals}" PARENT_SCOPE)
endfunction()

if(NOT DEFINED MODE OR NOT MODE MATCHES "^(sync|check|selftest)$")
    message(FATAL_ERROR
        "wasm-c-api-vendor: MODE must be sync, check or selftest, not '${MODE}'.\n"
        "  cmake -DMODE=sync -DCOMMIT=<SHA> -DDEST=<dir> -P cmake/WasmCApiVendor.cmake\n"
        "  cmake -DMODE=check -DDEST=<dir> -P cmake/WasmCApiVendor.cmake\n"
        "  cmake -DMODE=selftest -DDEST=<dir> -DSCRATCH=<dir> -P cmake/WasmCApiVendor.cmake")
endif()
if(NOT DEFINED DEST OR DEST STREQUAL "")
    message(FATAL_ERROR "wasm-c-api-vendor: DEST is not set; it names the directory the copy lives in.")
endif()
get_filename_component(DEST "${DEST}" ABSOLUTE)

# --- MODE=check ----------------------------------------------------------------
if(MODE STREQUAL "check")
    wasm_c_api_vendor_refusals("${DEST}" refusals)
    if(refusals)
        message(FATAL_ERROR
            "wasm-c-api-vendor: ${DEST} is not the verbatim copy its manifest describes:${refusals}\n"
            "A vendored copy carries no local change: fix it upstream and re-run MODE=sync with the new "
            "commit. To restore this copy, re-run MODE=sync with the manifest's commit.")
    endif()
    message(STATUS "wasm-c-api-vendor: ${DEST} matches its manifest")
    return()
endif()

# --- MODE=selftest -------------------------------------------------------------
#
# One row per way a copy can go wrong: its name, and what is done to a fresh copy to break it.
if(MODE STREQUAL "selftest")
    if(NOT DEFINED SCRATCH OR SCRATCH STREQUAL "")
        message(FATAL_ERROR "wasm-c-api-vendor: MODE=selftest needs SCRATCH, a directory it may empty.")
    endif()
    get_filename_component(SCRATCH "${SCRATCH}" ABSOLUTE)
    wasm_c_api_vendor_refusals("${DEST}" refusals)
    if(refusals)
        message(FATAL_ERROR "wasm-c-api-vendor: selftest needs an intact copy to break:${refusals}")
    endif()
    set(breakages
        "edited|file(APPEND \"\${copy}/include/wasm.h\" \"// a local edit\\n\")"
        "missing|file(REMOVE \"\${copy}/LICENSE\")"
        "unlisted|file(WRITE \"\${copy}/include/extra.h\" \"\")"
        "delisted|file(REMOVE \"\${copy}/LICENSE\")\nfile(STRINGS \"\${copy}/MANIFEST\" lines REGEX \"^# (repository|commit) |  include/wasm.h$\")\nlist(JOIN lines \"\\n\" text)\nfile(WRITE \"\${copy}/MANIFEST\" \"\${text}\\n# files 1\\n\")"
        "no-commit|file(STRINGS \"\${copy}/MANIFEST\" lines REGEX \"^[^#]|^# (repository|files) \")\nlist(JOIN lines \"\\n\" text)\nfile(WRITE \"\${copy}/MANIFEST\" \"\${text}\\n\")"
        "emptied|file(GLOB_RECURSE all \"\${copy}/*\")\nfile(REMOVE \${all})\nfile(WRITE \"\${copy}/MANIFEST\" \"\")")
    set(failures "")
    foreach(row IN LISTS breakages)
        string(REGEX MATCH "^([a-z-]+)[|](.*)$" matched "${row}")
        set(name "${CMAKE_MATCH_1}")
        set(breakage "${CMAKE_MATCH_2}")
        set(copy "${SCRATCH}/${name}")
        file(REMOVE_RECURSE "${copy}")
        file(COPY "${DEST}/" DESTINATION "${copy}")
        cmake_language(EVAL CODE "${breakage}")
        wasm_c_api_vendor_refusals("${copy}" refusals)
        if(refusals STREQUAL "")
            string(APPEND failures "\n  ${name}: the check accepted the broken copy")
        else()
            message(STATUS "wasm-c-api-vendor: selftest ${name}: refused as it should be:${refusals}")
        endif()
        file(REMOVE_RECURSE "${copy}")
    endforeach()
    if(failures)
        message(FATAL_ERROR "wasm-c-api-vendor: selftest failed:${failures}")
    endif()
    message(STATUS "wasm-c-api-vendor: selftest passed")
    return()
endif()

# --- MODE=sync -----------------------------------------------------------------
if(NOT DEFINED COMMIT OR NOT COMMIT MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "wasm-c-api-vendor: COMMIT must be a full commit SHA, not '${COMMIT}'.")
endif()
string(LENGTH "${COMMIT}" commitLength)
if(NOT commitLength EQUAL 40)
    message(FATAL_ERROR "wasm-c-api-vendor: COMMIT must be a full 40-character SHA, not '${COMMIT}'.")
endif()

set(staging "${DEST}.wasm-c-api-vendor-new")
file(REMOVE_RECURSE "${staging}")
string(CONCAT manifest
    "# wasm-c-api vendored copy -- verify it with:\n"
    "#   cmake -DMODE=check -DDEST=vendor/wasm-c-api -P cmake/WasmCApiVendor.cmake\n"
    "# repository ${WASM_C_API_VENDOR_REPOSITORY}\n"
    "# commit ${COMMIT}\n")
list(LENGTH WASM_C_API_VENDOR_FILES fileCount)
string(APPEND manifest "# files ${fileCount}\n")
list(SORT WASM_C_API_VENDOR_FILES)
foreach(path IN LISTS WASM_C_API_VENDOR_FILES)
    file(DOWNLOAD "${WASM_C_API_VENDOR_RAW}/${COMMIT}/${path}" "${staging}/${path}" TLS_VERIFY ON STATUS status)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
        file(REMOVE_RECURSE "${staging}")
        message(FATAL_ERROR "wasm-c-api-vendor: downloading ${path} at ${COMMIT} failed: ${status}")
    endif()
    file(SHA256 "${staging}/${path}" hash)
    string(APPEND manifest "${hash}  ${path}\n")
endforeach()
file(WRITE "${staging}/${WASM_C_API_VENDOR_MANIFEST}" "${manifest}")
wasm_c_api_vendor_refusals("${staging}" refusals)
if(refusals)
    file(REMOVE_RECURSE "${staging}")
    message(FATAL_ERROR "wasm-c-api-vendor: the fetched copy does not check:${refusals}")
endif()
file(REMOVE_RECURSE "${DEST}")
file(RENAME "${staging}" "${DEST}")
message(STATUS "wasm-c-api-vendor: ${DEST} is wasm-c-api at ${COMMIT}: ${fileCount} file(s)")
