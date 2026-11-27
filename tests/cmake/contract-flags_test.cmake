# SPDX-License-Identifier: Apache-2.0
#
# The self-test of cmake/IsolineContractFlags.cmake, the build-time check of the flags Isoline's contracts
# compile with, run in script mode:
#     cmake -DWORK_DIR=<scratch directory> -P tests/cmake/contract-flags_test.cmake
# Each case writes a compile_commands.json and runs the check on it as the build does, then compares its
# outcome. The large cases hold 5,000 entries, most of them a parent project's with response files and
# refused flags of their own, and must finish within two seconds: the check is linear in the document.

cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "contract-flags: WORK_DIR is not given")
endif()
set(script "${CMAKE_CURRENT_LIST_DIR}/../../cmake/IsolineContractFlags.cmake")
set(sourceDir "/isoline-fixture/source")
file(MAKE_DIRECTORY "${WORK_DIR}")
file(WRITE "${WORK_DIR}/refused.txt"
     "-fcontract-evaluation-semantic=ignore|does not evaluate the check\n-fno-contracts|turns contracts off\n")
file(WRITE "${WORK_DIR}/parent.rsp" "-fno-contracts\n")

## @brief Appends to @p outVar one compile command for @p file in @p directory with @p flags.
function(entry outVar directory file flags)
    set(text "${${outVar}}")
    if(NOT text STREQUAL "")
        string(APPEND text ",\n")
    endif()
    string(APPEND text "{\n  \"directory\": \"${directory}\",\n  \"command\": \"/usr/bin/g++ ${flags} -c ${file}\",\n  \"file\": \"${file}\",\n  \"output\": \"x.o\"\n}")
    set(${outVar} "${text}" PARENT_SCOPE)
endfunction()

## @brief The large document: @p count entries, every tenth one Isoline's (clean), the others a parent's,
## some of which read a response file or carry a refused flag. @p isolineFlags are added to the last
## Isoline entry.
function(largeDocument outVar count isolineFlags)
    set(text "")
    math(EXPR last "${count} - 1")
    math(EXPR lastTen "${count} - 10")
    foreach(index RANGE ${last})
        math(EXPR kind "${index} % 10")
        if(kind EQUAL 0)
            set(flags "-std=c++26 -fcontracts -fcontract-evaluation-semantic=enforce")
            if(index GREATER_EQUAL lastTen)
                string(APPEND flags " ${isolineFlags}")
            endif()
            entry(text "/build" "${sourceDir}/src/isoline/File${index}.cpp" "${flags}")
        elseif(kind EQUAL 1)
            entry(text "/build" "/parent/src/Parent${index}.cpp" "@${WORK_DIR}/parent.rsp -O2")
        elseif(kind EQUAL 2)
            entry(text "/build" "/parent/src/Parent${index}.cpp" "-fcontract-evaluation-semantic=ignore")
        else()
            entry(text "/build" "/parent/src/Parent${index}.cpp" "-O2 -DPARENT_${index}=1 -I/parent/include")
        endif()
    endforeach()
    set(${outVar} "[\n${text}\n]\n" PARENT_SCOPE)
endfunction()


## @brief Runs the check on @p document; @p expect is a regular expression its output must match, or
## PASS for a check that succeeds. A case with SECONDS must finish within them.
function(expectCheck name document expect)
    cmake_parse_arguments(PARSE_ARGV 3 arg "" "SECONDS;SOURCE" "COMPILER")
    if(NOT DEFINED arg_SOURCE)
        set(arg_SOURCE "${sourceDir}")
    endif()
    set(commands "${WORK_DIR}/${name}.json")
    file(WRITE "${commands}" "${document}")
    string(TIMESTAMP started "%s%f")
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DCOMPILE_COMMANDS=${commands}" "-DSOURCE_DIR=${arg_SOURCE}"
                            "-DREFUSED_FLAGS=${WORK_DIR}/refused.txt" "-DCXX_COMPILER=${arg_COMPILER}"
                            -P "${script}"
                    OUTPUT_VARIABLE output ERROR_VARIABLE output RESULT_VARIABLE result)
    string(TIMESTAMP finished "%s%f")
    math(EXPR milliseconds "(${finished} - ${started}) / 1000")
    set(problem "")
    if(expect STREQUAL "PASS")
        if(NOT result EQUAL 0)
            set(problem "failed: ${output}")
        endif()
    elseif(result EQUAL 0 OR NOT output MATCHES "${expect}")
        set(problem "did not refuse as expected: ${output}")
    endif()
    if(DEFINED arg_SECONDS AND milliseconds GREATER ${arg_SECONDS}000)
        string(APPEND problem " took ${milliseconds} ms, more than ${arg_SECONDS} s")
    endif()
    if(NOT problem STREQUAL "")
        set_property(GLOBAL APPEND_STRING PROPERTY failures "\n  ${name}: ${problem}")
    endif()
    set_property(GLOBAL APPEND PROPERTY cases "${name} (${milliseconds} ms)")
endfunction()

largeDocument(clean 5000 "")
expectCheck(large-clean "${clean}" PASS SECONDS 2)
largeDocument(refused 5000 "-fcontract-evaluation-semantic=ignore")
expectCheck(large-refused "${refused}" "src/isoline/File4990[.]cpp is compiled with -fcontract-evaluation-semantic=ignore" SECONDS 2)

# A parent entry whose flag holds `{},{}` in a string: the document is still read in linear time, and
# nothing in it is Isoline's to refuse.
largeDocument(poisoned 5000 "")
string(REPLACE "\"command\": \"/usr/bin/g++ -O2 -DPARENT_7=1" "\"command\": \"/usr/bin/g++ -O2 -DX=\\\"{},{}\\\" -DPARENT_7=1"
       poisoned "${poisoned}")
expectCheck(large-braces-in-a-parent-string "${poisoned}" PASS SECONDS 2)

# Braces inside strings never end an entry: a parent's output holding `a},{}`, an Isoline file whose name
# holds `},{`, a `file` member before the others, and the `arguments` form with `},{` in an argument.
expectCheck(parent-string-with-a-brace "[
{\"directory\":\"/build\",\"command\":\"/usr/bin/g++ -fno-contracts -c /parent/p.cpp\",\"file\":\"/parent/p.cpp\",\"output\":\"a},{}\"},
{\"directory\":\"/build\",\"command\":\"/usr/bin/g++ -O2 -c ${sourceDir}/src/isoline/z.cpp\",\"file\":\"${sourceDir}/src/isoline/z.cpp\"}
]" PASS)
expectCheck(file-name-with-braces "[
{\"directory\":\"/build\",\"command\":\"/usr/bin/g++ -fno-contracts -c ${sourceDir}/src/isoline/a},{b.cpp\",\"file\":\"${sourceDir}/src/isoline/a},{b.cpp\",\"output\":\"x.o\"}
]" "src/isoline/a},{b[.]cpp is compiled with -fno-contracts")
expectCheck(file-member-first "[
{\"file\":\"${sourceDir}/src/isoline/a.cpp\",\"directory\":\"/build\",\"command\":\"/usr/bin/g++ -DX=\\\"},{\\\" -fno-contracts -c a.cpp\"}
]" "src/isoline/a[.]cpp is compiled with -fno-contracts")
expectCheck(arguments-form "[
{\"directory\":\"/build\",\"arguments\":[\"/usr/bin/g++\",\"-O2\",\"-c\",\"/parent/p.cpp\"],\"file\":\"/parent/p.cpp\"},
{\"directory\":\"/build\",\"arguments\":[\"/usr/bin/g++\",\"-fno-contracts\",\"-DY=},{\",\"-c\",\"${sourceDir}/src/isoline/x.cpp\"],\"file\":\"${sourceDir}/src/isoline/x.cpp\"},
{\"directory\":\"/build\",\"arguments\":[\"/usr/bin/g++\",\"-fno-contracts\",\"-c\",\"${sourceDir}/src/isoline/y.cpp\"],\"file\":\"${sourceDir}/src/isoline/y.cpp\"}
]" "src/isoline/x[.]cpp is compiled with -fno-contracts.*src/isoline/y[.]cpp is compiled with -fno-contracts")
# An absolute path through `..` is normalized before it is compared.
expectCheck(absolute-path-through-dot-dot "[
{\"directory\":\"/build\",\"command\":\"/usr/bin/g++ -fno-contracts -c x.cpp\",\"file\":\"/isoline-fixture/build/../source/src/isoline/x.cpp\"}
]" "src/isoline/x[.]cpp is compiled with -fno-contracts")
# What is not a JSON array of objects is an error, not a pass.
expectCheck(unbalanced-document "[\n{\"directory\":\"/build\",\"file\":\"x.cpp\",\"command\":\"-fno-contracts\"\n" "not[ \n]+a[ \n]+well-formed[ \n]+JSON[ \n]+array")

# Long strings: a string is one token whatever its length (a pattern that recursed once per character
# would overflow the stack), and its escapes are restored exactly. A 100 KB definition, a 1 MB command
# line, and 40,000 escapes, each in an Isoline entry that is refused; then the shape of the reviewer's
# thr.json, a 25 KB parent command beside a clean Isoline entry, which passes.
string(REPEAT "a" 100000 longValue)
set(long "")
entry(long "/build" "${sourceDir}/src/isoline/Long.cpp" "-DLONG=${longValue} -fno-contracts")
expectCheck(hundred-kilobyte-string "[\n${long}\n]\n" "src/isoline/Long[.]cpp is compiled with -fno-contracts" SECONDS 2)
string(REPEAT " -DMANY=1" 120000 manyDefinitions)
set(megabyte "")
entry(megabyte "/build" "${sourceDir}/src/isoline/Megabyte.cpp" "${manyDefinitions} -fno-contracts")
expectCheck(megabyte-command-line "[\n${megabyte}\n]\n" "src/isoline/Megabyte[.]cpp is compiled with -fno-contracts" SECONDS 2)
string(REPEAT "\\\\\\\"" 20000 escapes)
set(escaped "")
entry(escaped "/build" "${sourceDir}/src/isoline/Escapes.cpp" "-DQ=${escapes} -fno-contracts")
entry(escaped "/build" "/parent/src/Escapes.cpp" "-DQ=\\\\\\\"},{\\\\ -fno-contracts")
expectCheck(forty-thousand-escapes "[\n${escaped}\n]\n" "src/isoline/Escapes[.]cpp is compiled with -fno-contracts" SECONDS 2)
string(REPEAT " -I/usr/include/x" 1500 includes)
set(threshold "")
entry(threshold "/build" "/parent/p.cpp" "-O2${includes}")
entry(threshold "/build" "${sourceDir}/src/isoline/z.cpp" "-fcontracts")
entry(threshold "/build" "/parent/q.cpp" "-fno-contracts")
expectCheck(reviewer-thr "[\n${threshold}\n]\n" PASS SECONDS 2)

# A string that ends in an escaped backslash closes at its next quote: here a parent's output and then
# an Isoline command whose last argument does.
expectCheck(trailing-escaped-backslash "[
{\"directory\":\"/build\",\"command\":\"g++ -O2 -c p.cpp\",\"file\":\"/parent/p.cpp\",\"output\":\"x\\\\\"},
{\"directory\":\"/build\",\"command\":\"g++ -fno-contracts -DPATH=C:\\\\\",\"file\":\"${sourceDir}/src/isoline/Trailing.cpp\",\"output\":\"end\\\\\"}
]" "src/isoline/Trailing[.]cpp is compiled with -fno-contracts")

# A flag the shell would unquote is refused as the compiler sees it.
set(quoted "")
entry(quoted "/build" "${sourceDir}/src/isoline/Quoted.cpp" "-fcontract-evaluation-semantic=\\\"ignore\\\"")
expectCheck(quoted-flag "[\n${quoted}\n]\n" "src/isoline/Quoted[.]cpp is compiled with -fcontract-evaluation-semantic=ignore")
# The `arguments` form with 50,000 arguments, one of them holding escapes, `;` and brackets, is decoded in
# linear time.
string(REPEAT "\"-DMANY=1\"," 50000 manyArguments)
expectCheck(fifty-thousand-arguments "[
{\"directory\":\"/build\",\"arguments\":[\"/usr/bin/g++\",${manyArguments}\"-DQ=\\\"a;b[c]\\\\\",\"-fno-contracts\",\"-c\",\"x.cpp\"],\"file\":\"${sourceDir}/src/isoline/Arguments.cpp\"}
]" "src/isoline/Arguments[.]cpp is compiled with -fno-contracts" SECONDS 2)
# A raw control character is not JSON, and could be taken for a placeholder: refused.
string(ASCII 2 rawControl)
expectCheck(raw-control-character "[
{\"directory\":\"/build\",\"command\":\"g++ -fno-contracts ${rawControl}\",\"file\":\"${sourceDir}/src/isoline/Raw.cpp\"}
]" "control[ \n]+character[ \n]+2[ \n]+unescaped")

# A `\uXXXX` escape decodes to the character it names, so an escaped refused flag or `@` is refused as
# the compiler sees it: in the `command` form, in the `arguments` form, and as a response file's `@`.
## @brief Expects the check to refuse one Isoline entry whose compile command is the JSON member
## @p member, as compiled with @p flag.
function(expectUnicodeEscape name member flag)
    expectCheck(unicode-escape-${name}
                "[{\"directory\":\"/build\",${member},\"file\":\"${sourceDir}/src/isoline/Unicode.cpp\"}]"
                "src/isoline/Unicode[.]cpp is compiled with ${flag}")
endfunction()
expectUnicodeEscape(command "\"command\":\"g++ \\u002dfno-contracts -c a.cpp\"" -fno-contracts)
expectUnicodeEscape(arguments "\"arguments\":[\"g++\",\"\\u002dfno-contracts\"]" -fno-contracts)
expectUnicodeEscape(escaped-equals "\"command\":\"g++ -fcontract-evaluation-semantic\\u003dignore\""
                    -fcontract-evaluation-semantic=ignore)
expectUnicodeEscape(arguments-mid-flag "\"arguments\":[\"g++\",\"-fno-\\u0063ontracts\"]" -fno-contracts)
expectCheck(unicode-escaped-response-file
            "[{\"directory\":\"/build\",\"command\":\"g++ \\u0040unicode.rsp\",\"file\":\"${sourceDir}/src/isoline/Unicode.cpp\"}]"
            "unicode[.]rsp,[ \n]+which[ \n]+cannot[ \n]+be[ \n]+read")

# A relative file is resolved against its directory: here neither names Isoline's source tree, and the
# file is Isoline's all the same.
set(relative "")
entry(relative "/isoline-fixture/build" "../source/src/isoline/Relative.cpp" "-fno-contracts")
expectCheck(relative-file "[\n${relative}\n]\n" "src/isoline/Relative[.]cpp is compiled with -fno-contracts")

# A response file of Isoline's: its refused flag is named with it; one that cannot be read is an error.
file(WRITE "${WORK_DIR}/isoline.rsp" "-O0 -fcontract-evaluation-semantic=ignore\n")
set(response "")
entry(response "/build" "${sourceDir}/src/isoline/Response.cpp" "@${WORK_DIR}/isoline.rsp")
expectCheck(response-file "[\n${response}\n]\n"
            "src/isoline/Response[.]cpp is compiled with -fcontract-evaluation-semantic=ignore [(]from the response file")
set(missing "")
entry(missing "/build" "${sourceDir}/src/isoline/Missing.cpp" "@${WORK_DIR}/missing.rsp")
expectCheck(missing-response-file "[\n${missing}\n]\n" "missing[.]rsp,[ \n]+which[ \n]+cannot[ \n]+be[ \n]+read")

# GCC specs files add flags to the compiler proper's command line, the semantic among them: each is read as
# the driver reads it, the files it names by %include too. A bare name is found where the compiler finds
# it (-print-file-name=, here a stand-in that knows one directory), else in the entry's directory; one that
# cannot be read is an error, unless %include_noerr names it.
file(MAKE_DIRECTORY "${WORK_DIR}/specs-library")
file(WRITE "${WORK_DIR}/override.specs" "*cc1plus:\n+ -fcontract-evaluation-semantic=ignore\n")
file(WRITE "${WORK_DIR}/clean.specs" "*cc1plus:\n+ -O0\n%include_noerr <absent.specs>\n")
file(WRITE "${WORK_DIR}/outer.specs" "%include <${WORK_DIR}/override.specs>\n")
file(WRITE "${WORK_DIR}/specs-library/library.specs" "*cc1plus:\n+ -fno-contracts\n")
file(WRITE "${WORK_DIR}/print-file-name.cmake" [=[
math(EXPR last "${CMAKE_ARGC} - 1")
foreach(index RANGE ${last})
    if(CMAKE_ARGV${index} MATCHES "^-print-file-name=(.*)$")
        set(found "${CMAKE_MATCH_1}")
        if(EXISTS "${LIBRARY}/${CMAKE_MATCH_1}")
            set(found "${LIBRARY}/${CMAKE_MATCH_1}")
        endif()
        execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${found}")
    endif()
endforeach()
]=])
set(fakeCompiler "${CMAKE_COMMAND}" "-DLIBRARY=${WORK_DIR}/specs-library" -P "${WORK_DIR}/print-file-name.cmake")
## @brief Expects the check on one Isoline entry compiled with @p flags in @p directory to match @p expect.
## A case whose directory is a real one (WORK_DIR) takes its source tree there too: on Windows the fixture's
## root-relative /isoline-fixture/source resolves against a directory with a drive to another tree.
function(expectSpecs name directory flags expect)
    set(source "${sourceDir}")
    if(NOT directory STREQUAL "/build")
        set(source "${directory}/source")
    endif()
    set(document "")
    entry(document "${directory}" "${source}/src/isoline/Specs.cpp" "${flags}")
    expectCheck(specs-${name} "[\n${document}\n]\n" "${expect}" COMPILER ${fakeCompiler} SOURCE "${source}")
endfunction()
expectSpecs(file "/build" "-specs=${WORK_DIR}/override.specs"
            "src/isoline/Specs[.]cpp is compiled with -fcontract-evaluation-semantic=ignore [(]from the specs file .*override[.]specs[)]")
expectSpecs(double-dash "/build" "--specs=${WORK_DIR}/override.specs" "from the specs file .*override[.]specs")
expectSpecs(relative-to-directory "${WORK_DIR}" "-specs=override.specs" "from the specs file .*override[.]specs")
expectSpecs(found-by-the-compiler "/build" "-specs=library.specs"
            "compiled with -fno-contracts [(]from the specs file .*specs-library/library[.]specs[)]")
expectSpecs(included "/build" "-specs=${WORK_DIR}/outer.specs" "from the specs file .*override[.]specs")
expectSpecs(clean-with-an-optional-include "/build" "-specs=${WORK_DIR}/clean.specs" PASS)
expectSpecs(missing "/build" "-specs=nowhere.specs" "nowhere[.]specs,[ \n]+which[ \n]+cannot[ \n]+be[ \n]+read")
file(WRITE "${WORK_DIR}/specs.rsp" "-specs=${WORK_DIR}/override.specs\n")
expectSpecs(named-by-a-response-file "/build" "@${WORK_DIR}/specs.rsp" "from the specs file .*override[.]specs")

get_property(failures GLOBAL PROPERTY failures)
get_property(cases GLOBAL PROPERTY cases)
if(NOT "${failures}" STREQUAL "")
    message(FATAL_ERROR "contract-flags: cases whose outcome differs:${failures}")
endif()
list(JOIN cases ", " cases)
message(STATUS "contract-flags: all cases hold: ${cases}")
