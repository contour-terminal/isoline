// SPDX-License-Identifier: Apache-2.0
//
// The C API calls back into its embedder through ISOLINE_CALL function pointers (export.h): host
// functions, finalizers. These hold the trampoline that makes those calls (detail/Callback.hpp) to the C
// ABI: a callback type declared the way the headers declare one is the trampoline's, and each argument
// arrives, and the result returns, as the C side wrote them.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <type_traits>
#include <utility>

#include <isoline/c/detail/Callback.hpp>
#include <isoline/c/export.h>
#include <isoline/c/testing/CallingConventionProbe.h>

namespace
{

/// What the callback below received.
struct Received
{
    void* context = nullptr;
    std::uint8_t kind = 0;
    bool isSet = false;
    std::uint32_t narrow = 0;
    std::uint64_t wide = 0;
    double real = 0.0;
    int calls = 0;
};

// A callback type as a C header declares one, the convention inside the declarator. (Language linkage is
// no part of it: compilers give extern "C" and C++ functions one function type.)
using ProbeCallback = std::uint64_t(ISOLINE_CALL*)(
    void* context, std::uint8_t kind, bool isSet, std::uint32_t narrow, std::uint64_t wide, double real);

// A callback as an embedder defines one: records its arguments and returns a mix of them.
std::uint64_t ISOLINE_CALL recordArguments(
    void* context, std::uint8_t kind, bool isSet, std::uint32_t narrow, std::uint64_t wide, double real)
{
    auto& received = *static_cast<Received*>(context);
    received = { .context = context,
                 .kind = kind,
                 .isSet = isSet,
                 .narrow = narrow,
                 .wide = wide,
                 .real = real,
                 .calls = received.calls + 1 };
    return wide ^ narrow;
}

// A finalizer as an embedder defines one: counts its calls on the context.
void ISOLINE_CALL countFinalization(void* context)
{
    ++*static_cast<int*>(context);
}

} // namespace

using isoline::c::detail::Callback;
using isoline::c::detail::CallbackFunction;
using isoline::c::detail::Finalizer;

TEST_CASE("CallingConvention.callbackRoundTrips", "[c-api]")
{
    using Probe = Callback<std::uint64_t, std::uint8_t, bool, std::uint32_t, std::uint64_t, double>;
    static_assert(std::is_same_v<Probe::Function, ProbeCallback>);
    static_assert(
        std::is_same_v<
            CallbackFunction<std::uint64_t, void*, std::uint8_t, bool, std::uint32_t, std::uint64_t, double>,
            ProbeCallback>);

    auto received = Received {};
    auto const callback = Probe { &recordArguments, &received };
    auto const wide = std::uint64_t { 0xF0E1'D2C3'B4A5'9687 };
    auto const narrow = std::uint32_t { 0x8000'0001 };
    auto const result = callback(std::uint8_t { 0xFE }, true, narrow, wide, -2.5);

    CHECK(result == (wide ^ narrow));
    CHECK(received.calls == 1);
    CHECK(received.context == &received);
    CHECK(received.kind == 0xFE);
    CHECK(received.isSet);
    CHECK(received.narrow == narrow);
    CHECK(received.wide == wide);
    CHECK(received.real == -2.5);
}

TEST_CASE("CallingConvention.callbackCompiledAsCRoundTrips", "[c-api]")
{
    // The same round trip, through a callback the C compiler compiled (testing/CallingConventionProbe.c):
    // what the C side defines with ISOLINE_CALL is what the trampoline calls with it.
    using Probe = Callback<std::uint64_t, std::uint8_t, bool, std::uint32_t, std::uint64_t, double>;
    static_assert(std::is_same_v<decltype(&callingConventionProbe), Probe::Function>);

    auto received = CallingConventionReceived {};
    auto const callback = Probe { &callingConventionProbe, &received };
    auto const wide = std::uint64_t { 0x0123'4567'89AB'CDEF };
    auto const narrow = std::uint32_t { 0xFFFF'FFFE };
    auto const result = callback(std::uint8_t { 0x81 }, true, narrow, wide, 1.0e300);

    CHECK(result == (wide ^ narrow));
    CHECK(received.calls == 1);
    CHECK(received.context == &received);
    CHECK(received.kind == 0x81);
    CHECK(received.isSet);
    CHECK(received.narrow == narrow);
    CHECK(received.wide == wide);
    CHECK(received.real == 1.0e300);
}

TEST_CASE("CallingConvention.finalizerRunsOnceWithItsContext", "[c-api]")
{
    static_assert(std::is_same_v<Finalizer::Function, void(ISOLINE_CALL*)(void*)>);

    auto calls = 0;
    {
        auto finalizer = Finalizer { &countFinalization, &calls };
        auto moved = std::move(finalizer);
        CHECK(calls == 0);
    }
    CHECK(calls == 1);

    // Assigning over a finalizer runs it, and the one assigned runs when its new holder ends.
    auto first = 0;
    auto second = 0;
    {
        auto holder = Finalizer { &countFinalization, &first };
        holder = Finalizer { &countFinalization, &second };
        CHECK(first == 1);
        CHECK(second == 0);
    }
    CHECK(first == 1);
    CHECK(second == 1);

    // Assigning a finalizer to itself keeps it, and it runs once.
    auto self = 0;
    {
        auto holder = Finalizer { &countFinalization, &self };
        auto& alias = holder;
        holder = std::move(alias);
        CHECK(self == 0);
    }
    CHECK(self == 1);

    // No finalizer, as wasm.h allows (a NULL finalizer parameter), is no call.
    {
        auto const none = Finalizer { nullptr, &calls };
    }
    CHECK(calls == 1);
}
