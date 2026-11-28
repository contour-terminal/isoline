// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <filesystem>
#include <system_error>
#include <utility>

namespace isoline::differential::testing
{

/// A directory of a test's own, for the oracle's modules and outputs, removed with its scope.
class ScratchDirectory
{
  public:
    /// Creates @p path.
    /// @param path The directory.
    explicit ScratchDirectory(std::filesystem::path path): _path { std::move(path) }
    {
        std::filesystem::create_directories(_path);
    }
    ~ScratchDirectory()
    {
        auto ignored = std::error_code {};
        std::filesystem::remove_all(_path, ignored);
    }
    ScratchDirectory(ScratchDirectory const&) = delete;
    ScratchDirectory(ScratchDirectory&&) = delete;
    ScratchDirectory& operator=(ScratchDirectory const&) = delete;
    ScratchDirectory& operator=(ScratchDirectory&&) = delete;

    /// The directory.
    [[nodiscard]] std::filesystem::path const& path() const noexcept { return _path; }

  private:
    std::filesystem::path _path;
};

} // namespace isoline::differential::testing
