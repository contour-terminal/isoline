# SPDX-License-Identifier: Apache-2.0

# isoline::fastinterpreter: the fast interpreter, which runs code lowered once, at load, from a
# validated module into register-based operations over frame slots.

set(isolineFastAvailabilityTemplate "${CMAKE_CURRENT_SOURCE_DIR}/DispatchAvailability.hpp.in")
set(isolineFastAvailabilityHeader "${PROJECT_BINARY_DIR}/include/isoline/fastinterpreter/DispatchAvailability.hpp")

# The tail-call dispatch's handler files, one per OperationFamily group (TailCallControl.cpp's FamilyFiles
# names the same): compiled only where a probe row passes. TailCallControl.cpp is the file the probe
# compiles. Where none passes, TailCallUnavailable.cpp stands in for their entry points.
set(isolineTailCallFamilyFiles TailCallControl.cpp TailCallNumeric.cpp TailCallMemory.cpp)
set(isolineTailCallUnavailableFiles TailCallUnavailable.cpp)

## @brief Writes a DispatchAvailability.hpp from the template to @p header, for @p row, the tail-call probe
## row that passed or is being probed, or "" for none.
function(isoline_write_dispatch_availability header row)
    set(ISOLINE_FAST_DISPATCHES "Dispatch::Switch")
    set(ISOLINE_FAST_MUSTTAIL_DEFINITION "// No guaranteed tail call: TailCall is not compiled.")
    if(row)
        string(APPEND ISOLINE_FAST_DISPATCHES ", Dispatch::TailCall")
        set(ISOLINE_FAST_MUSTTAIL_DEFINITION "#define ISOLINE_FAST_MUSTTAIL [[${row}]]")
    endif()
    set(ISOLINE_FAST_PASSED_PROBE_ROW "${row}")
    list(TRANSFORM ISOLINE_TAILCALL_PROBE_ROWS REPLACE "^(.*)$" "\"\\1\"" OUTPUT_VARIABLE rows)
    list(JOIN rows ", " ISOLINE_FAST_PROBE_ROWS)
    list(TRANSFORM ISOLINE_TAILCALL_PROBE_FAMILIES REPLACE "^(.*)$" "\"\\1\"" OUTPUT_VARIABLE families)
    list(JOIN families ", " ISOLINE_FAST_PROBE_FAMILIES)
    configure_file("${isolineFastAvailabilityTemplate}" "${header}" @ONLY)
endfunction()

## @brief Probes each tail-call row of this compiler's family in turn, and sets @p resultVar to the first that
## compiles the real TailCallControl.cpp with the target policy's flags and warnings as errors, or to "" when
## none does. A compiler no row names is not probed at all: MSVC has no guaranteed tail call, and an attribute
## it does not know compiles into plain calls, so its build is pinned to Switch by the rows themselves.
function(isoline_probe_tail_call resultVar)
    isoline_target_policy_flags(compileOptions linkOptions)
    # Warnings as errors, an unknown attribute's included, which the policy may mute: a compiler that does
    # not know a row's spelling ignores it and compiles plain calls, which are no tail calls at all. clang-cl
    # takes Clang's own -W options beside MSVC's /WX.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        list(APPEND compileOptions -Werror -Werror=attributes)
    elseif(MSVC)
        list(APPEND compileOptions /WX -Werror=unknown-attributes)
    else()
        list(APPEND compileOptions -Werror -Werror=unknown-attributes)
    endif()
    # The build's own configuration, so that the probe compiles at the build's optimization level. A
    # multi-config generator has none to name; its probe compiles unoptimized, which is the stricter case: a
    # tail call that is guaranteed there (no call inlined away, every local in memory) is guaranteed optimized.
    if(CMAKE_BUILD_TYPE)
        set(CMAKE_TRY_COMPILE_CONFIGURATION "${CMAKE_BUILD_TYPE}")
    endif()
    # core-cpp's headers, by the paths its build interface names (or an installed package's plain ones).
    get_target_property(coreBase core::base ALIASED_TARGET)
    if(NOT coreBase)
        set(coreBase core::base)
    endif()
    get_target_property(coreIncludes ${coreBase} INTERFACE_INCLUDE_DIRECTORIES)
    list(TRANSFORM coreIncludes REPLACE "^\\$<BUILD_INTERFACE:(.*)>$" "\\1")
    list(FILTER coreIncludes EXCLUDE REGEX "^\\$<")
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    foreach(row families IN ZIP_LISTS ISOLINE_TAILCALL_PROBE_ROWS ISOLINE_TAILCALL_PROBE_FAMILIES)
        string(REPLACE " " ";" families "${families}")
        if(NOT CMAKE_CXX_COMPILER_ID IN_LIST families)
            continue()
        endif()
        string(MAKE_C_IDENTIFIER "${row}" rowName)
        set(probeInclude "${PROJECT_BINARY_DIR}/dispatch-probe/${rowName}/include")
        isoline_write_dispatch_availability(
            "${probeInclude}/isoline/fastinterpreter/DispatchAvailability.hpp" "${row}")
        set(includes "${probeInclude}" "${PROJECT_SOURCE_DIR}/src" "${PROJECT_BINARY_DIR}/include" ${coreIncludes})
        # Probed at every configure, never cached: a row that failed once (the file did not compile then)
        # must not stay failed, nor one that passed stay passed once the file stopped compiling with it.
        try_compile(probePassed
            SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/TailCallControl.cpp"
            CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${includes}"
            COMPILE_DEFINITIONS ${compileOptions}
            CXX_STANDARD 23
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
            OUTPUT_VARIABLE probeOutput
            NO_CACHE)
        if(probePassed)
            message(STATUS "[isoline] tail-call probe row ${row}: passed")
            file(REMOVE "${PROJECT_BINARY_DIR}/dispatch-probe/${rowName}/output.log")
            set(${resultVar} "${row}" PARENT_SCOPE)
            return()
        endif()
        message(STATUS "[isoline] tail-call probe row ${row}: failed")
        file(WRITE "${PROJECT_BINARY_DIR}/dispatch-probe/${rowName}/output.log" "${probeOutput}")
    endforeach()
    set(${resultVar} "" PARENT_SCOPE)
endfunction()

if(ISOLINE_FAST_DISPATCH_TAILCALL STREQUAL "AUTO")
    # An edit of the file the probe compiles, of the handler it instantiates or of the bodies and registers
    # the handler holds, probes again.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 TailCallControl.cpp TailCall.hpp Operations.hpp Context.hpp)
    isoline_probe_tail_call(isolineTailCallRow)
elseif(ISOLINE_FAST_DISPATCH_TAILCALL STREQUAL "OFF")
    set(isolineTailCallRow "")
else()
    message(FATAL_ERROR "ISOLINE_FAST_DISPATCH_TAILCALL must be AUTO or OFF, not '${ISOLINE_FAST_DISPATCH_TAILCALL}'.")
endif()
isoline_write_dispatch_availability("${isolineFastAvailabilityHeader}" "${isolineTailCallRow}")

# The dispatch rows: the dispatches this build compiles, from the most portable to the fastest, and their
# sources. Switch is portable C++ and always the first row.
set(isolineDispatchSources SwitchDispatch.cpp SwitchDispatchCold.cpp)
if(isolineTailCallRow)
    message(STATUS "[isoline] fast interpreter dispatches: Switch TailCall (${isolineTailCallRow})")
    list(APPEND isolineDispatchSources ${isolineTailCallFamilyFiles})
else()
    message(STATUS "[isoline] fast interpreter dispatches: Switch")
    list(APPEND isolineDispatchSources ${isolineTailCallUnavailableFiles})
endif()

# The frame-size rows of the dispatch files, per build flavour: an optimized build without sanitizers or
# coverage keeps each frame within 4 KiB (a deep host re-entry nests these frames on a small thread); a
# sanitizer, coverage or unoptimized build, whose frames carry redzones and counters, records its frame
# sizes instead (-fstack-usage writes a .su file beside each object).
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU" OR (NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT
                                                          AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
    set(isolineOptimized "$<CONFIG:Release,RelWithDebInfo,MinSizeRel>")
    if(ISOLINE_SANITIZE OR ISOLINE_COVERAGE)
        set(isolineDispatchFrameOptions -fstack-usage)
    else()
        set(isolineDispatchFrameOptions "$<IF:${isolineOptimized},-Wframe-larger-than=4096,-fstack-usage>")
    endif()
    # Source properties are per directory: these are the ones of isoline_core's, which compiles the files.
    set_property(SOURCE ${isolineDispatchSources} TARGET_DIRECTORY isoline_core
                 APPEND PROPERTY COMPILE_OPTIONS ${isolineDispatchFrameOptions})
endif()

target_sources(isoline_core
    PRIVATE
        Code.cpp
        Dispatch.cpp
        FastInterpreter.cpp
        Listing.cpp
        Lowering.cpp
        LoweringArena.cpp
        SlotCodec.cpp
        ${isolineDispatchSources}
    PUBLIC FILE_SET HEADERS
        BASE_DIRS "${PROJECT_SOURCE_DIR}/src" "${PROJECT_BINARY_DIR}/include"
        FILES
            Backends.hpp
            Code.hpp
            Context.hpp
            Dispatch.hpp
            FastInterpreter.hpp
            LittleEndian.hpp
            Listing.hpp
            Lowering.hpp
            LoweringArena.hpp
            LoweringTable.hpp
            Operations.def
            Operations.hpp
            SlotCodec.hpp
            TailCall.hpp
            "${isolineFastAvailabilityHeader}")
