# SPDX-License-Identifier: Apache-2.0
#
# Installs the C API's install component, isoline-c, into a scratch prefix and checks its layout:
#
#   cmake -DBUILD=<build directory> -DSOURCE=<source directory> -DPREFIX=<scratch prefix> [-DCONFIG=<config>]
#         -DLIBDIR=<the install's library directory, CMAKE_INSTALL_LIBDIR>
#         -DCXX_COMPILER=<c++> "-DCXX_FLAGS=<flags>" -DCXX_FRONTEND=<GNU|MSVC>
#         [-DC_COMPILER=<cc> -DPKG_CONFIG=<pkg-config> "-DC_FLAGS=<flags>" "-DLINK_FLAGS=<flag>;..."]
#         [-DNM=<nm> -DCONTRACTS_PATH=<native|fallback>]
#         -P CheckInstall.cmake
#
# - Every C header of src/isoline/c/ and wasm.h are installed beneath include/isoline/, and nothing is
#   installed as include/wasm.h, where another runtime's wasm.h lives.
# - The shared library is installed in the library directory with the package's component c
#   (isolineCTargets.cmake), and the static library with the static libraries it links beneath
#   <libdir>/isoline/, the C API's own directory; no C++ header is installed.
# - isoline-c.pc gives -I<includedir>/isoline, the static library's definitions (Cflags.private), and the
#   libraries a static link needs (Libs.private): their directory, isoline_core, core-cpp and the C++
#   runtime the compiler itself links, as its driver's link line (-###) shows.
# A consumer of the CMake package (isoline::c) is check-install's mode c (tests/consumer/ConsumerModes.cmake),
# which installs the package's core-cpp beside it.
# - With PKG_CONFIG, a C program is compiled and linked by the C compiler from nothing but what
#   `pkg-config --cflags --libs isoline-c` says, and runs against the installed shared library; then,
#   with the shared library removed from the prefix, from what `pkg-config --static --cflags --libs
#   isoline-c` says, and runs with no shared library of Isoline anywhere on its search path. The program
#   makes and deletes a wasm_valtype_t, so the static link needs the C++ libraries behind it, which only
#   Libs.private names: an empty Libs.private fails the static link. Where contracts are native, the static
#   link also pulls in an object that calls the contract violation handler, so a Libs.private without
#   stdc++exp fails it too (NM finds the object). LINK_FLAGS
#   are what this build's instrumentation (sanitizers, coverage) needs on any link of its code; an
#   uninstrumented build passes none.

cmake_minimum_required(VERSION 3.25)

# The library directory is the build's, never a guess: a prefix may hold lib/ and lib64/ both.
if(NOT LIBDIR)
    message(FATAL_ERROR "CheckInstall: LIBDIR (CMAKE_INSTALL_LIBDIR) is not given")
endif()
file(REMOVE_RECURSE "${PREFIX}")
set(configArguments "")
if(CONFIG)
    set(configArguments --config "${CONFIG}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD}" --prefix "${PREFIX}" --component isoline-c
                        ${configArguments}
                RESULT_VARIABLE status OUTPUT_QUIET ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "CheckInstall: cmake --install failed (${status}): ${errors}")
endif()
set(missing "")

# The headers: every C header the source tree has, beneath include/isoline/.
file(GLOB headers RELATIVE "${SOURCE}/src" "${SOURCE}/src/isoline/c/*.h")
set(expected "include/isoline/wasm.h" "${LIBDIR}/pkgconfig/isoline-c.pc" "${LIBDIR}/cmake/isoline/isolineCTargets.cmake"
             "${LIBDIR}/isoline/*isoline_core*")
foreach(header IN LISTS headers)
    list(APPEND expected "include/isoline/${header}")
endforeach()
foreach(pattern IN LISTS expected)
    file(GLOB matches "${PREFIX}/${pattern}")
    if(NOT matches)
        string(APPEND missing "\n  ${pattern}")
    endif()
endforeach()
if(EXISTS "${PREFIX}/include/wasm.h")
    string(APPEND missing "\n  no include/wasm.h, which is another runtime's place")
endif()
file(GLOB_RECURSE cxxHeaders "${PREFIX}/*.hpp")
if(cxxHeaders)
    string(APPEND missing "\n  no C++ header, but found: ${cxxHeaders}")
endif()
file(GLOB_RECURSE libraries RELATIVE "${PREFIX}" "${PREFIX}/*isoline_c*")
list(FILTER libraries EXCLUDE REGEX "^include/")
list(LENGTH libraries libraryCount)
if(libraryCount LESS 2)
    string(APPEND missing "\n  the shared and the static library (found: ${libraries})")
endif()

# The C++ runtime the compiler's driver links, from its link line: what Libs.private must name.
set(runtime "")
if(CXX_FRONTEND STREQUAL "GNU")
    set(probe "${PREFIX}.probe.cpp")
    file(WRITE "${probe}" "int main() { return 0; }\n")
    separate_arguments(flags NATIVE_COMMAND "${CXX_FLAGS}")
    execute_process(COMMAND "${CXX_COMPILER}" ${flags} "-###" "${probe}" -o "${PREFIX}.probe"
                    RESULT_VARIABLE status OUTPUT_VARIABLE driverOutput ERROR_VARIABLE driverErrors)
    file(REMOVE "${probe}")
    string(APPEND driverOutput "${driverErrors}")
    if(driverOutput MATCHES "\"?-lc[+][+]\"?[ \n]")
        set(runtime "-lc++")
    elseif(driverOutput MATCHES "\"?-lstdc[+][+]\"?[ \n]")
        set(runtime "-lstdc++")
    else()
        string(APPEND missing "\n  a C++ runtime on the compiler's link line (${CXX_COMPILER} -###)")
    endif()
endif()

set(pc "${PREFIX}/${LIBDIR}/pkgconfig/isoline-c.pc")
if(EXISTS "${pc}")
    file(STRINGS "${pc}" pcLines)
    set(fields "")
    foreach(line IN LISTS pcLines)
        if(line MATCHES "^([A-Za-z.]+)[:=] ?(.*)$")
            set(field_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
        endif()
    endforeach()
    if(NOT field_Cflags STREQUAL "-I\${includedir}/isoline")
        string(APPEND missing "\n  Cflags: -I\${includedir}/isoline (found: '${field_Cflags}')")
    endif()
    if(NOT field_Cflags.private STREQUAL "-DISOLINE_C_STATIC -DLIBWASM_STATIC")
        string(APPEND missing "\n  Cflags.private: -DISOLINE_C_STATIC -DLIBWASM_STATIC (found: '${field_Cflags.private}')")
    endif()
    separate_arguments(libsPrivate UNIX_COMMAND "${field_Libs.private}")
    foreach(library IN ITEMS "-L\${libdir}/isoline" -lisoline_core -lcore-cpp-base)
        if(NOT library IN_LIST libsPrivate)
            string(APPEND missing "\n  ${library} in Libs.private (found: '${field_Libs.private}')")
        endif()
    endforeach()
    set(namedRuntimes "${libsPrivate}")
    list(FILTER namedRuntimes INCLUDE REGEX "^-l(stdc|c)[+][+]$")
    if(NOT namedRuntimes STREQUAL runtime)
        string(APPEND missing "\n  Libs.private naming the runtime '${runtime}' (it names '${namedRuntimes}')")
    endif()
    if(NOT field_prefix MATCHES "^[$][{]pcfiledir[}]/")
        string(APPEND missing "\n  a prefix relative to the .pc file, so the install relocates (found: '${field_prefix}')")
    endif()
endif()

# A consumer of pkg-config, built by the C compiler and run: first against the shared library, then
# statically, once the shared library is gone from the prefix.
if(PKG_CONFIG)
    set(pcDirectory "${PREFIX}/${LIBDIR}/pkgconfig")
    set(libraryDirectory "${PREFIX}/${LIBDIR}")
    set(consumer "${PREFIX}.pkg-config")
    file(REMOVE_RECURSE "${consumer}")
    file(WRITE "${consumer}/consumer.c"
         "#include <stdio.h>\n#include <wasm.h>\n#include <isoline/c/isoline.h>\n"
         "int main(void)\n{\n"
         "    wasm_valtype_t* type = NULL;\n"
         "    if (!isoline_abi_compatible(ISOLINE_ABI_VERSION))\n        return 1;\n"
         "    /* C++ behind it: a static link needs the libraries Libs.private names. */\n"
         "    type = wasm_valtype_new(WASM_I64);\n"
         "    if (type == NULL || wasm_valtype_kind(type) != WASM_I64)\n        return 2;\n"
         "    wasm_valtype_delete(type);\n"
         "    printf(\"isoline-c ABI %u\\n\", (unsigned) isoline_abi_version());\n"
         "    return 0;\n}\n")
    separate_arguments(cFlags NATIVE_COMMAND "${C_FLAGS}")
    # Only the prefix's .pc files are visible, and only the prefix's libraries are loaded.
    set(pcEnvironment "PKG_CONFIG_LIBDIR=${pcDirectory}" "PKG_CONFIG_PATH=")
    if(APPLE)
        set(loaderVariable DYLD_LIBRARY_PATH)
    else()
        set(loaderVariable LD_LIBRARY_PATH)
    endif()
    # The static link pulls in an object of the closure that calls the contract violation handler, whatever the
    # program uses: on a native-contracts build that is every object with a contracted function (Engine.cpp.o
    # among them), which leaves handle_contract_violation undefined for stdc++exp, so a Libs.private without
    # -lstdc++exp fails the link. The object is pulled by one symbol it defines (-u); where contracts are
    # native, a closure with no such object is itself a failure, so the proof cannot vanish.
    set(contractObjectLinks "")
    if(NM)
        file(GLOB coreArchives "${PREFIX}/${LIBDIR}/isoline/*isoline_core*")
        foreach(archive IN LISTS coreArchives)
            execute_process(COMMAND "${NM}" -A "${archive}" OUTPUT_VARIABLE symbols ERROR_QUIET)
            string(REGEX MATCH "([^\n:]+[.]o[bj]*):[ ]+U [^\n]*handle_contract_violation" contractUse "${symbols}")
            if(contractUse)
                set(member "${CMAKE_MATCH_1}")
                string(REGEX MATCH ":${member}:[0-9a-f ]+ T ([^\n]+)" defined "${symbols}")
                if(defined)
                    set(contractObjectLinks "-Wl,-u,${CMAKE_MATCH_1}")
                    message(STATUS "CheckInstall: the static consumer pulls in ${member}, which calls the contract "
                                   "violation handler")
                endif()
            endif()
        endforeach()
    endif()
    if(CONTRACTS_PATH STREQUAL "native" AND NOT contractObjectLinks)
        string(APPEND missing "\n  an object of the static closure that calls handle_contract_violation (nm: '${NM}')")
    endif()
    foreach(variant IN ITEMS shared static)
        set(pcArguments --cflags --libs isoline-c)
        set(runEnvironment "${loaderVariable}=${libraryDirectory}")
        if(variant STREQUAL "static")
            set(pcArguments --static ${pcArguments})
            set(runEnvironment "${loaderVariable}=")
            file(GLOB sharedLibraries "${libraryDirectory}/libisoline_c.so*" "${libraryDirectory}/libisoline_c.*dylib")
            file(REMOVE ${sharedLibraries})
        endif()
        execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${pcEnvironment} "${PKG_CONFIG}" ${pcArguments}
                        RESULT_VARIABLE status OUTPUT_VARIABLE pcFlags ERROR_VARIABLE errors
                        OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT status EQUAL 0)
            string(APPEND missing "\n  pkg-config ${pcArguments} (${status}): ${errors}")
            continue()
        endif()
        separate_arguments(pcFlags UNIX_COMMAND "${pcFlags}")
        set(program "${consumer}/consumer-${variant}")
        set(forcedObjects "")
        if(variant STREQUAL "static")
            set(forcedObjects ${contractObjectLinks})
        endif()
        set(compile "${C_COMPILER}" ${cFlags} "${consumer}/consumer.c" ${forcedObjects} ${pcFlags} ${LINK_FLAGS}
                    -o "${program}")
        execute_process(COMMAND ${compile} RESULT_VARIABLE status OUTPUT_VARIABLE errors ERROR_VARIABLE errors)
        list(JOIN compile " " compileLine)
        if(NOT status EQUAL 0)
            string(APPEND missing "\n  a ${variant} pkg-config consumer that builds (${status}): ${compileLine}\n${errors}")
            continue()
        endif()
        execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${runEnvironment} "${program}"
                        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors)
        if(NOT status EQUAL 0 OR NOT output MATCHES "^isoline-c ABI [0-9]+\n$")
            string(APPEND missing "\n  a ${variant} pkg-config consumer that runs (${status}, '${output}'): "
                                  "${compileLine}\n${errors}")
        else()
            message(STATUS "CheckInstall: the ${variant} pkg-config consumer runs: ${compileLine}")
        endif()
    endforeach()
    file(REMOVE_RECURSE "${consumer}")
endif()

file(REMOVE_RECURSE "${PREFIX}")
if(missing)
    message(FATAL_ERROR "CheckInstall: the install into ${PREFIX} lacks:${missing}")
endif()
message(STATUS "CheckInstall: the C API's layout is complete (C++ runtime '${runtime}')")
