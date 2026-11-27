// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Generator.hpp>
#include <core/platform/FileSystem.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace isoline::cli::testing
{

/// A FileSystem call a FaultyFileSystem fails.
enum class Fault : uint8_t
{
    None,           ///< Every call succeeds as the inner file system's does.
    Permissions,    ///< permissions() fails.
    CreateNewFile,  ///< createNewFile() fails.
    OpenWrite,      ///< openWrite() fails.
    Write,          ///< openWrite() returns a stream that takes no byte.
    SetPermissions, ///< setPermissions() fails.
    SyncFile,       ///< syncFile() fails.
    SyncFileThrows, ///< syncFile() throws std::runtime_error, as a broken host might.
    ReplaceFile,    ///< replaceFile() fails.
    SyncDirectory,  ///< syncDirectory() fails.
    OpenRead,       ///< openRead() fails.
    Remove,         ///< remove() fails.
};

/// A FileSystem that forwards every call to another one, but fails those its Faults name: the
/// failures a real file system shows only when a disk fills or breaks.
class FaultyFileSystem final: public core::platform::FileSystem
{
  public:
    /// Forwards to @p inner, failing @p faults.
    /// @param inner The file system; it must outlive this one.
    /// @param faults The calls to fail.
    FaultyFileSystem(core::platform::FileSystem const& inner, std::initializer_list<Fault> faults):
        _inner { inner }, _faults { faults }
    {
    }

    [[nodiscard]] bool exists(std::filesystem::path const& path) const override
    {
        return _inner.exists(path);
    }
    [[nodiscard]] bool isDirectory(std::filesystem::path const& path) const override
    {
        return _inner.isDirectory(path);
    }
    [[nodiscard]] bool isRegularFile(std::filesystem::path const& path) const override
    {
        return _inner.isRegularFile(path);
    }
    [[nodiscard]] bool isSymlink(std::filesystem::path const& path) const override
    {
        return _inner.isSymlink(path);
    }
    [[nodiscard]] bool isExecutableFile(std::filesystem::path const& path) const override
    {
        return _inner.isExecutableFile(path);
    }
    [[nodiscard]] std::filesystem::path weaklyCanonical(std::filesystem::path const& path) const override
    {
        return _inner.weaklyCanonical(path);
    }
    [[nodiscard]] std::filesystem::path currentPath() const override { return _inner.currentPath(); }
    [[nodiscard]] std::expected<std::string, std::string> readFile(
        std::filesystem::path const& path) const override
    {
        return _inner.readFile(path);
    }
    [[nodiscard]] std::expected<void, std::string> writeFile(std::filesystem::path const& path,
                                                             std::string_view content) const override
    {
        return _inner.writeFile(path, content);
    }
    [[nodiscard]] std::expected<void, std::string> appendFile(std::filesystem::path const& path,
                                                              std::string_view content) const override
    {
        return _inner.appendFile(path, content);
    }
    [[nodiscard]] std::expected<std::unique_ptr<std::istream>, std::string> openRead(
        std::filesystem::path const& path) const override
    {
        if (_faults.contains(Fault::OpenRead))
            return refuse("openRead");
        return _inner.openRead(path);
    }
    [[nodiscard]] std::expected<std::unique_ptr<std::ostream>, std::string> openWrite(
        std::filesystem::path const& path, core::platform::WriteMode mode) const override
    {
        if (_faults.contains(Fault::OpenWrite))
            return refuse("openWrite");
        if (_faults.contains(Fault::Write))
        {
            auto stream = std::make_unique<std::ostringstream>();
            stream->setstate(std::ios::badbit);
            return stream;
        }
        return _inner.openWrite(path, mode);
    }
    [[nodiscard]] std::expected<std::unique_ptr<std::iostream>, std::string> openReadWrite(
        std::filesystem::path const& path) const override
    {
        return _inner.openReadWrite(path);
    }
    [[nodiscard]] std::expected<void, std::string> createDirectory(
        std::filesystem::path const& path) const override
    {
        return _inner.createDirectory(path);
    }
    [[nodiscard]] std::expected<void, std::string> createDirectories(
        std::filesystem::path const& path) const override
    {
        return _inner.createDirectories(path);
    }
    [[nodiscard]] std::expected<bool, std::string> remove(std::filesystem::path const& path) const override
    {
        if (_faults.contains(Fault::Remove))
            return refuse("remove");
        return _inner.remove(path);
    }
    [[nodiscard]] std::expected<std::uintmax_t, std::string> removeAll(
        std::filesystem::path const& path) const override
    {
        return _inner.removeAll(path);
    }
    [[nodiscard]] std::expected<void, std::string> copyFile(
        std::filesystem::path const& from,
        std::filesystem::path const& to,
        core::platform::OverwritePolicy policy) const override
    {
        return _inner.copyFile(from, to, policy);
    }
    [[nodiscard]] std::expected<void, std::string> rename(std::filesystem::path const& from,
                                                          std::filesystem::path const& to) const override
    {
        return _inner.rename(from, to);
    }
    [[nodiscard]] std::expected<std::vector<DirectoryEntry>, std::string> listDirectory(
        std::filesystem::path const& path) const override
    {
        return _inner.listDirectory(path);
    }
    [[nodiscard]] core::Generator<DirectoryEntry> walkDirectoryRecursive(
        std::filesystem::path path, std::error_code* outError) const override
    {
        return _inner.walkDirectoryRecursive(std::move(path), outError);
    }
    [[nodiscard]] std::expected<std::uintmax_t, std::string> fileSize(
        std::filesystem::path const& path) const override
    {
        return _inner.fileSize(path);
    }
    [[nodiscard]] std::expected<std::filesystem::file_time_type, std::string> lastWriteTime(
        std::filesystem::path const& path) const override
    {
        return _inner.lastWriteTime(path);
    }
    [[nodiscard]] std::expected<std::filesystem::perms, std::string> permissions(
        std::filesystem::path const& path) const override
    {
        if (_faults.contains(Fault::Permissions))
            return refuse("permissions");
        return _inner.permissions(path);
    }
    [[nodiscard]] std::expected<void, std::string> setPermissions(std::filesystem::path const& path,
                                                                  std::filesystem::perms perms) const override
    {
        if (_faults.contains(Fault::SetPermissions))
            return refuse("setPermissions");
        return _inner.setPermissions(path, perms);
    }
    [[nodiscard]] std::expected<std::filesystem::path, std::string> createTempFile(
        std::string_view prefix) const override
    {
        return _inner.createTempFile(prefix);
    }
    [[nodiscard]] std::expected<void, std::string> createNewFile(
        std::filesystem::path const& path) const override
    {
        if (_faults.contains(Fault::CreateNewFile))
            return refuse("createNewFile");
        return _inner.createNewFile(path);
    }
    [[nodiscard]] std::expected<void, std::string> syncFile(std::filesystem::path const& path) const override
    {
        if (_faults.contains(Fault::SyncFile))
            return refuse("syncFile");
        if (_faults.contains(Fault::SyncFileThrows))
            throw std::runtime_error { "syncFile threw" };
        return _inner.syncFile(path);
    }
    [[nodiscard]] std::expected<void, std::string> replaceFile(std::filesystem::path const& from,
                                                               std::filesystem::path const& to) const override
    {
        if (_faults.contains(Fault::ReplaceFile))
            return refuse("replaceFile");
        return _inner.replaceFile(from, to);
    }
    [[nodiscard]] std::expected<void, std::string> syncDirectory(
        std::filesystem::path const& directory) const override
    {
        if (_faults.contains(Fault::SyncDirectory))
            return refuse("syncDirectory");
        return _inner.syncDirectory(directory);
    }

  private:
    /// The failure of @p call.
    /// @param call The call's name.
    /// @return Its refusal.
    [[nodiscard]] static std::unexpected<std::string> refuse(std::string_view call)
    {
        return std::unexpected { std::format("{} failed", call) };
    }

    core::platform::FileSystem const& _inner; ///< What every call goes to.
    std::set<Fault> _faults;                  ///< The calls that fail.
};

} // namespace isoline::cli::testing
