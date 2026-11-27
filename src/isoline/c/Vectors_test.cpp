// SPDX-License-Identifier: Apache-2.0
//
// What a vector owns (wasm.h's WASM_DECLARE_VEC families, detail/Vector.hpp): delete deletes its elements,
// copy deep-copies them, vec_new takes them (also when it refuses), and a copy that fails midway leaves
// nothing behind. The elements are counted test doubles (Counted), so every family of pointers is checked the
// same way, from its row of detail/VectorRows.def, before its element type has members of its own; ASan holds
// the counts to the memory.
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string_view>
#include <tuple>
#include <utility>

#include <isoline/c/detail/Handles.hpp>
#include <isoline/c/detail/Vector.hpp>
#include <isoline/c/isoline.h>

using isoline::c::detail::ElementOwnership;
using isoline::c::detail::Object;

namespace
{

/// The objects of one kind of test double alive, and the clones they may still make.
struct Census
{
    int live = 0;               ///< The doubles constructed and not yet destroyed.
    int clones = 0;             ///< The clones made.
    int clonesLeft = 1'000'000; ///< The clones made before one throws std::bad_alloc.
};

/// A wasm.h object of type @p Base that counts itself in a Census.
template <typename Base>
class Counted final: public Base
{
  public:
    explicit Counted(Census& census) noexcept: _census { &census } { ++_census->live; }
    Counted(Counted const& other) noexcept: Base(other), _census { other._census } { ++_census->live; }
    Counted(Counted&&) = delete;
    Counted& operator=(Counted const&) = delete;
    Counted& operator=(Counted&&) = delete;
    ~Counted() override { --_census->live; }

    [[nodiscard]] auto clone() const -> std::unique_ptr<Object> override
    {
        if (_census->clonesLeft == 0)
            throw std::bad_alloc {};
        --_census->clonesLeft;
        ++_census->clones;
        return std::make_unique<Counted>(*this);
    }

  private:
    Census* _census; ///< Where it counts itself.
};

/// One vector family of wasm.h, from its row: its types, its ownership and its five functions.
template <typename VectorType,
          ElementOwnership ElementRule,
          auto NewEmptyFunction,
          auto NewUninitializedFunction,
          auto NewFunction,
          auto CopyFunction,
          auto DeleteFunction>
struct Family
{
    using Vector = VectorType;
    using Element = isoline::c::detail::ElementOf<Vector>;
    static constexpr auto Ownership = ElementRule;
    static constexpr auto NewEmpty = NewEmptyFunction;
    static constexpr auto NewUninitialized = NewUninitializedFunction;
    static constexpr auto New = NewFunction;
    static constexpr auto Copy = CopyFunction;
    static constexpr auto Delete = DeleteFunction;
};

#define ISOLINE_VECTOR_FAMILY(name, ownership)   \
    Family<wasm_##name##_vec_t,                  \
           ElementOwnership::ownership,          \
           &wasm_##name##_vec_new_empty,         \
           &wasm_##name##_vec_new_uninitialized, \
           &wasm_##name##_vec_new,               \
           &wasm_##name##_vec_copy,              \
           &wasm_##name##_vec_delete>

// Every family, and the families of pointers, as type lists for the template cases below.
#define ISOLINE_VECTOR_ROW(name, ownership) \
    , std::declval<std::tuple<ISOLINE_VECTOR_FAMILY(name, ownership)>>()
using AllFamilies = decltype(std::tuple_cat(std::declval<std::tuple<>>()
#include <isoline/c/detail/VectorRows.def>
                                                ));
#undef ISOLINE_VECTOR_ROW

#define ISOLINE_VECTOR_ROW(name, ownership) ISOLINE_VECTOR_ROW_##ownership(name)
#define ISOLINE_VECTOR_ROW_Plain(name)
#define ISOLINE_VECTOR_ROW_Value(name)
#define ISOLINE_VECTOR_ROW_Owned(name) , std::declval<std::tuple<ISOLINE_VECTOR_FAMILY(name, Owned)>>()
using PointerFamilies = decltype(std::tuple_cat(std::declval<std::tuple<>>()
#include <isoline/c/detail/VectorRows.def>
                                                    ));
#undef ISOLINE_VECTOR_ROW
#undef ISOLINE_VECTOR_ROW_Plain
#undef ISOLINE_VECTOR_ROW_Value
#undef ISOLINE_VECTOR_ROW_Owned

/// The names of the rows the families above were made from: the lists cannot lose a row unnoticed.
constexpr auto RowNames = std::to_array<std::string_view>({
#define ISOLINE_VECTOR_ROW(name, ownership) std::string_view { #name },
#include <isoline/c/detail/VectorRows.def>
#undef ISOLINE_VECTOR_ROW
});
static_assert(std::tuple_size_v<AllFamilies> == RowNames.size());

/// A Counted element of the family @p F, as its vector holds it.
template <typename F>
[[nodiscard]] auto newElement(Census& census) -> F::Element
{
    return new Counted<std::remove_pointer_t<typename F::Element>>(census);
}

} // namespace

TEMPLATE_LIST_TEST_CASE("Vectors.everyRowIsAFamilyOfItsOwnership", "[c-api][vectors]", AllFamilies)
{
    // wasm.h's `*` (an own pointer) is exactly the Owned rows.
    STATIC_REQUIRE(std::is_pointer_v<typename TestType::Element>
                   == (TestType::Ownership == ElementOwnership::Owned));
    // The value row is wasm_val_t's, whose elements may hold references.
    STATIC_REQUIRE(std::is_same_v<typename TestType::Element, wasm_val_t>
                   == (TestType::Ownership == ElementOwnership::Value));
}

TEMPLATE_LIST_TEST_CASE("Vectors.emptyIsEmptyInEveryFamily", "[c-api][vectors]", AllFamilies)
{
    // new_empty, and vec_new of nothing, write the empty vector over whatever the caller's struct held.
    auto element = typename TestType::Element {};
    typename TestType::Vector vector { .size = 7, .data = &element };
    TestType::NewEmpty(&vector);
    CHECK(vector.size == 0);
    CHECK(vector.data == nullptr);
    vector = { .size = 7, .data = &element };
    TestType::New(&vector, 0, nullptr);
    CHECK(vector.size == 0);
    CHECK(vector.data == nullptr);
    TestType::Delete(&vector);
    CHECK(vector.size == 0);
}

TEMPLATE_LIST_TEST_CASE("Vectors.copyIsDeep", "[c-api][vectors]", PointerFamilies)
{
    auto census = Census {};
    auto const elements = std::array { newElement<TestType>(census),
                                       typename TestType::Element {},
                                       newElement<TestType>(census) };
    typename TestType::Vector original {};
    TestType::New(&original, elements.size(), elements.data());
    REQUIRE(original.size == 3);
    // vec_new takes the elements it is given: no copy is made.
    CHECK(census.clones == 0);
    CHECK(original.data[0] == elements[0]);

    typename TestType::Vector copy {};
    TestType::Copy(&copy, &original);
    REQUIRE(copy.size == 3);
    CHECK(census.clones == 2);
    CHECK(census.live == 4);
    CHECK(copy.data[0] != original.data[0]);
    // A slot not yet filled is copied as one.
    CHECK(copy.data[1] == nullptr);

    // The copy survives its original, whose delete deletes the original's elements and only those.
    TestType::Delete(&original);
    CHECK(original.size == 0);
    CHECK(original.data == nullptr);
    CHECK(census.live == 2);
    TestType::Delete(&copy);
    CHECK(census.live == 0);
}

TEMPLATE_LIST_TEST_CASE("Vectors.copyFailingMidwayLeavesNothing", "[c-api][vectors]", PointerFamilies)
{
    auto census = Census {};
    auto const elements = std::array { newElement<TestType>(census),
                                       newElement<TestType>(census),
                                       newElement<TestType>(census) };
    typename TestType::Vector original {};
    TestType::New(&original, elements.size(), elements.data());

    // The second clone fails: the first is deleted again, the copy is the empty vector, and the original
    // is as it was.
    census.clonesLeft = 1;
    typename TestType::Vector copy { .size = 7, .data = nullptr };
    TestType::Copy(&copy, &original);
    CHECK(copy.size == 0);
    CHECK(copy.data == nullptr);
    CHECK(census.clones == 1);
    CHECK(census.live == 3);
    CHECK(original.size == 3);

    TestType::Delete(&original);
    CHECK(census.live == 0);
}

TEMPLATE_LIST_TEST_CASE("Vectors.refusedNewTakesItsElements", "[c-api][vectors]", PointerFamilies)
{
    auto census = Census {};

    // No vector to make: the elements are taken all the same, and deleted, those after a null one too.
    auto const taken = std::array { newElement<TestType>(census),
                                    typename TestType::Element {},
                                    newElement<TestType>(census) };
    TestType::New(nullptr, taken.size(), taken.data());
    CHECK(census.live == 0);

    // A size no vector has cannot be walked: those elements are left untouched, and the caller still owns
    // them.
    auto const element = newElement<TestType>(census);
    auto const kept = std::array { element };
    typename TestType::Vector vector {};
    TestType::New(&vector, isoline::c::detail::MaxVectorSize<typename TestType::Element> + 1, kept.data());
    CHECK(vector.size == 0);
    CHECK(vector.data == nullptr);
    CHECK(census.live == 1);
    delete element;
    CHECK(census.live == 0);
}

TEMPLATE_LIST_TEST_CASE("Vectors.copyOntoItselfIsACopy", "[c-api][vectors]", AllFamilies)
{
    // wasm_<name>_vec_copy(&v, &v): the copy replaces the vector it was made from, whose data the caller
    // still owns through the pointer it read before.
    typename TestType::Vector vector {};
    TestType::NewUninitialized(&vector, 2);
    auto before = vector;
    TestType::Copy(&vector, &vector);
    CHECK(vector.size == 2);
    CHECK(vector.data != before.data);
    TestType::Delete(&before);
    TestType::Delete(&vector);
}

TEST_CASE("Vectors.valuesOwnTheirReferences", "[c-api][vectors]")
{
    auto census = Census {};
    auto values = std::array<wasm_val_t, 4> {};
    values[0].kind = WASM_EXTERNREF;
    values[0].of.ref = new Counted<wasm_ref_t>(census);
    values[1].kind = WASM_FUNCREF;
    values[1].of.ref = nullptr;
    // A number's bits are no pointer, nor are those of a kind no row knows.
    values[2].kind = WASM_I64;
    values[2].of.i64 = 0x1234;
    values[3].kind = 77;
    values[3].of.i64 = 0x5678;

    wasm_val_vec_t original {};
    wasm_val_vec_new(&original, values.size(), values.data());
    REQUIRE(original.size == 4);
    CHECK(census.clones == 0);

    wasm_val_vec_t copy {};
    wasm_val_vec_copy(&copy, &original);
    REQUIRE(copy.size == 4);
    CHECK(census.clones == 1);
    CHECK(copy.data[0].of.ref != original.data[0].of.ref);
    CHECK(copy.data[1].of.ref == nullptr);
    CHECK(copy.data[2].of.i64 == 0x1234);
    CHECK(copy.data[3].kind == 77);
    CHECK(copy.data[3].of.i64 == 0x5678);

    wasm_val_vec_delete(&original);
    CHECK(census.live == 1);
    wasm_val_vec_delete(&copy);
    CHECK(census.live == 0);
}
