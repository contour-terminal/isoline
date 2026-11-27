/* SPDX-License-Identifier: Apache-2.0 */
#include "CallingConventionProbe.h"

uint64_t ISOLINE_CALL
callingConventionProbe(void* context, uint8_t kind, bool isSet, uint32_t narrow, uint64_t wide, double real)
{
    struct CallingConventionReceived* const received = (struct CallingConventionReceived*) context;
    received->context = context;
    received->kind = kind;
    received->isSet = isSet;
    received->narrow = narrow;
    received->wide = wide;
    received->real = real;
    ++received->calls;
    return wide ^ narrow;
}
