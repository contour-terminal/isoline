// SPDX-License-Identifier: Apache-2.0

#include "Wasi02Subset.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace isoline::component::testing
{

namespace
{

    using api::typed::Borrow;
    using api::typed::HostResourceTable;

    /// The HostFailure a body of the double fails its call with.
    [[nodiscard]] isoline::api::HostFailure failureOf(Wasi02Failure failure) noexcept
    {
        return isoline::api::HostFailure { .kind = isoline::api::HostFailureKind::Embedder,
                                           .code = static_cast<uint32_t>(failure),
                                           .mismatch = std::nullopt };
    }

    /// Destroys a stream: erases its object from the double's table.
    template <typename T>
    class TableDestructor final: public api::IHostResourceDestructor
    {
      public:
        explicit TableDestructor(HostResourceTable<T>& table) noexcept: _table { &table } {}

        void destroy(uint32_t rep) noexcept override { _table->erase(rep); }

      private:
        HostResourceTable<T>* _table;
    };

    /// What get-type and stat answer for a descriptor: the double hands none out, so none is valid.
    template <typename T>
    [[nodiscard]] std::expected<T, ErrorCode> badDescriptor() noexcept
    {
        return std::unexpected { ErrorCode::BadDescriptor };
    }

    constexpr auto Self = std::to_array<std::string_view>({ "self" });
    constexpr auto SelfContents = std::to_array<std::string_view>({ "self", "contents" });
    constexpr auto SelfOffset = std::to_array<std::string_view>({ "self", "offset" });
    constexpr auto Status = std::to_array<std::string_view>({ "status" });
    constexpr auto Err = std::to_array<std::string_view>({ "err" });

    constexpr auto NanosecondsPerSecond = uint64_t { 1'000'000'000 };

} // namespace

Wasi02Subset::Wasi02Subset(FakeClock const& clock, Wasi02Options options):
    _resources { api::HostResourceType { "error", nullptr },
                 api::HostResourceType { "output-stream",
                                         std::make_shared<TableDestructor<OutputTarget>>(_outputs) },
                 api::HostResourceType { "input-stream", std::make_shared<TableDestructor<Unit>>(_inputs) },
                 api::HostResourceType { "descriptor", nullptr } },
    _clock { &clock },
    _options { std::move(options) }
{
}

std::span<Wasi02Subset::Row const> Wasi02Subset::rows() noexcept
{
    // Every definition, grouped by interface in the order clock.wasm imports them; the resources of an
    // interface before its functions, as the linker needs a resource defined before a function names it.
    static constexpr auto Rows = std::to_array<Row>({
        { { "wasi:cli/environment@0.2.12", "get-environment" }, &Wasi02Subset::defineGetEnvironment },
        { { "wasi:cli/exit@0.2.12", "exit" }, &Wasi02Subset::defineExit },
        { { "wasi:io/error@0.2.12", "error" }, &Wasi02Subset::defineError },
        { { "wasi:io/streams@0.2.12", "output-stream" }, &Wasi02Subset::defineOutputStream },
        { { "wasi:io/streams@0.2.12", "input-stream" }, &Wasi02Subset::defineInputStream },
        { { "wasi:io/streams@0.2.12", "[method]output-stream.check-write" },
          &Wasi02Subset::defineCheckWrite },
        { { "wasi:io/streams@0.2.12", "[method]output-stream.write" }, &Wasi02Subset::defineWrite },
        { { "wasi:io/streams@0.2.12", "[method]output-stream.blocking-write-and-flush" },
          &Wasi02Subset::defineWrite },
        { { "wasi:io/streams@0.2.12", "[method]output-stream.blocking-flush" },
          &Wasi02Subset::defineBlockingFlush },
        { { "wasi:cli/stdin@0.2.12", "get-stdin" }, &Wasi02Subset::defineGetStdin },
        { { "wasi:cli/stdout@0.2.12", "get-stdout" }, &Wasi02Subset::defineGetStdout },
        { { "wasi:cli/stderr@0.2.12", "get-stderr" }, &Wasi02Subset::defineGetStderr },
        { { "wasi:clocks/monotonic-clock@0.2.12", "now" }, &Wasi02Subset::defineMonotonicNow },
        { { "wasi:clocks/wall-clock@0.2.12", "now" }, &Wasi02Subset::defineWallClockNow },
        { { "wasi:filesystem/types@0.2.12", "descriptor" }, &Wasi02Subset::defineDescriptor },
        { { "wasi:filesystem/types@0.2.12", "[method]descriptor.write-via-stream" },
          &Wasi02Subset::defineWriteViaStream },
        { { "wasi:filesystem/types@0.2.12", "[method]descriptor.append-via-stream" },
          &Wasi02Subset::defineAppendViaStream },
        { { "wasi:filesystem/types@0.2.12", "[method]descriptor.get-type" }, &Wasi02Subset::defineGetType },
        { { "wasi:filesystem/types@0.2.12", "[method]descriptor.stat" }, &Wasi02Subset::defineStat },
        { { "wasi:filesystem/types@0.2.12", "filesystem-error-code" },
          &Wasi02Subset::defineFilesystemErrorCode },
        { { "wasi:filesystem/preopens@0.2.12", "get-directories" }, &Wasi02Subset::defineGetDirectories },
    });
    return Rows;
}

std::expected<std::unique_ptr<Wasi02Subset>, api::LinkerError> Wasi02Subset::define(api::Linker& linker,
                                                                                    FakeClock const& clock,
                                                                                    Wasi02Options options)
{
    // The constructor is private, so make_unique cannot reach it.
    auto subset = std::unique_ptr<Wasi02Subset> { new Wasi02Subset { clock, std::move(options) } };
    for (auto const& row: rows())
    {
        auto const path = std::format("{}{}{}", row.binding.interface, api::PathSeparator, row.binding.name);
        if (auto defined = (subset.get()->*row.define)(linker, path); !defined)
            return std::unexpected { std::move(defined.error()) };
    }
    return subset;
}

std::vector<Wasi02Binding> Wasi02Subset::bindings()
{
    auto all = std::vector<Wasi02Binding> {};
    for (auto const& row: rows())
        all.push_back(row.binding);
    return all;
}

std::vector<std::string_view> Wasi02Subset::interfaces()
{
    auto names = std::vector<std::string_view> {};
    for (auto const& row: rows())
        if (std::ranges::find(names, row.binding.interface) == names.end())
            names.push_back(row.binding.interface);
    return names;
}

template <typename Signature, typename Body>
std::expected<void, api::LinkerError> Wasi02Subset::defineTyped(api::Linker& linker,
                                                                std::string const& path,
                                                                std::span<std::string_view const> labels,
                                                                Body body)
{
    auto spec = api::typed::functionType<Signature>(linker, _resources, labels);
    if (!spec)
        return std::unexpected { std::move(spec.error()) };
    return linker.defineFunction(path, *spec, api::typed::makeTypedHostFunction<Signature>(std::move(body)));
}

template <typename Tag, typename T>
std::expected<Own<Tag>, isoline::api::HostFailure> Wasi02Subset::openStream(
    api::HostComponentContext& context, HostResourceTable<T>& table, T object)
{
    auto const rep = table.insert(std::move(object));
    if (!rep)
        return std::unexpected { failureOf(Wasi02Failure::TableFull) };
    auto own = api::typed::hostOwn<Tag>(context.store(), _resources, *rep);
    if (!own)
    {
        table.erase(*rep);
        return std::unexpected { failureOf(Wasi02Failure::OwnRefused) };
    }
    return std::move(*own);
}

std::expected<void, api::LinkerError> Wasi02Subset::defineError(api::Linker& linker, std::string const& path)
{
    return linker.defineResource(path, _resources.get<Error>());
}

std::expected<void, api::LinkerError> Wasi02Subset::defineOutputStream(api::Linker& linker,
                                                                       std::string const& path)
{
    return linker.defineResource(path, _resources.get<OutputStream>());
}

std::expected<void, api::LinkerError> Wasi02Subset::defineInputStream(api::Linker& linker,
                                                                      std::string const& path)
{
    return linker.defineResource(path, _resources.get<InputStream>());
}

std::expected<void, api::LinkerError> Wasi02Subset::defineDescriptor(api::Linker& linker,
                                                                     std::string const& path)
{
    return linker.defineResource(path, _resources.get<Descriptor>());
}

std::expected<void, api::LinkerError> Wasi02Subset::defineGetEnvironment(api::Linker& linker,
                                                                         std::string const& path)
{
    return defineTyped<std::vector<EnvironmentVariable>()>(linker, path, {}, [this]() noexcept {
        ++_capture.environmentReads;
        return _options.environment;
    });
}

std::expected<void, api::LinkerError> Wasi02Subset::defineExit(api::Linker& linker, std::string const& path)
{
    // exit never returns to the guest: its call fails, which ends the guest's call and locks the store down.
    return defineTyped<void(std::expected<Unit, Unit>)>(
        linker,
        path,
        Status,
        [this](std::expected<Unit, Unit> status) noexcept -> std::expected<void, isoline::api::HostFailure> {
            _capture.exits.push_back(status ? ExitStatus::Success : ExitStatus::Failure);
            return std::unexpected { failureOf(Wasi02Failure::Exited) };
        });
}

std::expected<void, api::LinkerError> Wasi02Subset::defineCheckWrite(api::Linker& linker,
                                                                     std::string const& path)
{
    return defineTyped<std::expected<uint64_t, StreamError>(Borrow<OutputStream>)>(
        linker,
        path,
        Self,
        [this](Borrow<OutputStream> self) noexcept -> std::expected<uint64_t, StreamError> {
            if (_outputs.find(self.rep()) == nullptr)
                return std::unexpected { StreamError { Case<"closed", Unit> {} } };
            return _options.writeCapacity;
        });
}

std::expected<void, api::LinkerError> Wasi02Subset::defineWrite(api::Linker& linker, std::string const& path)
{
    // write and blocking-write-and-flush alike: a captured stream takes every byte at once.
    return defineTyped<std::expected<Unit, StreamError>(Borrow<OutputStream>, std::vector<uint8_t>)>(
        linker,
        path,
        SelfContents,
        [this](Borrow<OutputStream> self,
               std::vector<uint8_t> contents) noexcept -> std::expected<Unit, StreamError> {
            auto const* const target = _outputs.find(self.rep());
            if (target == nullptr)
                return std::unexpected { StreamError { Case<"closed", Unit> {} } };
            auto& captured =
                *target == OutputTarget::StandardOutput ? _capture.standardOutput : _capture.standardError;
            captured.insert(captured.end(), contents.begin(), contents.end());
            return Unit {};
        });
}

std::expected<void, api::LinkerError> Wasi02Subset::defineBlockingFlush(api::Linker& linker,
                                                                        std::string const& path)
{
    return defineTyped<std::expected<Unit, StreamError>(Borrow<OutputStream>)>(
        linker, path, Self, [this](Borrow<OutputStream> self) noexcept -> std::expected<Unit, StreamError> {
            if (_outputs.find(self.rep()) == nullptr)
                return std::unexpected { StreamError { Case<"closed", Unit> {} } };
            return Unit {};
        });
}

std::expected<void, api::LinkerError> Wasi02Subset::defineGetStdin(api::Linker& linker,
                                                                   std::string const& path)
{
    return defineTyped<Own<InputStream>()>(
        linker, path, {}, [this](api::HostComponentContext& context) noexcept {
            return openStream<InputStream>(context, _inputs, Unit {});
        });
}

std::expected<void, api::LinkerError> Wasi02Subset::defineGetStdout(api::Linker& linker,
                                                                    std::string const& path)
{
    return defineTyped<Own<OutputStream>()>(
        linker, path, {}, [this](api::HostComponentContext& context) noexcept {
            return openStream<OutputStream>(context, _outputs, OutputTarget::StandardOutput);
        });
}

std::expected<void, api::LinkerError> Wasi02Subset::defineGetStderr(api::Linker& linker,
                                                                    std::string const& path)
{
    return defineTyped<Own<OutputStream>()>(
        linker, path, {}, [this](api::HostComponentContext& context) noexcept {
            return openStream<OutputStream>(context, _outputs, OutputTarget::StandardError);
        });
}

std::expected<void, api::LinkerError> Wasi02Subset::defineMonotonicNow(api::Linker& linker,
                                                                       std::string const& path)
{
    return defineTyped<uint64_t()>(
        linker, path, {}, [this]() noexcept { return _clock->monotonicNanoseconds(); });
}

std::expected<void, api::LinkerError> Wasi02Subset::defineWallClockNow(api::Linker& linker,
                                                                       std::string const& path)
{
    return defineTyped<Datetime()>(linker, path, {}, [this]() noexcept {
        auto const now = _clock->unixTimeNanoseconds();
        return Datetime { .seconds = now / NanosecondsPerSecond,
                          .nanoseconds = static_cast<uint32_t>(now % NanosecondsPerSecond) };
    });
}

std::expected<void, api::LinkerError> Wasi02Subset::defineWriteViaStream(api::Linker& linker,
                                                                         std::string const& path)
{
    return defineTyped<std::expected<Own<OutputStream>, ErrorCode>(Borrow<Descriptor>, uint64_t)>(
        linker, path, SelfOffset, [](Borrow<Descriptor>, uint64_t) noexcept {
            return badDescriptor<Own<OutputStream>>();
        });
}

std::expected<void, api::LinkerError> Wasi02Subset::defineAppendViaStream(api::Linker& linker,
                                                                          std::string const& path)
{
    return defineTyped<std::expected<Own<OutputStream>, ErrorCode>(Borrow<Descriptor>)>(
        linker, path, Self, [](Borrow<Descriptor>) noexcept { return badDescriptor<Own<OutputStream>>(); });
}

std::expected<void, api::LinkerError> Wasi02Subset::defineGetType(api::Linker& linker,
                                                                  std::string const& path)
{
    return defineTyped<std::expected<DescriptorType, ErrorCode>(Borrow<Descriptor>)>(
        linker, path, Self, [](Borrow<Descriptor>) noexcept { return badDescriptor<DescriptorType>(); });
}

std::expected<void, api::LinkerError> Wasi02Subset::defineStat(api::Linker& linker, std::string const& path)
{
    return defineTyped<std::expected<DescriptorStat, ErrorCode>(Borrow<Descriptor>)>(
        linker, path, Self, [](Borrow<Descriptor>) noexcept { return badDescriptor<DescriptorStat>(); });
}

std::expected<void, api::LinkerError> Wasi02Subset::defineFilesystemErrorCode(api::Linker& linker,
                                                                              std::string const& path)
{
    // No stream fails with an error resource, so none carries a filesystem error code.
    return defineTyped<std::optional<ErrorCode>(Borrow<Error>)>(
        linker, path, Err, [](Borrow<Error>) noexcept { return std::optional<ErrorCode> {}; });
}

std::expected<void, api::LinkerError> Wasi02Subset::defineGetDirectories(api::Linker& linker,
                                                                         std::string const& path)
{
    // No directory is preopened: the guest has no filesystem.
    return defineTyped<std::vector<std::tuple<Own<Descriptor>, std::string>>()>(
        linker, path, {}, []() noexcept { return std::vector<std::tuple<Own<Descriptor>, std::string>> {}; });
}

} // namespace isoline::component::testing
