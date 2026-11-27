/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The harness of Isoline's C tests: plain C11, header-only, built on the public C headers alone.
 *
 * A test file names its suite and lists its cases before it includes this header, then defines each
 * case and hands main() to the runner:
 *
 *     #define C_TEST_SUITE "abi"
 *     #define C_TEST_CASES(X) X(versionMatchesHeader) X(compatibilityRule)
 *     #include "IsolineCTest.h"
 *
 *     ISOLINE_TEST_CASE(versionMatchesHeader) { CHECK(isoline_abi_version() == ...); }
 *
 *     int main(int argc, char** argv) { return c_test_run_all(argc, argv); }
 *
 * The list is the table the runner walks, and the compiler holds it to the definitions: a listed case
 * that is never defined is a static function used but not defined, and a defined case the list leaves
 * out is an unused static function, both errors under the C tests' warning set.
 *
 * CHECK(condition) records a failure and carries on; REQUIRE(condition) records it and returns from
 * the case. `-c <case>` runs one case, named "<suite>.<case>" or "<case>". The exit status is 0 when
 * every case that ran passed, 1 when one failed and 2 for a usage error, a filter that matches no case
 * included.
 *
 * Its names carry no isoline_ prefix, which belongs to the library's own API.
 */
#pragma once

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#if !defined(C_TEST_SUITE) || !defined(C_TEST_CASES)
    #error "define C_TEST_SUITE and C_TEST_CASES(X) before including IsolineCTest.h"
#endif

/** What the running case has recorded. */
struct c_test_state
{
    size_t failures; /**< The checks of the case that failed so far. */
};

/** One row of the case table: the case's name and its body. */
struct c_test_case
{
    char const* name;
    void (*run)(struct c_test_state* current);
};

#define C_TEST_DECLARE(name) static void c_test_case_##name(struct c_test_state* c_test_current);
C_TEST_CASES(C_TEST_DECLARE)
#undef C_TEST_DECLARE

/** Defines the body of the listed case @p name. */
#define ISOLINE_TEST_CASE(name) static void c_test_case_##name(struct c_test_state* c_test_current)

/** Records a failure of @p condition, with its text and place, and carries on. */
#define CHECK(condition) \
    ((void) c_test_check(c_test_current, (condition) ? 1 : 0, #condition, __FILE__, __LINE__))

/** Records a failure of @p condition and returns from the case. */
#define REQUIRE(condition)                                                                      \
    do                                                                                          \
    {                                                                                           \
        if (!c_test_check(c_test_current, (condition) ? 1 : 0, #condition, __FILE__, __LINE__)) \
            return;                                                                             \
    } while (0)

#define C_TEST_ENTRY(name) { #name, c_test_case_##name },
static struct c_test_case const c_test_cases[] = { C_TEST_CASES(C_TEST_ENTRY) };
#undef C_TEST_ENTRY

/** Records the outcome of one check. @return 1 when it held, 0 when it failed. */
static int c_test_check(struct c_test_state* current, int held, char const* text, char const* file, int line)
{
    if (!held)
    {
        ++current->failures;
        fprintf(stderr, "%s:%d: check failed: %s\n", file, line, text);
    }
    return held;
}

/** Whether @p filter names the case @p name of this suite, as "<suite>.<case>" or as "<case>". */
static int c_test_matches(char const* filter, char const* name)
{
    size_t const suiteLength = strlen(C_TEST_SUITE);
    if (strcmp(filter, name) == 0)
        return 1;
    return strncmp(filter, C_TEST_SUITE, suiteLength) == 0 && filter[suiteLength] == '.'
           && strcmp(filter + suiteLength + 1, name) == 0;
}

/** Runs every case of the table, or the one `-c <case>` names. @return The process exit status. */
static int c_test_run_all(int argc, char** argv)
{
    char const* filter = NULL;
    size_t const count = sizeof c_test_cases / sizeof c_test_cases[0];
    size_t ran = 0;
    size_t failed = 0;
    size_t index = 0;

    if (argc == 3 && strcmp(argv[1], "-c") == 0)
        filter = argv[2];
    else if (argc != 1)
    {
        fprintf(stderr, "usage: %s [-c <case>]\n", argc > 0 ? argv[0] : "c-test");
        return 2;
    }

    for (index = 0; index < count; ++index)
    {
        struct c_test_state state = { 0 };
        struct c_test_case const* const testCase = &c_test_cases[index];
        if (filter != NULL && !c_test_matches(filter, testCase->name))
            continue;
        ++ran;
        testCase->run(&state);
        if (state.failures != 0)
            ++failed;
        printf("%s %s.%s\n", state.failures == 0 ? "passed" : "FAILED", C_TEST_SUITE, testCase->name);
    }

    if (filter != NULL && ran == 0)
    {
        fprintf(stderr, "no case of suite %s matches '%s'\n", C_TEST_SUITE, filter);
        return 2;
    }
    printf("%s: %zu case(s) ran, %zu failed\n", C_TEST_SUITE, ran, failed);
    return failed == 0 ? 0 : 1;
}
