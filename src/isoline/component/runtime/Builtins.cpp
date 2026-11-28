// SPDX-License-Identifier: Apache-2.0
#include <isoline/component/runtime/Builtins.hpp>

#include <core/Assert.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <variant>

#include <isoline/api/Types.hpp>
#include <isoline/component/canonical/Trap.hpp>
#include <isoline/component/runtime/HandleTable.hpp>
#include <isoline/component/runtime/Resources.hpp>
#include <isoline/meta/Check.hpp>

namespace isoline::component::runtime
{

using canonical::TrapCode;
using canonical::trapOf;

namespace
{

    /// A resource built-in's operation: its one i32 argument in, its i32 result (if it has one) out.
    using ResourceOperation = std::expected<std::optional<uint32_t>, Trap> (*)(IBuiltinHost& host,
                                                                               uint32_t instance,
                                                                               ResourceTypeIndex type,
                                                                               uint32_t argument);

    /// The core function of a resource built-in: reads its i32, runs the operation, and writes its result;
    /// a trap is raised into the store's carrier and crosses the core frames as carrierTrap().
    class ResourceBuiltin final: public isoline::api::IHostFunction
    {
      public:
        ResourceBuiltin(BuiltinBinding const& binding, ResourceOperation operation) noexcept:
            _host { &binding.host },
            _instance { binding.instance },
            _type { binding.type },
            _operation { operation }
        {
        }

        [[nodiscard]] std::expected<void, isoline::api::Abrupt> call(
            isoline::api::HostContext& /*context*/,
            std::span<isoline::api::Value const> args,
            std::span<isoline::api::Value> results) noexcept override
        {
            // The store checked the arguments against the function's type: one i32.
            ISOLINE_PRECONDITION(args.size() == 1);
            auto const* const argument = std::get_if<isoline::api::I32>(args.data());
            ISOLINE_PRECONDITION(argument != nullptr);
            auto outcome = _operation(*_host, _instance, _type, argument->bits);
            if (!outcome)
            {
                _host->pendingTrap().raise(std::move(outcome.error()));
                return std::unexpected { isoline::api::Abrupt { canonical::carrierTrap() } };
            }
            auto const produced = outcome->has_value();
            ISOLINE_PRECONDITION(results.size() == (produced ? 1U : 0U));
            if (*outcome)
                results[0] = isoline::api::I32 { .bits = **outcome };
            return {};
        }

      private:
        IBuiltinHost* _host;
        uint32_t _instance;
        ResourceTypeIndex _type;
        ResourceOperation _operation;
    };

    /// The core i32 type.
    constexpr auto CoreI32 = isoline::api::ValueType { .kind = isoline::api::ValueKind::I32 };

} // namespace

std::expected<uint32_t, Trap> resourceNew(IBuiltinHost& host,
                                          uint32_t instance,
                                          ResourceTypeIndex type,
                                          uint32_t rep)
{
    auto& state = host.resources().instances()[instance];
    if (state.mayLeave == MayLeave::No)
        return std::unexpected { trapOf(TrapCode::CannotLeave) };
    return state.handles.add(ResourceHandle { .type = type, .rep = rep, .ownership = Ownership::Own });
}

std::expected<void, Trap> resourceDrop(IBuiltinHost& host,
                                       uint32_t instance,
                                       ResourceTypeIndex type,
                                       uint32_t index)
{
    auto& resources = host.resources();
    if (resources.instances()[instance].mayLeave == MayLeave::No)
        return std::unexpected { trapOf(TrapCode::CannotLeave) };
    auto found = resourceHandleAt(resources, instance, type, index);
    if (!found)
        return std::unexpected { std::move(found.error()) };
    auto const handle = found->get();
    if (handle.lendCount != 0)
        return std::unexpected { trapOf(TrapCode::LendOutstanding) };
    if (handle.ownership == Ownership::Borrow)
    {
        // A borrow whose call ended outlived it: that call trapped, and the store is locked down.
        auto* const task = resources.tasks().find(handle.borrowScope);
        if (task == nullptr)
            return std::unexpected { trapOf(TrapCode::BorrowOutstanding) };
        auto removed = resources.instances()[instance].handles.remove(index);
        Require(removed.has_value());
        --task->borrowCount;
        return {};
    }
    auto removed = resources.instances()[instance].handles.remove(index);
    Require(removed.has_value());
    // The handle is gone before the destructor runs: one that re-enters finds its slot free, and
    // holds no reference into a table it may grow.
    // Copied, not referenced: the destructor's guest code may create resource types, and the table
    // holding the implementor may grow under it.
    auto const implementor = resources.types()[type];
    if (auto const* const hosted = std::get_if<HostImplementor>(&implementor))
    {
        if (hosted->destructor)
            hosted->destructor->destroy(handle.rep);
        return {};
    }
    auto const& guest = std::get<GuestImplementor>(implementor);
    if (!guest.destructor)
        return {};
    return host.callGuestDestructor(guest, instance, handle.rep);
}

std::expected<uint32_t, Trap> resourceRep(IBuiltinHost& host,
                                          uint32_t instance,
                                          ResourceTypeIndex type,
                                          uint32_t index)
{
    return resourceHandleAt(host.resources(), instance, type, index)
        .transform([](ResourceHandle const& handle) { return handle.rep; });
}

BuiltinFunction makeResourceNew(BuiltinBinding const& binding)
{
    return BuiltinFunction {
        .type = isoline::api::FuncType { .params = { CoreI32 }, .results = { CoreI32 } },
        .function = std::make_shared<ResourceBuiltin>(
            binding,
            [](IBuiltinHost& host, uint32_t instance, ResourceTypeIndex type, uint32_t rep)
                -> std::expected<std::optional<uint32_t>, Trap> {
                return resourceNew(host, instance, type, rep);
            }),
    };
}

BuiltinFunction makeResourceDrop(BuiltinBinding const& binding)
{
    return BuiltinFunction {
        .type = isoline::api::FuncType { .params = { CoreI32 }, .results = {} },
        .function = std::make_shared<ResourceBuiltin>(
            binding,
            [](IBuiltinHost& host, uint32_t instance, ResourceTypeIndex type, uint32_t index)
                -> std::expected<std::optional<uint32_t>, Trap> {
                return resourceDrop(host, instance, type, index).transform([] {
                    return std::optional<uint32_t> {};
                });
            }),
    };
}

BuiltinFunction makeResourceRep(BuiltinBinding const& binding)
{
    return BuiltinFunction {
        .type = isoline::api::FuncType { .params = { CoreI32 }, .results = { CoreI32 } },
        .function = std::make_shared<ResourceBuiltin>(
            binding,
            [](IBuiltinHost& host, uint32_t instance, ResourceTypeIndex type, uint32_t index)
                -> std::expected<std::optional<uint32_t>, Trap> {
                return resourceRep(host, instance, type, index);
            }),
    };
}

BuiltinHandlerRow const& builtinHandlerRow(binary::CanonBuiltin builtin) noexcept
{
    ISOLINE_PRECONDITION(builtin >= FirstHandledBuiltin);
    return BuiltinHandlerTable[static_cast<std::size_t>(builtin)
                               - static_cast<std::size_t>(FirstHandledBuiltin)];
}

} // namespace isoline::component::runtime
