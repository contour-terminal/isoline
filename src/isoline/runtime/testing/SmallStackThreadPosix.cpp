// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <expected>
#include <functional>

#include <pthread.h>

#include <isoline/runtime/testing/SmallStackThread.hpp>

namespace isoline::runtime::testing
{

namespace
{
    /// The thread's entry: runs the work its argument points at.
    void* runWork(void* work)
    {
        (*static_cast<std::function<void()>*>(work))();
        return nullptr;
    }
} // namespace

std::expected<void, SmallStackError> runOnSmallStack(std::size_t stackBytes,
                                                     std::function<void()> const& work)
{
    auto attributes = pthread_attr_t {};
    if (pthread_attr_init(&attributes) != 0)
        return std::unexpected { SmallStackError::Unavailable };
    // The thread takes its argument as a void*: a copy of the work this frame owns, alive until the join.
    auto task = work;
    auto thread = pthread_t {};
    auto const created = pthread_attr_setstacksize(&attributes, stackBytes) == 0
                         && pthread_create(&thread, &attributes, &runWork, &task) == 0;
    pthread_attr_destroy(&attributes);
    if (!created)
        return std::unexpected { SmallStackError::Unavailable };
    pthread_join(thread, nullptr);
    return {};
}

} // namespace isoline::runtime::testing
