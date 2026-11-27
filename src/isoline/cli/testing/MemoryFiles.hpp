// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <expected>
#include <map>
#include <new>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <isoline/cli/FileAccess.hpp>

namespace isoline::cli::testing
{

/// An in-memory file system for the tool's tests: files by path, paths that refuse a write, and paths
/// whose read throws std::bad_alloc, as a host out of memory would.
class MemoryFiles final: public IFileAccess
{
  public:
    std::map<std::string, std::vector<std::byte>, std::less<>> files; ///< The files, by path.
    std::set<std::string, std::less<>> unwritable;                    ///< Paths a write refuses.
    std::set<std::string, std::less<>> exhausting;                    ///< Paths a read throws on.
    std::set<std::string, std::less<>> terminals;                     ///< Paths that are terminals.

    /// Adds a file of text.
    /// @param path Its path.
    /// @param text Its contents.
    void add(std::string const& path, std::string_view text)
    {
        auto const bytes = std::as_bytes(std::span { text });
        files[path] = std::vector<std::byte>(bytes.begin(), bytes.end());
    }

    /// The text of a file, which must exist.
    /// @param path Its path.
    /// @return Its contents.
    [[nodiscard]] std::string text(std::string const& path) const
    {
        auto const& bytes = files.at(path);
        return { reinterpret_cast<char const*>(bytes.data()), bytes.size() };
    }

    [[nodiscard]] std::expected<std::vector<std::byte>, FileError> read(std::string_view path,
                                                                        std::size_t maxBytes) override
    {
        if (exhausting.contains(path))
            throw std::bad_alloc {};
        auto const file = files.find(path);
        if (file == files.end())
            return std::unexpected { FileError { .kind = FileErrorKind::Unavailable,
                                                 .reason = "No such file or directory" } };
        if (file->second.size() > maxBytes)
            return std::unexpected { FileError { .kind = FileErrorKind::TooLarge, .reason = "too large" } };
        return file->second;
    }

    [[nodiscard]] std::expected<void, FileError> write(std::string_view path,
                                                       std::span<std::byte const> bytes) override
    {
        if (unwritable.contains(path))
            return std::unexpected { FileError { .kind = FileErrorKind::Unavailable,
                                                 .reason = "Permission denied" } };
        files[std::string { path }] = std::vector<std::byte>(bytes.begin(), bytes.end());
        return {};
    }

    [[nodiscard]] bool isTerminal(std::string_view path) const override { return terminals.contains(path); }

    [[nodiscard]] bool isSameFile(std::string_view first, std::string_view second) const override
    {
        return first == second && first != StandardStreamPath;
    }
};

} // namespace isoline::cli::testing
