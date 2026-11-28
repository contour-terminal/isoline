// SPDX-License-Identifier: Apache-2.0
#include "ComponentSpectest.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/api/Engine.hpp>
#include <isoline/api/Module.hpp>
#include <isoline/api/Types.hpp>
#include <isoline/component/api/ComponentStore.hpp>
#include <isoline/component/api/Linker.hpp>
#include <isoline/component/api/Typed.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>

namespace isoline::spectest
{

namespace
{
    using component::api::HostComponentContext;
    using component::api::HostResourceType;
    using component::api::IHostResourceDestructor;
    using component::api::Linker;
    using component::api::typed::Borrow;
    using component::api::typed::Own;
    using component::api::typed::ResourceTypes;
    using isoline::api::HostFailure;
    using Outcome = std::expected<void, std::string>;

    /// The C++ tag of the host resource type `resource1` (and `resource1-again`).
    struct Resource1
    {
    };

    /// The C++ tag of the host resource type `resource2`.
    struct Resource2
    {
    };

    /// How often a resource1 was destroyed, and the representation of the last one.
    struct DropCounts
    {
        std::atomic<uint32_t> drops { 0 };    ///< The destroyed resource1s.
        std::atomic<uint32_t> lastDrop { 0 }; ///< The representation of the last one; 0 before any.
    };

    /// resource1's destructor: counts the drop and keeps its representation.
    class CountingDestructor final: public IHostResourceDestructor
    {
      public:
        explicit CountingDestructor(std::shared_ptr<DropCounts> counts) noexcept:
            _counts { std::move(counts) }
        {
        }

        void destroy(uint32_t rep) noexcept override
        {
            _counts->drops.fetch_add(1);
            _counts->lastDrop.store(rep);
        }

      private:
        std::shared_ptr<DropCounts> _counts;
    };

    /// The failure of a spectest assertion that does not hold, as Wasmtime's panics in it trap.
    [[nodiscard]] HostFailure assertionFailure() noexcept
    {
        return HostFailure { .kind = isoline::api::HostFailureKind::Embedder,
                             .code = 1,
                             .mismatch = std::nullopt };
    }

    /// What a row's definition reads and writes.
    struct Definitions
    {
        isoline::api::Engine const& engine;                   ///< The engine `simple-module` is loaded on.
        Linker& linker;                                       ///< The linker defined into.
        ResourceTypes<Resource1, Resource2> const& resources; ///< resource1 and resource2.
        std::shared_ptr<DropCounts> counts;                   ///< resource1's drops.
    };

    /// Defines one row at @p path.
    using Define = Outcome (*)(Definitions& definitions, std::string path);

    [[nodiscard]] Outcome linked(std::expected<void, component::api::LinkerError> const& defined)
    {
        return defined.transform_error(
            [](component::api::LinkerError const& error) { return error.message(); });
    }

    /// Defines a host function of @p Signature, its parameters labelled @p names, at @p path.
    template <typename Signature, typename F>
    [[nodiscard]] Outcome defineTyped(Definitions& definitions,
                                      std::string path,
                                      std::span<std::string_view const> names,
                                      F body)
    {
        auto const spec =
            component::api::typed::functionType<Signature>(definitions.linker, definitions.resources, names);
        if (!spec)
            return std::unexpected { spec.error().message() };
        return linked(definitions.linker.defineFunction(
            std::move(path),
            *spec,
            component::api::typed::makeTypedHostFunction<Signature>(std::move(body))));
    }

    /// A host function of no parameters returning @p Value.
    template <uint32_t Value>
    [[nodiscard]] Outcome defineConstant(Definitions& definitions, std::string path)
    {
        return defineTyped<uint32_t()>(definitions, std::move(path), {}, []() noexcept { return Value; });
    }

    /// `simple-module`: a core module exporting the global `g` (100) and the function `f` (101).
    [[nodiscard]] Outcome defineSimpleModule(Definitions& definitions, std::string path)
    {
        constexpr auto Source = std::string_view {
            R"((module (global (export "g") i32 i32.const 100) (func (export "f") (result i32) i32.const 101)))"
        };
        auto const bytes = text::compileText(Source, text::TextLimits {}, text::CoreForms);
        if (!bytes)
            return std::unexpected { std::string { "simple-module does not compile" } };
        auto const module = definitions.engine.load(*bytes);
        if (!module)
            return std::unexpected { std::string { "simple-module does not load" } };
        return linked(definitions.linker.defineModule(std::move(path), *module));
    }

    /// A host resource type, the one of @p R.
    template <typename R>
    [[nodiscard]] Outcome defineResource(Definitions& definitions, std::string path)
    {
        return linked(
            definitions.linker.defineResource(std::move(path), definitions.resources.template get<R>()));
    }

    constexpr auto SelfRep = std::to_array<std::string_view>({ "self", "rep" });
    constexpr auto SelfOther = std::to_array<std::string_view>({ "self", "b" });
    constexpr auto ResourceRep = std::to_array<std::string_view>({ "r", "rep" });
    constexpr auto Rep = std::to_array<std::string_view>({ "r" });

    /// [constructor]resource1: a resource1 the host holds, of the representation given.
    [[nodiscard]] Outcome defineConstructor(Definitions& definitions, std::string path)
    {
        return defineTyped<Own<Resource1>(uint32_t)>(
            definitions,
            std::move(path),
            Rep,
            [resources = definitions.resources](HostComponentContext& context, uint32_t rep) noexcept
                -> std::expected<Own<Resource1>, HostFailure> {
                auto made = component::api::typed::hostOwn<Resource1>(context.store(), resources, rep);
                if (!made)
                    return std::unexpected { assertionFailure() };
                return std::move(*made);
            });
    }

    /// [static]resource1.assert: that the own given is of the representation given.
    [[nodiscard]] Outcome defineAssert(Definitions& definitions, std::string path)
    {
        // The own is forgotten, as Wasmtime's host drops its Resource without destroying it.
        return defineTyped<void(Own<Resource1>, uint32_t)>(
            definitions,
            std::move(path),
            ResourceRep,
            [](Own<Resource1> const& resource, uint32_t rep) noexcept -> std::expected<void, HostFailure> {
                if (resource.rep() != rep)
                    return std::unexpected { assertionFailure() };
                return {};
            });
    }

    /// [static]resource1.last-drop: the representation of the resource1 destroyed last.
    [[nodiscard]] Outcome defineLastDrop(Definitions& definitions, std::string path)
    {
        return defineTyped<uint32_t()>(
            definitions, std::move(path), {}, [counts = definitions.counts]() noexcept {
                return counts->lastDrop.load();
            });
    }

    /// [static]resource1.drops: how many resource1s were destroyed.
    [[nodiscard]] Outcome defineDrops(Definitions& definitions, std::string path)
    {
        return defineTyped<uint32_t()>(
            definitions, std::move(path), {}, [counts = definitions.counts]() noexcept {
                return counts->drops.load();
            });
    }

    /// [method]resource1.simple: that the borrow given is of the representation given.
    [[nodiscard]] Outcome defineSimple(Definitions& definitions, std::string path)
    {
        return defineTyped<void(Borrow<Resource1>, uint32_t)>(
            definitions,
            std::move(path),
            SelfRep,
            [](Borrow<Resource1> self, uint32_t rep) noexcept -> std::expected<void, HostFailure> {
                if (self.rep() != rep)
                    return std::unexpected { assertionFailure() };
                return {};
            });
    }

    /// [method]resource1.take-borrow: takes two borrows.
    [[nodiscard]] Outcome defineTakeBorrow(Definitions& definitions, std::string path)
    {
        return defineTyped<void(Borrow<Resource1>, Borrow<Resource1>)>(
            definitions, std::move(path), SelfOther, [](Borrow<Resource1>, Borrow<Resource1>) noexcept {});
    }

    /// [method]resource1.take-own: takes a borrow and an own.
    [[nodiscard]] Outcome defineTakeOwn(Definitions& definitions, std::string path)
    {
        // As [static]resource1.assert, the own is forgotten.
        return defineTyped<void(Borrow<Resource1>, Own<Resource1>)>(
            definitions, std::move(path), SelfOther, [](Borrow<Resource1>, Own<Resource1> const&) noexcept {
            });
    }

    /// return-hi: the string "hi".
    [[nodiscard]] Outcome defineReturnHi(Definitions& definitions, std::string path)
    {
        return defineTyped<std::string()>(
            definitions, std::move(path), {}, []() noexcept { return std::string { "hi" }; });
    }

    /// What defines each row of ComponentSpectestRows, at the same index; none for an async row.
    constexpr auto Defines = std::array<Define, ComponentSpectestRows.size()> {
        nullptr, // host-echo-u32
        &defineConstant<2>,
        &defineConstant<3>,
        &defineConstant<4>,
        &defineSimpleModule,
        &defineResource<Resource1>,
        &defineResource<Resource2>,
        &defineResource<Resource1>, // resource1-again: resource1's type, so one resource type
        &defineConstructor,
        &defineAssert,
        &defineLastDrop,
        &defineDrops,
        &defineSimple,
        &defineTakeBorrow,
        &defineTakeOwn,
        nullptr, // never-return
        nullptr, // return-two-slowly
        nullptr, // echo-slowly
        nullptr, // [method]resource1.never-return
        &defineReturnHi,
        nullptr, // unit-future
    };

    static_assert(Defines.size() == ComponentSpectestRows.size(),
                  "Defines holds one definition per ComponentSpectestRows row");
    static_assert(
        [] {
            for (auto const index: std::views::iota(std::size_t { 0 }, Defines.size()))
                if ((Defines.at(index) == nullptr)
                    != (ComponentSpectestRows.at(index).kind == SpectestRowKind::Async))
                    return false;
            return true;
        }(),
        "exactly the async rows of ComponentSpectestRows are left undefined");
} // namespace

std::expected<Linker, std::string> makeComponentSpectest(isoline::api::Engine const& engine,
                                                         component::api::ComponentConfig const& config)
{
    auto linker = Linker { config };
    auto const counts = std::make_shared<DropCounts>();
    auto const resources = ResourceTypes<Resource1, Resource2> {
        HostResourceType { "resource1", std::make_shared<CountingDestructor>(counts) },
        HostResourceType { "resource2", nullptr },
    };
    auto definitions =
        Definitions { .engine = engine, .linker = linker, .resources = resources, .counts = counts };
    for (auto const index: std::views::iota(std::size_t { 0 }, Defines.size()))
    {
        auto const define = Defines.at(index);
        if (define == nullptr)
            continue;
        auto const& row = ComponentSpectestRows.at(index);
        if (auto const defined = define(definitions, std::string { row.path }); !defined)
            return std::unexpected { std::format("spectest {}: {}", row.path, defined.error()) };
    }
    return linker;
}

} // namespace isoline::spectest
