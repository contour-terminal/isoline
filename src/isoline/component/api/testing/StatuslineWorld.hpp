// SPDX-License-Identifier: Apache-2.0
#pragma once

// Contour's status-line world (`terminal-extensions:statusline@0.1.0`, Contour's wit/statusline/world.wit) as
// the host's C++ types, with their typed-binding rows: what the extension world's host (Extension_test.cpp,
// whose world transcribes it) and the host of Contour's own clock extension (tests/component/contour-clock)
// share. Their bool fields are the WIT's: the wire's yes/no, kept at this boundary as AGENT.md allows.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

#include <isoline/component/api/Typed.hpp>

namespace isoline::component::api::testing
{

using typed::Case;
using typed::Unit;

/// types.render-request.
enum class RenderRequest : uint8_t
{
    None,
    Render,
};

/// types.input-mode.
enum class InputMode : uint8_t
{
    Insert,
    Normal,
    Visual,
    VisualLine,
    VisualBlock,
    Hint,
};

/// types.progress-state.
enum class ProgressState : uint8_t
{
    Hidden,
    Normal,
    Error,
    Indeterminate,
    Warning,
};

/// types.tab.
struct Tab
{
    std::string title;
    bool isActive = false;

    bool operator==(Tab const&) const = default;
};

/// types.progress.
struct Progress
{
    ProgressState state = ProgressState::Hidden;
    uint8_t percent = 0;
};

/// types.font-metrics.
struct FontMetrics
{
    float sizePt = 0;
    uint32_t cellWidthPx = 0;
    uint32_t cellHeightPx = 0;
};

/// types.size.
struct Size
{
    uint32_t cols = 0;
    uint32_t rows = 0;

    bool operator==(Size const&) const = default;
};

/// types.event-kinds: bit i for label i.
struct EventKinds
{
    uint32_t bits = 0;

    bool operator==(EventKinds const&) const = default;
};

/// A name and a value: a configuration entry, a user variable, a command argument.
using Assignment = std::tuple<std::string, std::string>;

/// types.event.
using Event = std::variant<Case<"title", std::string>,
                           Case<"cwd", std::string>,
                           Case<"foreground-process", std::string>,
                           Case<"input-mode", InputMode>,
                           Case<"tabs", std::vector<Tab>>,
                           Case<"user-var", Assignment>,
                           Case<"progress", Progress>,
                           Case<"focus", bool>,
                           Case<"font-changed", FontMetrics>,
                           Case<"resized", Size>,
                           Case<"timer", Unit>,
                           Case<"reply", std::vector<uint8_t>>>;

/// host.log-level.
enum class LogLevel : uint8_t
{
    Trace,
    Debug,
    Info,
    Warn,
    Error,
};

/// host.state-snapshot.
struct StateSnapshot
{
    std::string title;
    std::string cwd;
    std::string foregroundProcess;
    InputMode inputMode = InputMode::Insert;
    std::vector<Tab> tabs;
    std::vector<Assignment> userVars;
    Progress progress {};
    bool isFocused = false;
    Size size {};
    uint64_t unixTimeMs = 0;
    int32_t utcOffsetMinutes = 0;
    FontMetrics font {};
};

/// host.command-output.
struct CommandOutput
{
    int32_t exitCode = 0;
    std::vector<uint8_t> standardOutput;
    std::vector<uint8_t> standardError;
};

/// host.command-error.
enum class CommandError : uint8_t
{
    NotDeclared,
    Pending,
    Denied,
    Failed,
    TimedOut,
};

} // namespace isoline::component::api::testing

namespace isoline::component::api::typed
{

template <>
struct Enum<testing::RenderRequest>
{
    static constexpr auto Labels = std::to_array<std::string_view>({ "none", "render" });
};

template <>
struct Enum<testing::InputMode>
{
    static constexpr auto Labels = std::to_array<std::string_view>(
        { "insert", "normal", "visual", "visual-line", "visual-block", "hint" });
};

template <>
struct Enum<testing::ProgressState>
{
    static constexpr auto Labels =
        std::to_array<std::string_view>({ "hidden", "normal", "error", "indeterminate", "warning" });
};

template <>
struct Record<testing::Tab>
{
    static constexpr auto Fields =
        std::tuple { field("title", &testing::Tab::title), field("active", &testing::Tab::isActive) };
};

template <>
struct Record<testing::Progress>
{
    static constexpr auto Fields = std::tuple { field("state", &testing::Progress::state),
                                                field("percent", &testing::Progress::percent) };
};

template <>
struct Record<testing::FontMetrics>
{
    static constexpr auto Fields =
        std::tuple { field("size-pt", &testing::FontMetrics::sizePt),
                     field("cell-width-px", &testing::FontMetrics::cellWidthPx),
                     field("cell-height-px", &testing::FontMetrics::cellHeightPx) };
};

template <>
struct Record<testing::Size>
{
    static constexpr auto Fields =
        std::tuple { field("cols", &testing::Size::cols), field("rows", &testing::Size::rows) };
};

template <>
struct Flags<testing::EventKinds>
{
    static constexpr auto Labels = std::to_array<std::string_view>({ "title",
                                                                     "cwd",
                                                                     "foreground-process",
                                                                     "input-mode",
                                                                     "tabs",
                                                                     "user-var",
                                                                     "progress",
                                                                     "focus",
                                                                     "font" });
};

template <>
struct Enum<testing::LogLevel>
{
    static constexpr auto Labels =
        std::to_array<std::string_view>({ "trace", "debug", "info", "warn", "error" });
};

template <>
struct Record<testing::StateSnapshot>
{
    static constexpr auto Fields = std::tuple {
        field("title", &testing::StateSnapshot::title),
        field("cwd", &testing::StateSnapshot::cwd),
        field("foreground-process", &testing::StateSnapshot::foregroundProcess),
        field("input-mode", &testing::StateSnapshot::inputMode),
        field("tabs", &testing::StateSnapshot::tabs),
        field("user-vars", &testing::StateSnapshot::userVars),
        field("progress", &testing::StateSnapshot::progress),
        field("focused", &testing::StateSnapshot::isFocused),
        field("size", &testing::StateSnapshot::size),
        field("unix-time-ms", &testing::StateSnapshot::unixTimeMs),
        field("utc-offset-minutes", &testing::StateSnapshot::utcOffsetMinutes),
        field("font", &testing::StateSnapshot::font),
    };
};

template <>
struct Record<testing::CommandOutput>
{
    static constexpr auto Fields = std::tuple { field("exit-code", &testing::CommandOutput::exitCode),
                                                field("stdout", &testing::CommandOutput::standardOutput),
                                                field("stderr", &testing::CommandOutput::standardError) };
};

template <>
struct Enum<testing::CommandError>
{
    static constexpr auto Labels =
        std::to_array<std::string_view>({ "not-declared", "pending", "denied", "failed", "timed-out" });
};

} // namespace isoline::component::api::typed
