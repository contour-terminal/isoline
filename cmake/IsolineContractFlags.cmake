# SPDX-License-Identifier: Apache-2.0
#
# The build-time check of the flags Isoline's contracts compile with (cmake/IsolineLanguageFeatures.cmake,
# _isoline_add_contract_flags_check()), run in script mode:
#     cmake -DCOMPILE_COMMANDS=<compile_commands.json> -DSOURCE_DIR=<Isoline's source tree>
#           -DREFUSED_FLAGS=<file of flag|reason lines> [-DCXX_COMPILER=<compiler>]
#           [-DSTAMP=<file> [-DDEPFILE=<file>]] -P IsolineContractFlags.cmake
# It reads the compile commands as the generator wrote them, so flags built by generator expressions and
# add_definitions() are seen as the compiler sees them, and refuses every translation unit under
# <SOURCE_DIR>/src/ compiled with a refused flag, response files (`@file`) and GCC specs files
# (`-specs=file`, `--specs=file`, and the files a specs file names by `%include`) read as the compiler
# reads them: a specs file can add any flag to the compiler proper's command line, the semantic included.
# A bare specs name is resolved as the driver resolves it, through `CXX_COMPILER -print-file-name=`, and
# then against the entry's directory. A response or specs file it cannot read is an error (a
# `%include_noerr` the driver may skip excepted). On success it touches STAMP, and writes DEPFILE, which
# names every file it read, so that editing one reruns the check. What the compiler is given past the
# command line and those files (a compiler launcher, a rule override of CMAKE_CXX_COMPILE_OBJECT, a
# `-wrapper` program, a `-fplugin=` plugin) is the embedder's own toolchain, which no build can audit.
#
# Linear in the size of the compile commands: string(JSON) re-parses the whole document on every call, so
# the document is cut into its entries first (braces inside strings never cut it), and each entry is
# parsed alone.

cmake_minimum_required(VERSION 3.25)

foreach(variable IN ITEMS COMPILE_COMMANDS SOURCE_DIR REFUSED_FLAGS)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "IsolineContractFlags.cmake: ${variable} is not given")
    endif()
endforeach()

file(STRINGS "${REFUSED_FLAGS}" refusedRows)
file(READ "${COMPILE_COMMANDS}" commands)
set(findings "")
set(responseFiles "")

# The placeholders of the characters that are structure to the splitting or to CMake lists: the control
# characters 1 to 7, which valid JSON never holds unescaped (a document that does is refused).
string(ASCII 1 backslashes)
string(ASCII 2 quote)
string(ASCII 3 semicolon)
string(ASCII 4 open)
string(ASCII 5 close)
string(ASCII 6 leftBrace)
string(ASCII 7 rightBrace)

## @brief Sets @p outVar to ON where @p text may compile with a refused flag or read a response or specs
## file: it holds a `\uXXXX` escape (which may name any character of a flag, or an `@`), or its text with
## every quote and backslash removed, held in a placeholder or not (which the shell would strip from a
## flag, and which only ever joins more text together) holds a refused flag, an `@`, or `specs=`. A
## pre-filter, never a verdict.
function(mayBeRefused text outVar)
    string(FIND "${text}" "\\u" unicodeEscapeAt)
    if(NOT unicodeEscapeAt EQUAL -1)
        set(${outVar} ON PARENT_SCOPE)
        return()
    endif()
    set(bare "${text}")
    foreach(character IN ITEMS "\"" "'" "\\" "${quote}" "${backslashes}")
        string(REPLACE "${character}" "" bare "${bare}")
    endforeach()
    refusedRowIn("${bare}" row)
    string(FIND "${bare}" "@" responseFileAt)
    string(FIND "${bare}" "specs=" specsFileAt)
    set(may OFF)
    if(NOT row STREQUAL "" OR NOT responseFileAt EQUAL -1 OR NOT specsFileAt EQUAL -1)
        set(may ON)
    endif()
    set(${outVar} ${may} PARENT_SCOPE)
endfunction()

## @brief Sets @p outVar to the refused row (`flag|reason`) whose flag @p text holds, or to "".
function(refusedRowIn text outVar)
    foreach(row IN LISTS refusedRows)
        string(REGEX MATCH "^([^|]*)[|]" matched "${row}")
        string(FIND "${text}" "${CMAKE_MATCH_1}" position)
        if(NOT position EQUAL -1)
            set(${outVar} "${row}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${outVar} "" PARENT_SCOPE)
endfunction()

## @brief Sets @p outVar to the file a specs file name @p name denotes, as the GCC driver finds it: a name
## with a directory is relative to @p directory; a bare one is looked up first where the driver looks
## (`CXX_COMPILER -print-file-name=<name>`, which echoes the name when it finds nothing), then in
## @p directory. Each bare name is asked of the compiler once.
function(specsFileOf name directory outVar)
    set(path "${name}")
    if(NOT name MATCHES "[/\\]" AND DEFINED CXX_COMPILER AND NOT CXX_COMPILER STREQUAL "")
        get_property(known GLOBAL PROPERTY "isolineSpecs:${name}" SET)
        if(NOT known)
            execute_process(COMMAND ${CXX_COMPILER} "-print-file-name=${name}"
                            OUTPUT_VARIABLE found OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE result)
            if(NOT result EQUAL 0)
                set(found "${name}")
            endif()
            set_property(GLOBAL PROPERTY "isolineSpecs:${name}" "${found}")
        endif()
        get_property(found GLOBAL PROPERTY "isolineSpecs:${name}")
        if(IS_ABSOLUTE "${found}" AND EXISTS "${found}" AND NOT IS_DIRECTORY "${found}")
            set(path "${found}")
        endif()
    endif()
    cmake_path(ABSOLUTE_PATH path BASE_DIRECTORY "${directory}" NORMALIZE)
    set(${outVar} "${path}" PARENT_SCOPE)
endfunction()

## @brief Reads the specs files @p text names (`-specs=`, `--specs=`) for the entry @p relative in
## @p directory, and every file those name by `%include` or `%include_noerr`, each once: sets `specsText`
## in the caller to their joined text, `specsOrigin` to the first one holding a refused flag (or ""), and
## appends each to `read`. A file that cannot be read is a FATAL_ERROR, except one `%include_noerr` names.
## A function, not a macro: @p text is a whole command line, which a macro would splice into its code.
function(readSpecsFiles text relative directory)
    set(specsSeen "")
    set(specsText "")
    set(specsOrigin "")
    string(REGEX MATCHALL "(^|[ \t\n])--?specs=[^ \t\n]+" pendingSpecs "${text}")
    list(TRANSFORM pendingSpecs REPLACE "^[ \t\n]*--?specs=" "required|")
    while(pendingSpecs)
        list(POP_FRONT pendingSpecs pendingSpec)
        string(REGEX MATCH "^([a-z]+)[|](.*)$" matched "${pendingSpec}")
        set(specsKind "${CMAKE_MATCH_1}")
        string(REPLACE "${semicolon}" ";" specsName "${CMAKE_MATCH_2}")
        specsFileOf("${specsName}" "${directory}" specsFile)
        if(specsFile IN_LIST specsSeen)
            continue()
        endif()
        list(APPEND specsSeen "${specsFile}")
        if(NOT EXISTS "${specsFile}" OR IS_DIRECTORY "${specsFile}")
            if(specsKind STREQUAL "optional")
                continue()
            endif()
            message(FATAL_ERROR
                "[isoline] ${relative} is compiled with the specs file ${specsFile}, which cannot be read, so the "
                "flags Isoline's contracts compile with cannot be checked")
        endif()
        file(READ "${specsFile}" specs)
        if(NOT specsFile IN_LIST read)
            list(APPEND read "${specsFile}")
        endif()
        refusedRowIn("${specs}" row)
        if(NOT row STREQUAL "" AND specsOrigin STREQUAL "")
            set(specsOrigin " (from the specs file ${specsFile})")
        endif()
        string(APPEND specsText " ${specs}")
        string(REGEX MATCHALL "%include(_noerr)?[ \t]*<[^>\n]*>" included "${specs}")
        foreach(include IN LISTS included)
            string(REGEX MATCH "^%include(_noerr)?[ \t]*<([^>]*)>$" matched "${include}")
            if(CMAKE_MATCH_1 STREQUAL "")
                list(APPEND pendingSpecs "required|${CMAKE_MATCH_2}")
            else()
                list(APPEND pendingSpecs "optional|${CMAKE_MATCH_2}")
            endif()
        endforeach()
    endwhile()
    set(specsText "${specsText}" PARENT_SCOPE)
    set(specsOrigin "${specsOrigin}" PARENT_SCOPE)
    set(read "${read}" PARENT_SCOPE)
endfunction()

## @brief Checks one compile command, @p entry, a JSON object: appends a line to `findings` in the caller
## where it compiles a translation unit under <SOURCE_DIR>/src/ with a refused flag, and every response
## or specs file it reads to `responseFiles`. A response or specs file that cannot be read is a FATAL_ERROR.
function(checkEntry entry)
    string(JSON directory GET "${entry}" directory)
    string(JSON file GET "${entry}" file)
    cmake_path(ABSOLUTE_PATH file BASE_DIRECTORY "${directory}" NORMALIZE)
    cmake_path(IS_PREFIX SOURCE_DIR "${file}" NORMALIZE underIsoline)
    if(NOT underIsoline)
        return()
    endif()
    file(RELATIVE_PATH relative "${SOURCE_DIR}" "${file}")
    if(NOT relative MATCHES "^src/")
        return()
    endif()
    string(JSON command ERROR_VARIABLE noCommand GET "${entry}" command)
    if(noCommand)
        # The `arguments` form: one string per argument. Getting each by its index would re-parse the
        # entry every time; the array is tokenized as the document is, and each string decoded alone.
        string(JSON arguments GET "${entry}" arguments)
        string(REPLACE "\\\\" "${backslashes}" arguments "${arguments}")
        string(REPLACE "\\\"" "${quote}" arguments "${arguments}")
        string(REPLACE ";" "${semicolon}" arguments "${arguments}")
        string(REPLACE "[" "${open}" arguments "${arguments}")
        string(REPLACE "]" "${close}" arguments "${arguments}")
        string(REGEX MATCHALL "\"[^\"]*\"" encoded "${arguments}")
        # A string without escapes is its own text between the quotes: unquoted natively. Only one
        # with escapes is decoded, alone; the order of the arguments does not matter to what is refused.
        set(escaped "${encoded}")
        list(FILTER escaped INCLUDE REGEX "[${backslashes}${quote}]")
        set(command "${encoded}")
        list(FILTER command EXCLUDE REGEX "[${backslashes}${quote}]")
        list(TRANSFORM command REPLACE "^\"(.*)\"$" "\\1")
        list(TRANSFORM command REPLACE "${open}" "[")
        list(TRANSFORM command REPLACE "${close}" "]")
        foreach(argument IN LISTS escaped)
            string(REPLACE "${open}" "[" argument "${argument}")
            string(REPLACE "${close}" "]" argument "${argument}")
            string(REPLACE "${semicolon}" ";" argument "${argument}")
            string(REPLACE "${quote}" "\\\"" argument "${argument}")
            string(REPLACE "${backslashes}" "\\\\" argument "${argument}")
            string(JSON argument GET "[${argument}]" 0)
            # A `;` in an argument would split the list: held in its placeholder, which no flag holds.
            string(REPLACE ";" "${semicolon}" argument "${argument}")
            list(APPEND command "${argument}")
        endforeach()
    else()
        # A `;` would split the list separate_arguments() returns: held in its placeholder.
        string(REPLACE ";" "${semicolon}" command "${command}")
        separate_arguments(command UNIX_COMMAND "${command}")
    endif()
    # What the compiler is given: the arguments, then each response file's text. Joined and filtered
    # natively, never argument by argument, so a megabyte command line costs one search.
    list(JOIN command " " text)
    set(responseArguments "${command}")
    list(FILTER responseArguments INCLUDE REGEX "^@.")
    set(read "${responseFiles}")
    set(origin "")
    foreach(argument IN LISTS responseArguments)
        string(REPLACE "${semicolon}" ";" argument "${argument}")
        string(SUBSTRING "${argument}" 1 -1 responseFile)
        cmake_path(ABSOLUTE_PATH responseFile BASE_DIRECTORY "${directory}" NORMALIZE)
        if(NOT EXISTS "${responseFile}" OR IS_DIRECTORY "${responseFile}")
            message(FATAL_ERROR
                "[isoline] ${relative} is compiled with the response file ${responseFile}, which cannot be "
                "read, so the flags Isoline's contracts compile with cannot be checked")
        endif()
        file(READ "${responseFile}" response)
        if(NOT responseFile IN_LIST read)
            list(APPEND read "${responseFile}")
        endif()
        refusedRowIn("${response}" row)
        if(NOT row STREQUAL "" AND origin STREQUAL "")
            set(origin " (from the response file ${responseFile})")
        endif()
        string(APPEND text " ${response}")
    endforeach()
    # The specs files the arguments and the response files name, read as the driver reads them.
    readSpecsFiles("${text}" "${relative}" "${directory}")
    refusedRowIn("${text}" rowBeforeSpecs)
    if(rowBeforeSpecs STREQUAL "")
        set(origin "${specsOrigin}")
    endif()
    string(APPEND text "${specsText}")
    set(responseFiles "${read}" PARENT_SCOPE)
    refusedRowIn("${text}" row)
    if(NOT row STREQUAL "")
        string(REGEX MATCH "^([^|]*)[|](.*)$" matched "${row}")
        set(findings
            "${findings}\n  ${relative} is compiled with ${CMAKE_MATCH_1}${origin}, which is refused, since it ${CMAKE_MATCH_2}."
            PARENT_SCOPE)
    endif()
endfunction()

# Most builds hold no refused flag and no response file at all: decided on the text alone.
mayBeRefused("${commands}" anyMay)
if(anyMay)
    # The document is cut into its entries without a token-by-token loop, so the check is linear in its
    # size. First `\\` and `\"` are held in placeholders, so that every `"` left delimits a string. Split
    # at every `"`, the document alternates between text outside strings and inside them; the braces
    # inside are held in placeholders too (natively, every second element), and `;`, `[` and `]`, which
    # CMake lists treat as structure, everywhere. What remains is cut at each `},{` between the objects of
    # the top-level array, and each object is put back together and parsed alone. A compile command is a
    # flat object, so an entry that is not exactly one object is refused, as is anything else that is not
    # a JSON array of objects. The placeholders are the control characters 1 to 7, which valid JSON never
    # holds unescaped; a document that does is refused.
    foreach(code RANGE 1 7)
        string(ASCII ${code} placeholder)
        string(FIND "${commands}" "${placeholder}" rawControl)
        if(NOT rawControl EQUAL -1)
            message(FATAL_ERROR
                "[isoline] ${COMPILE_COMMANDS} is not valid JSON: it holds the control character ${code} unescaped")
        endif()
    endforeach()
    set(malformed "[isoline] ${COMPILE_COMMANDS} is not a well-formed JSON array of compile commands")
    string(REPLACE "\\\\" "${backslashes}" text "${commands}")
    string(REPLACE "\\\"" "${quote}" text "${text}")
    string(REPLACE ";" "${semicolon}" text "${text}")
    string(REPLACE "[" "${open}" text "${text}")
    string(REPLACE "]" "${close}" text "${text}")
    string(REPLACE "\"" ";" parts "${text}")
    list(LENGTH parts partCount)
    math(EXPR quoteCount "${partCount} - 1")
    math(EXPR unpaired "${quoteCount} % 2")
    if(unpaired)
        message(FATAL_ERROR "${malformed}: a string is not closed")
    endif()
    if(partCount GREATER 1)
        math(EXPR lastPart "${partCount} - 1")
        list(TRANSFORM parts REPLACE "{" "${leftBrace}" FOR 1 ${lastPart} 2)
        list(TRANSFORM parts REPLACE "}" "${rightBrace}" FOR 1 ${lastPart} 2)
    endif()
    list(JOIN parts "\"" text)
    string(STRIP "${text}" text)
    string(LENGTH "${text}" textLength)
    set(first "")
    set(last "")
    if(textLength GREATER 0)
        string(SUBSTRING "${text}" 0 1 first)
        math(EXPR lastAt "${textLength} - 1")
        string(SUBSTRING "${text}" ${lastAt} 1 last)
    endif()
    if(NOT first STREQUAL open OR NOT last STREQUAL close)
        message(FATAL_ERROR "${malformed}: it is not an array")
    endif()
    string(REGEX REPLACE "^${open}[ \t\r\n]*" "" text "${text}")
    string(REGEX REPLACE "[ \t\r\n]*${close}$" "" text "${text}")
    string(REGEX REPLACE "}[ \t\r\n]*,[ \t\r\n]*{" "};{" entries "${text}")
    foreach(entry IN LISTS entries)
        if(NOT entry MATCHES "^{[^{}]*}$")
            message(FATAL_ERROR "${malformed}: an entry is not one flat object")
        endif()
        # An entry with neither a refused flag nor a response file cannot be refused: not parsed.
        mayBeRefused("${entry}" entryMay)
        if(entryMay)
            string(REPLACE "${leftBrace}" "{" entry "${entry}")
            string(REPLACE "${rightBrace}" "}" entry "${entry}")
            string(REPLACE "${semicolon}" ";" entry "${entry}")
            string(REPLACE "${open}" "[" entry "${entry}")
            string(REPLACE "${close}" "]" entry "${entry}")
            string(REPLACE "${quote}" "\\\"" entry "${entry}")
            string(REPLACE "${backslashes}" "\\\\" entry "${entry}")
            checkEntry("${entry}")
        endif()
    endforeach()
endif()

if(NOT findings STREQUAL "")
    message(FATAL_ERROR
        "[isoline] Isoline's contracts are checked under the enforce semantic only (docs/language-features.md):"
        "${findings}")
endif()
if(DEFINED STAMP AND NOT STAMP STREQUAL "")
    if(DEFINED DEPFILE AND NOT DEPFILE STREQUAL "")
        set(dependencies "")
        foreach(responseFile IN LISTS responseFiles)
            string(REPLACE " " "\\ " responseFile "${responseFile}")
            string(APPEND dependencies " ${responseFile}")
        endforeach()
        string(REPLACE " " "\\ " target "${STAMP}")
        file(WRITE "${DEPFILE}" "${target}:${dependencies}\n")
    endif()
    file(TOUCH "${STAMP}")
endif()
