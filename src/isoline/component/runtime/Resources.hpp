// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/api/Extern.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/canonical/LiftLower.hpp>
#include <isoline/component/canonical/Trap.hpp>
#include <isoline/component/canonical/Values.hpp>
#include <isoline/component/runtime/HandleTable.hpp>
#include <isoline/component/types/TypeArena.hpp>

namespace isoline::component::runtime
{

using canonical::ResourceValue;

/// What destroys the resources of a host resource type: called once per own handle dropped, and once
/// per own handle a guest table still holds when its store is destroyed. The api layer re-exports it.
class IHostResourceDestructor
{
  public:
    virtual ~IHostResourceDestructor() = default;

    /// Destroys the resource @p rep represents. It must not throw, and runs no guest code.
    /// @param rep The resource's representation.
    virtual void destroy(uint32_t rep) noexcept = 0;
};

/// A host resource type's identity: one token shared by every copy of one api::HostResourceType, so
/// two imports a host defines from one type are one resource type (how `resource1-again` aliases
/// `resource1`). Compared by address, never by name. The api layer re-exports it.
struct HostResourceIdentity
{
    std::string name; ///< Its name, for messages only.
};

/// A resource type a guest's `(type (resource …))` defines, as one instantiation executed it.
struct GuestImplementor
{
    uint32_t componentInstance = 0; ///< The component instance that implements it.
    /// Its `dtor`, the core function that destroys one; none for a resource without a destructor.
    std::optional<isoline::api::Function> destructor {};
};

/// A resource type a host defines.
struct HostImplementor
{
    std::shared_ptr<HostResourceIdentity const> identity;   ///< Its identity token; never null.
    std::shared_ptr<IHostResourceDestructor> destructor {}; ///< What destroys one; null for nothing.
};

/// Who implements a resource type: a component instance or the host (definitions.py's ResourceType's
/// `impl` and `dtor`).
using ResourceImplementor = std::variant<GuestImplementor, HostImplementor>;

/// The runtime resource types of one ComponentStore (D5): one per execution of a resource definition,
/// so two instances of one component have two, never equal however alike; and one per host resource
/// type the store binds. Index 0 is no type, so a value-initialized ResourceTypeIndex names none.
class ResourceTypeTable
{
  public:
    /// An empty table.
    /// @param cap The most types it holds (ComponentLimits::maxResourceTypes).
    explicit ResourceTypeTable(uint32_t cap) noexcept: _cap { cap } {}

    /// Mints a fresh resource type: one per DefineResourceType executed.
    /// @param implementor Who implements it.
    /// @return Its index; or ResourceTypes when the table holds its cap already.
    /// @throws std::bad_alloc When it cannot be stored.
    [[nodiscard]] std::expected<ResourceTypeIndex, Trap> create(ResourceImplementor implementor);

    /// Who implements @p type.
    /// @param type A type of this table (checked).
    /// @return Its implementor.
    [[nodiscard]] ResourceImplementor const& operator[](ResourceTypeIndex type) const noexcept;

    /// Whether @p type is a type of this table: what to check of an index the host hands in.
    /// @param type Any index.
    /// @return Whether create() returned it.
    [[nodiscard]] bool contains(ResourceTypeIndex type) const noexcept
    {
        return type.value != 0 && type.value <= _types.size();
    }

    /// The types minted.
    /// @return Their count.
    [[nodiscard]] std::size_t size() const noexcept { return _types.size(); }

  private:
    uint32_t _cap;
    std::vector<ResourceImplementor> _types {};
};

/// Whether a component instance may call out of itself now (definitions.py's `may_leave`): cleared
/// while its `realloc` and `post-return` run.
enum class MayLeave : uint8_t
{
    No,  ///< It may not: a `canon lower`, `resource.new` or `resource.drop` traps CannotLeave.
    Yes, ///< It may.
};

/// What the canonical ABI keeps per component instance. Task 15 adds what its calls keep.
struct ComponentInstanceState
{
    /// A fresh instance: an empty table, free to leave.
    /// @param budget The store's handle budget.
    /// @param maxIndex The greatest handle index its table hands out.
    ComponentInstanceState(HandleBudget& budget, uint32_t maxIndex) noexcept: handles { budget, maxIndex } {}

    HandleTable handles;               ///< Its handle table.
    MayLeave mayLeave = MayLeave::Yes; ///< Whether it may leave.
};

/// Every component instance of a store, by instance number, each with its handle table. Instances are
/// never removed, and their states never move.
class InstanceTables
{
  public:
    /// No instance.
    /// @param budget The store's handle budget, which every table charges; it outlives this.
    /// @param maxIndex The greatest handle index a table hands out (HandleTable).
    explicit InstanceTables(HandleBudget& budget, uint32_t maxIndex = HandleTableMaxLength) noexcept:
        _budget { &budget }, _maxIndex { maxIndex }
    {
    }

    /// Adds an instance with an empty table. The caller counts instances (maxComponentInstances).
    /// @return Its number.
    /// @throws std::bad_alloc When it cannot be stored.
    [[nodiscard]] uint32_t add();

    /// The instance @p instance.
    /// @param instance A number add() returned (checked).
    /// @return Its state.
    [[nodiscard]] ComponentInstanceState& operator[](uint32_t instance) noexcept;

    /// The instances added.
    /// @return Their count.
    [[nodiscard]] std::size_t size() const noexcept { return _instances.size(); }

    /// The store's handle budget.
    /// @return It.
    [[nodiscard]] HandleBudget& budget() const noexcept { return *_budget; }

  private:
    HandleBudget* _budget;
    uint32_t _maxIndex;
    std::deque<ComponentInstanceState> _instances {};
};

/// A component call in progress, as it lends borrows (definitions.py's Task, synchronous part): a
/// borrow handle lowered into the callee counts here until the callee drops it.
struct Task
{
    /// Where the call is.
    enum class State : uint8_t
    {
        Initial,  ///< Started, not returned.
        Resolved, ///< Returned.
    };

    State state = State::Initial; ///< Where the call is.
    uint32_t borrowCount = 0;     ///< The borrow handles lent to it that the callee holds (num_borrows).
};

/// The tasks in progress of one store, innermost last: synchronous calls nest. A TaskId stays unique for
/// the store's life, so a borrow handle naming a task that ended finds none.
class TaskStack
{
  public:
    /// Starts a task: a component call entering its callee.
    /// @return Its id.
    /// @throws std::bad_alloc When it cannot be stored.
    [[nodiscard]] TaskId begin();

    /// The task @p id names, if in progress.
    /// @param id Any id.
    /// @return It; null when it ended or never was.
    [[nodiscard]] Task* find(TaskId id) noexcept;

    /// definitions.py's Task.return_: the callee returned.
    /// @param id The innermost task, not yet resolved (checked).
    /// @return Nothing; or BorrowOutstanding while a borrow lent to it is held, the task unchanged.
    /// @throws std::bad_alloc When the trap's message cannot be stored.
    [[nodiscard]] std::expected<void, Trap> resolve(TaskId id);

    /// Ends the innermost task, returned or trapped: a borrow handle that still names it is a borrow
    /// that outlived its call.
    /// @param id The innermost task (checked).
    void end(TaskId id) noexcept;

    /// The tasks in progress.
    /// @return Their count.
    [[nodiscard]] std::size_t size() const noexcept { return _tasks.size(); }

  private:
    std::vector<std::pair<TaskId, Task>> _tasks {}; ///< Innermost last, so ordered by id.
    uint64_t _next = 1;
};

/// A handle lent to a call in progress, by where it is: its instance and index. A lent handle cannot be
/// removed (LendOutstanding), so the pair names it until the call returns, however its table grows.
struct Lender
{
    uint32_t componentInstance = 0; ///< The instance whose table holds it.
    uint32_t handleIndex = 0;       ///< Its index there.

    /// Two lenders are equal when their members are.
    bool operator==(Lender const&) const = default;
};

/// A call a component instance makes out of itself (definitions.py's Subtask, synchronous part): the
/// borrows it lifts lend the caller's handles until the callee returns.
struct Subtask
{
    /// Where the call is.
    enum class State : uint8_t
    {
        Starting, ///< Lifting its arguments.
        Started,  ///< In the callee.
        Returned, ///< Returned; its lends are over.
    };

    State state = State::Starting;  ///< Where the call is.
    std::vector<Lender> lenders {}; ///< The handles it borrows.

    /// definitions.py's add_lender: lends the handle at @p lender to the call, charging the store's
    /// handle budget (one lend each, so no handle's lendCount overflows).
    /// @param instances The store's instances.
    /// @param lender A resource handle's place (checked: it holds one).
    /// @return Nothing; or Handles when the budget is at its limit, nothing lent.
    /// @throws std::bad_alloc When the lender cannot be stored; nothing lent.
    [[nodiscard]] std::expected<void, Trap> addLender(InstanceTables& instances, Lender lender);

    /// definitions.py's deliver_resolve: the callee returned; every lend ends, each handle re-found by its
    /// place, and the budget released.
    /// @param instances The store's instances.
    void deliverResolve(InstanceTables& instances) noexcept;
};

/// Which runtime resource type each resource of a side's arena stands for, in one component instance:
/// a resource its definitions created, or the one an import supplied.
class ResourceBindings
{
  public:
    /// Binds @p resource to @p type.
    /// @param resource A resource of the side's arena.
    /// @param type The runtime type it stands for.
    /// @throws std::bad_alloc When it cannot be stored.
    void bind(types::ResourceTypeId resource, ResourceTypeIndex type)
    {
        _types.insert_or_assign(resource, type);
    }

    /// The runtime type @p resource stands for.
    /// @param resource A bound resource (checked: validation binds every resource a function names).
    /// @return Its runtime type.
    [[nodiscard]] ResourceTypeIndex operator[](types::ResourceTypeId resource) const noexcept;

  private:
    std::map<types::ResourceTypeId, ResourceTypeIndex> _types {};
};

/// The resource state of one store: the handle budget, every instance's table, the runtime resource
/// types and the tasks in progress. Destroying it destroys every host resource a guest table still owns
/// (IHostResourceDestructor::destroy(), once each); no guest destructor runs.
class ResourceStore
{
  public:
    /// Empty state.
    /// @param limits Its caps: maxHandles and maxResourceTypes.
    /// @param maxIndex The greatest handle index a table hands out (HandleTable).
    explicit ResourceStore(ComponentLimits const& limits, uint32_t maxIndex = HandleTableMaxLength) noexcept:
        _budget { limits.maxHandles }, _instances { _budget, maxIndex }, _types { limits.maxResourceTypes }
    {
    }

    ResourceStore(ResourceStore const&) = delete;
    ResourceStore& operator=(ResourceStore const&) = delete;
    ResourceStore(ResourceStore&&) = delete;
    ResourceStore& operator=(ResourceStore&&) = delete;
    /// Destroys every host resource an own handle of a table holds, then the tables. Precondition
    /// (checked): no task is in progress.
    ~ResourceStore();

    /// Destroys every host resource an own handle of a table holds, removing the handle: a store's
    /// teardown, which runs host code only. A second call destroys nothing more. Precondition
    /// (checked): no task is in progress, so no call is running in the store.
    void destroyHostResources() noexcept;

    /// @return The handle budget.
    [[nodiscard]] HandleBudget& budget() noexcept { return _budget; }
    /// @return The instances.
    [[nodiscard]] InstanceTables& instances() noexcept { return _instances; }
    /// @return The runtime resource types.
    [[nodiscard]] ResourceTypeTable& types() noexcept { return _types; }
    /// @return The runtime resource types.
    [[nodiscard]] ResourceTypeTable const& types() const noexcept { return _types; }
    /// @return The tasks in progress.
    [[nodiscard]] TaskStack& tasks() noexcept { return _tasks; }

    /// Keeps owns no table holds any more for the store's end, which destroys each host resource among
    /// them once, as it destroys the ones a table still holds: what a failed lowering leaves of owns
    /// whose guest holder locked the store down (ComponentStore::undeliver()).
    /// @param owns The owns.
    /// @throws std::bad_alloc When they cannot be stored.
    void adoptOrphans(std::span<ResourceValue const> owns);

    /// The owns adoptOrphans() kept and teardown has not destroyed yet.
    [[nodiscard]] std::size_t orphanCount() const noexcept { return _orphans.size(); }

  private:
    HandleBudget _budget;
    InstanceTables _instances;
    ResourceTypeTable _types;
    TaskStack _tasks {};
    std::vector<ResourceValue> _orphans {}; ///< adoptOrphans(): destroyed at the store's end.
};

/// The trap of the handle at @p index being of @p found where @p expected is wanted, worded as the
/// component-model suite asserts it ("expected guest-defined resource but found a different
/// guest-defined resource").
/// @param types The store's resource types; both are of it.
/// @param index The handle index.
/// @param expected The type wanted.
/// @param found The handle's type.
/// @return WrongHandleType.
/// @throws std::bad_alloc When the message cannot be stored.
[[nodiscard]] Trap wrongHandleType(ResourceTypeTable const& types,
                                   uint32_t index,
                                   ResourceTypeIndex expected,
                                   ResourceTypeIndex found);

/// The handle at @p index of @p instance's table, if it is a resource handle of @p type: what
/// `resource.rep`, `resource.drop`, lift_own and lift_borrow check first.
/// @param resources The store's resource state.
/// @param instance The instance whose table it is.
/// @param type The runtime type expected.
/// @param index The guest's handle index.
/// @return The handle, for the operation at hand only; or UnknownHandle or WrongHandleType.
/// @throws std::bad_alloc When a trap's message cannot be stored.
[[nodiscard]] std::expected<std::reference_wrapper<ResourceHandle>, Trap> resourceHandleAt(
    ResourceStore& resources, uint32_t instance, ResourceTypeIndex type, uint32_t index);

/// The scope a side's borrows belong to (definitions.py's LiftLowerContext.borrow_scope): the Task of
/// the call a callee is entered by, where borrows are lowered; the Subtask of the call a caller makes,
/// where borrows are lifted; none where no borrow may pass (a `realloc`, a result).
using BorrowScope = std::variant<std::monostate, TaskId, Subtask*>;

/// The handle operations of one side of a canonical call, on its instance's table (definitions.py's
/// lift_own, lift_borrow, lower_own and lower_borrow). Every check precedes every change: a trap leaves
/// the table as it was (definitions.py removes before it checks, which nothing can observe once the
/// trap locked the store down, but which would free a lent handle's slot).
class ResourceOperations final: public canonical::HandleOps
{
  public:
    /// The operations of @p instance's side.
    /// @param resources The store's resource state.
    /// @param instance The side's component instance.
    /// @param bindings The runtime type each resource of the side's arena stands for in it.
    /// @param scope Where the side's borrows belong.
    ResourceOperations(ResourceStore& resources,
                       uint32_t instance,
                       ResourceBindings const& bindings,
                       BorrowScope scope) noexcept:
        _resources { &resources }, _instance { instance }, _bindings { &bindings }, _scope { scope }
    {
    }

    [[nodiscard]] std::expected<ResourceValue, Trap> liftOwn(types::ResourceTypeId type,
                                                             uint32_t index) override;
    /// Precondition (checked): the scope is a Subtask.
    [[nodiscard]] std::expected<ResourceValue, Trap> liftBorrow(types::ResourceTypeId type,
                                                                uint32_t index) override;
    /// @return As HandleOps::lowerOwn(); or WrongHandleType, naming the rep, when @p value is not of
    ///         the type @p type stands for.
    [[nodiscard]] std::expected<uint32_t, Trap> lowerOwn(types::ResourceTypeId type,
                                                         ResourceValue value) override;
    /// Precondition (checked): the scope is a TaskId of a task in progress.
    /// @return As HandleOps::lowerBorrow(); or WrongHandleType as lowerOwn() reports it.
    [[nodiscard]] std::expected<uint32_t, Trap> lowerBorrow(types::ResourceTypeId type,
                                                            ResourceValue value) override;

    /// Undoes a lowering that failed: removes every own handle lowerOwn() added to the side's table, so
    /// the owns stay with whoever passed them (nothing is destroyed). Only for a lowering no guest code
    /// has seen the result of: a `realloc` during it may not leave, so it changed no handle.
    void rollBackLoweredOwns() noexcept;

    /// The owns liftOwn() took out of the side's table so far, handed over once: what a lift that fails
    /// later leaves in no table (ComponentStore::undeliver() takes them back). A lift that succeeds hands
    /// them on in its values, and the list is dropped with the operations.
    /// @return The owns, in the order lifted.
    [[nodiscard]] std::vector<ResourceValue> takeLiftedOwns() noexcept
    {
        return std::exchange(_liftedOwns, {});
    }

  private:
    /// The handle table of the side's instance.
    [[nodiscard]] HandleTable& table() noexcept;

    /// Checks that @p value is of the runtime type @p type stands for.
    [[nodiscard]] std::expected<ResourceTypeIndex, Trap> checkLowered(types::ResourceTypeId type,
                                                                      ResourceValue value) const;

    ResourceStore* _resources;
    uint32_t _instance;
    ResourceBindings const* _bindings;
    BorrowScope _scope;
    std::vector<uint32_t> _loweredOwns {};     ///< The own handles lowerOwn() added, by index.
    std::vector<ResourceValue> _liftedOwns {}; ///< The owns liftOwn() took out of the table.
};

} // namespace isoline::component::runtime
