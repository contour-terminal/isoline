// SPDX-License-Identifier: Apache-2.0
#pragma once

// A test double of the WASI 0.2 the preview-1 reactor adapter imports once it is componentized into Contour's
// clock extension (plan P11 Task 22): exactly the interfaces clock.wasm's Component::imports() name, every
// function a typed host function. Isoline ships no WASI 0.2 (spec anti-goal): this lives in a test target
// only, and README.md lists the interfaces as what Contour's extension host (P9) binds in its own Linker.
//
// What it does: output streams capture what is written to them, stdout and stderr apart; the clocks read a
// FakeClock that moves only when the test advances it; the environment is the one it is given; `exit` fails
// its call (a trap to the guest, which locks the store down); there is no filesystem (no preopened directory,
// and every descriptor method answers bad-descriptor). The adapter imports no random source, so there is
// none.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

#include <isoline/component/api/ComponentStore.hpp>
#include <isoline/component/api/Linker.hpp>
#include <isoline/component/api/Typed.hpp>

namespace isoline::component::testing
{

using api::typed::Case;
using api::typed::Own;
using api::typed::Unit;

// The interfaces' types as C++ types (WASI 0.2.12's WIT, as clock.wasm imports it).

/// wasi:io/error.error's C++ tag.
struct Error
{
};

/// wasi:io/streams.output-stream's C++ tag.
struct OutputStream
{
};

/// wasi:io/streams.input-stream's C++ tag.
struct InputStream
{
};

/// wasi:filesystem/types.descriptor's C++ tag.
struct Descriptor
{
};

/// wasi:io/streams.stream-error.
using StreamError = std::variant<Case<"last-operation-failed", Own<Error>>, Case<"closed", Unit>>;

/// wasi:clocks/wall-clock.datetime.
struct Datetime
{
    uint64_t seconds = 0;
    uint32_t nanoseconds = 0;

    bool operator==(Datetime const&) const = default;
};

/// wasi:filesystem/types.error-code.
enum class ErrorCode : uint8_t
{
    Access,
    WouldBlock,
    Already,
    BadDescriptor,
    Busy,
    Deadlock,
    Quota,
    Exist,
    FileTooLarge,
    IllegalByteSequence,
    InProgress,
    Interrupted,
    Invalid,
    Io,
    IsDirectory,
    Loop,
    TooManyLinks,
    MessageSize,
    NameTooLong,
    NoDevice,
    NoEntry,
    NoLock,
    InsufficientMemory,
    InsufficientSpace,
    NotDirectory,
    NotEmpty,
    NotRecoverable,
    Unsupported,
    NoTty,
    NoSuchDevice,
    Overflow,
    NotPermitted,
    Pipe,
    ReadOnly,
    InvalidSeek,
    TextFileBusy,
    CrossDevice,
};

/// wasi:filesystem/types.descriptor-type.
enum class DescriptorType : uint8_t
{
    Unknown,
    BlockDevice,
    CharacterDevice,
    Directory,
    Fifo,
    SymbolicLink,
    RegularFile,
    Socket,
};

/// wasi:filesystem/types.descriptor-stat.
struct DescriptorStat
{
    DescriptorType type = DescriptorType::Unknown;
    uint64_t linkCount = 0;
    uint64_t size = 0;
    std::optional<Datetime> dataAccessTimestamp {};
    std::optional<Datetime> dataModificationTimestamp {};
    std::optional<Datetime> statusChangeTimestamp {};
};

/// A name and a value: an environment variable.
using EnvironmentVariable = std::tuple<std::string, std::string>;

/// The time the double's clocks tell: a wall time and a monotonic time, which move together, and only when
/// the test advances them.
class FakeClock
{
  public:
    /// A clock at @p unixTimeNanoseconds, its monotonic time 0.
    /// @param unixTimeNanoseconds The wall time, in nanoseconds since the Unix epoch.
    explicit FakeClock(uint64_t unixTimeNanoseconds) noexcept: _unixTimeNanoseconds { unixTimeNanoseconds } {}

    /// The wall time, in nanoseconds since the Unix epoch.
    [[nodiscard]] uint64_t unixTimeNanoseconds() const noexcept { return _unixTimeNanoseconds; }

    /// The monotonic time, in nanoseconds since the clock was made.
    [[nodiscard]] uint64_t monotonicNanoseconds() const noexcept { return _monotonicNanoseconds; }

    /// Moves both times on.
    /// @param nanoseconds How far.
    void advance(uint64_t nanoseconds) noexcept
    {
        _unixTimeNanoseconds += nanoseconds;
        _monotonicNanoseconds += nanoseconds;
    }

  private:
    uint64_t _unixTimeNanoseconds;
    uint64_t _monotonicNanoseconds = 0;
};

/// How a guest's exit status read.
enum class ExitStatus : uint8_t
{
    Success, ///< `exit(ok)`.
    Failure, ///< `exit(err)`.
};

/// Why a body of the double failed its call: the code of the HostFailure it returned.
enum class Wasi02Failure : uint8_t
{
    Exited = 1, ///< The guest called `exit`, which never returns to it.
    TableFull,  ///< The double's table of streams is full.
    OwnRefused, ///< The store refused the own of a stream (ComponentStore::hostResource()).
};

/// What the guest did through the double.
struct Wasi02Capture
{
    std::vector<uint8_t> standardOutput {}; ///< Everything written to stdout's streams.
    std::vector<uint8_t> standardError {};  ///< Everything written to stderr's streams.
    uint32_t environmentReads = 0;          ///< The calls of get-environment.
    std::vector<ExitStatus> exits {};       ///< The calls of exit, in order.
};

/// How a double is set up.
struct Wasi02Options
{
    std::vector<EnvironmentVariable> environment {}; ///< What get-environment answers.
    /// What check-write answers: the bytes a stream takes at once.
    uint64_t writeCapacity = 4'096;
};

/// One definition of the double: the interface, as the adapter imports it, and the name in it.
struct Wasi02Binding
{
    std::string_view interface; ///< `wasi:cli/environment@0.2.12`.
    std::string_view name;      ///< `get-environment`, `[method]output-stream.write`, or a resource's name.

    bool operator==(Wasi02Binding const&) const = default;
};

/// The resource types the double's handles name, for the typed bindings.
using Wasi02Resources = api::typed::ResourceTypes<Error, OutputStream, InputStream, Descriptor>;

/// Which of the guest's standard streams an output-stream writes to.
enum class OutputTarget : uint8_t
{
    StandardOutput,
    StandardError,
};

/// The double: its definitions, in a Linker it was given, and their state. Neither copied nor moved: its host
/// functions refer to it.
class Wasi02Subset
{
  public:
    /// Defines every binding in @p linker.
    /// @param linker The linker; the double outlives every store instantiated with it.
    /// @param clock What the clocks read; it outlives the double.
    /// @param options The environment and the streams' capacity.
    /// @return The double; or the linker's refusal of a definition (the linker keeps the ones before it).
    /// @throws std::bad_alloc When a definition cannot be stored.
    [[nodiscard]] static std::expected<std::unique_ptr<Wasi02Subset>, api::LinkerError> define(
        api::Linker& linker, FakeClock const& clock, Wasi02Options options = {});

    Wasi02Subset(Wasi02Subset const&) = delete;
    Wasi02Subset(Wasi02Subset&&) = delete;
    Wasi02Subset& operator=(Wasi02Subset const&) = delete;
    Wasi02Subset& operator=(Wasi02Subset&&) = delete;
    ~Wasi02Subset() = default;

    /// Every binding, in the order defined: a row per function and resource of every interface.
    [[nodiscard]] static std::vector<Wasi02Binding> bindings();

    /// The interfaces the bindings are of, each once, in their order: what Contour's extension host binds.
    [[nodiscard]] static std::vector<std::string_view> interfaces();

    /// What the guest did.
    [[nodiscard]] Wasi02Capture const& capture() const noexcept { return _capture; }

    /// The resource types of its handles.
    [[nodiscard]] Wasi02Resources const& resources() const noexcept { return _resources; }

    /// The streams the double holds an object for: those handed out and not yet destroyed.
    [[nodiscard]] std::size_t openStreams() const noexcept { return _outputs.size() + _inputs.size(); }

  private:
    /// Defines one binding at @p path in @p linker.
    using Definer = std::expected<void, api::LinkerError> (Wasi02Subset::*)(api::Linker& linker,
                                                                            std::string const& path);

    /// A binding and what defines it.
    struct Row
    {
        Wasi02Binding binding;
        Definer define = nullptr;
    };

    /// Every binding's row, in the order defined.
    [[nodiscard]] static std::span<Row const> rows() noexcept;

    Wasi02Subset(FakeClock const& clock, Wasi02Options options);

    /// Defines @\p body at @p path as a typed host function of @p Signature, its parameters labelled @p
    /// labels.
    template <typename Signature, typename Body>
    [[nodiscard]] std::expected<void, api::LinkerError> defineTyped(api::Linker& linker,
                                                                    std::string const& path,
                                                                    std::span<std::string_view const> labels,
                                                                    Body body);

    /// An own of a new stream of @p table, holding @p object, for the guest.
    template <typename Tag, typename T>
    [[nodiscard]] std::expected<Own<Tag>, isoline::api::HostFailure> openStream(
        api::HostComponentContext& context, api::typed::HostResourceTable<T>& table, T object);

    std::expected<void, api::LinkerError> defineError(api::Linker& linker, std::string const& path);
    std::expected<void, api::LinkerError> defineOutputStream(api::Linker& linker, std::string const& path);
    std::expected<void, api::LinkerError> defineInputStream(api::Linker& linker, std::string const& path);
    std::expected<void, api::LinkerError> defineDescriptor(api::Linker& linker, std::string const& path);
    std::expected<void, api::LinkerError> defineGetEnvironment(api::Linker& linker, std::string const& path);
    std::expected<void, api::LinkerError> defineExit(api::Linker& linker, std::string const& path);
    std::expected<void, api::LinkerError> defineCheckWrite(api::Linker& linker, std::string const& path);
    std::expected<void, api::LinkerError> defineWrite(api::Linker& linker, std::string const& path);
    std::expected<void, api::LinkerError> defineBlockingFlush(api::Linker& linker, std::string const& path);
    std::expected<void, api::LinkerError> defineGetStdin(api::Linker& linker, std::string const& path);
    std::expected<void, api::LinkerError> defineGetStdout(api::Linker& linker, std::string const& path);
    std::expected<void, api::LinkerError> defineGetStderr(api::Linker& linker, std::string const& path);
    std::expected<void, api::LinkerError> defineMonotonicNow(api::Linker& linker, std::string const& path);
    std::expected<void, api::LinkerError> defineWallClockNow(api::Linker& linker, std::string const& path);
    std::expected<void, api::LinkerError> defineWriteViaStream(api::Linker& linker, std::string const& path);
    std::expected<void, api::LinkerError> defineAppendViaStream(api::Linker& linker, std::string const& path);
    std::expected<void, api::LinkerError> defineGetType(api::Linker& linker, std::string const& path);
    std::expected<void, api::LinkerError> defineStat(api::Linker& linker, std::string const& path);
    std::expected<void, api::LinkerError> defineFilesystemErrorCode(api::Linker& linker,
                                                                    std::string const& path);
    std::expected<void, api::LinkerError> defineGetDirectories(api::Linker& linker, std::string const& path);

    api::typed::HostResourceTable<OutputTarget> _outputs { 16 }; ///< The output streams handed out.
    api::typed::HostResourceTable<Unit> _inputs { 16 };          ///< The input streams handed out.
    Wasi02Resources _resources;
    Wasi02Capture _capture {};
    FakeClock const* _clock;
    Wasi02Options _options;
};

} // namespace isoline::component::testing

namespace isoline::component::api::typed
{

template <>
struct Record<component::testing::Datetime>
{
    static constexpr auto Fields =
        std::tuple { field("seconds", &component::testing::Datetime::seconds),
                     field("nanoseconds", &component::testing::Datetime::nanoseconds) };
};

template <>
struct Enum<component::testing::ErrorCode>
{
    static constexpr auto Labels = std::to_array<std::string_view>({
        "access",
        "would-block",
        "already",
        "bad-descriptor",
        "busy",
        "deadlock",
        "quota",
        "exist",
        "file-too-large",
        "illegal-byte-sequence",
        "in-progress",
        "interrupted",
        "invalid",
        "io",
        "is-directory",
        "loop",
        "too-many-links",
        "message-size",
        "name-too-long",
        "no-device",
        "no-entry",
        "no-lock",
        "insufficient-memory",
        "insufficient-space",
        "not-directory",
        "not-empty",
        "not-recoverable",
        "unsupported",
        "no-tty",
        "no-such-device",
        "overflow",
        "not-permitted",
        "pipe",
        "read-only",
        "invalid-seek",
        "text-file-busy",
        "cross-device",
    });
};

template <>
struct Enum<component::testing::DescriptorType>
{
    static constexpr auto Labels = std::to_array<std::string_view>({ "unknown",
                                                                     "block-device",
                                                                     "character-device",
                                                                     "directory",
                                                                     "fifo",
                                                                     "symbolic-link",
                                                                     "regular-file",
                                                                     "socket" });
};

template <>
struct Record<component::testing::DescriptorStat>
{
    using Stat = component::testing::DescriptorStat;
    static constexpr auto Fields = std::tuple {
        field("type", &Stat::type),
        field("link-count", &Stat::linkCount),
        field("size", &Stat::size),
        field("data-access-timestamp", &Stat::dataAccessTimestamp),
        field("data-modification-timestamp", &Stat::dataModificationTimestamp),
        field("status-change-timestamp", &Stat::statusChangeTimestamp),
    };
};

} // namespace isoline::component::api::typed
