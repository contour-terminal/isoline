// SPDX-License-Identifier: Apache-2.0
#include <isoline/platform/JitMemory.hpp>

#include <thread>

#include <isoline/meta/Check.hpp>
#include <isoline/platform/Platform.hpp>

namespace isoline::platform
{

namespace detail
{

    void requireJitMapping(CodeMemoryKind kind, JitMapping mapping) noexcept
    {
        ISOLINE_PRECONDITION(allowsJitMapping(kind, mapping));
    }

    void requireJitThreadToggle(CodeMemoryKind kind) noexcept
    {
        ISOLINE_PRECONDITION(togglesJitPerThread(kind));
    }

} // namespace detail

JitWriteScope::JitWriteScope(IJitMemory& memory, Key /*key*/) noexcept:
    _memory { memory }, _owner { std::this_thread::get_id() }
{
    _memory.setThreadAccess(JitAccess::Write);
}

JitWriteScope::~JitWriteScope()
{
    ISOLINE_INVARIANT(std::this_thread::get_id() == _owner);
    _memory.setThreadAccess(JitAccess::Execute);
}

} // namespace isoline::platform
