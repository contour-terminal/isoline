/* SPDX-License-Identifier: Apache-2.0 */
/* Errors as a C program sees them (error.h): made, read and deleted through the shared library. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <isoline/c/isoline.h>

#define C_TEST_SUITE    "error"
#define C_TEST_CASES(X) X(newCopiesMessage) X(embeddedNulKeptInLength) X(nullIsSafe) X(codeNames)
#include "IsolineCTest.h"

ISOLINE_TEST_CASE(newCopiesMessage)
{
    char message[] = "the callback failed";
    isoline_error_t* const error = isoline_error_new(ISOLINE_ERROR_EMBEDDER, message, strlen(message));
    REQUIRE(error != NULL);
    /* The error holds a copy: the caller's buffer may change or go. */
    memset(message, 'x', strlen(message));
    CHECK(isoline_error_code(error) == ISOLINE_ERROR_EMBEDDER);
    CHECK(isoline_error_detail(error) == 0);
    CHECK(isoline_error_message_length(error) == strlen("the callback failed"));
    CHECK(strcmp(isoline_error_message(error), "the callback failed") == 0);
    CHECK(isoline_error_cause(error) == NULL);
    isoline_error_delete(error);
}

ISOLINE_TEST_CASE(embeddedNulKeptInLength)
{
    static char const message[] = { 'a', '\0', 'b' };
    isoline_error_t* const error = isoline_error_new(ISOLINE_ERROR_EMBEDDER, message, sizeof message);
    REQUIRE(error != NULL);
    CHECK(isoline_error_message_length(error) == sizeof message);
    CHECK(memcmp(isoline_error_message(error), message, sizeof message) == 0);
    /* Terminated after its own bytes, so a C string reader stops at the first NUL, and nothing overruns. */
    CHECK(isoline_error_message(error)[sizeof message] == '\0');
    isoline_error_delete(error);
}

ISOLINE_TEST_CASE(nullIsSafe)
{
    isoline_error_delete(NULL);
    CHECK(isoline_error_code(NULL) == 0);
    CHECK(isoline_error_detail(NULL) == 0);
    CHECK(isoline_error_message(NULL) != NULL);
    CHECK(isoline_error_message(NULL)[0] == '\0');
    CHECK(isoline_error_message_length(NULL) == 0);
    CHECK(isoline_error_cause(NULL) == NULL);
    {
        /* A NULL message: none for length 0, an INVALID_ARGUMENT error for any other. */
        isoline_error_t* const empty = isoline_error_new(ISOLINE_ERROR_EMBEDDER, NULL, 0);
        isoline_error_t* const refused = isoline_error_new(ISOLINE_ERROR_EMBEDDER, NULL, 3);
        REQUIRE(empty != NULL);
        REQUIRE(refused != NULL);
        CHECK(isoline_error_message_length(empty) == 0);
        CHECK(isoline_error_code(refused) == ISOLINE_ERROR_INVALID_ARGUMENT);
        isoline_error_delete(empty);
        isoline_error_delete(refused);
    }
}

ISOLINE_TEST_CASE(codeNames)
{
    CHECK(strcmp(isoline_error_code_name(ISOLINE_ERROR_DECODE), "decode") == 0);
    CHECK(strcmp(isoline_error_code_name(ISOLINE_ERROR_INVALID_ARGUMENT), "invalid_argument") == 0);
    CHECK(strcmp(isoline_error_code_name(ISOLINE_ERROR_LAST), "reentrant_call") == 0);
    CHECK(isoline_error_code_name(0) == NULL);
    CHECK(isoline_error_code_name(ISOLINE_ERROR_LAST + 1) == NULL);
}

int main(int argc, char** argv)
{
    return c_test_run_all(argc, argv);
}
