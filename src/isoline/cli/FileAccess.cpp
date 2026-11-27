// SPDX-License-Identifier: Apache-2.0
#include <isoline/cli/FileAccess.hpp>

#include <core/Utf8Path.hpp>
#include <core/platform/FileSystem.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <istream>
#include <memory>
#include <ostream>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace isoline::cli
{

namespace
{
    /// The most bytes one read asks a stream for.
    constexpr auto ChunkBytes = std::size_t { 1 } << 16;

    /// The most temporary names a write tries beside its target, should earlier runs have left some.
    constexpr auto TemporaryNameAttempts = 16;

    /// A refusal for a file that could not be opened, read or written.
    /// @param reason Why, for the user.
    /// @return The FileError.
    std::unexpected<FileError> unavailable(std::string reason)
    {
        return std::unexpected { FileError { .kind = FileErrorKind::Unavailable,
                                             .reason = std::move(reason) } };
    }

    /// The path that UTF-8 @p text names.
    /// @param text The path as the command line gave it.
    /// @return The path, or why it names none: text that is not UTF-8, on Windows alone.
    std::expected<std::filesystem::path, FileError> nameOf(std::string_view text)
    {
        auto name = core::pathFromUtf8(text);
        if (!name)
            return unavailable(name.error().message()); // uncovered(non-utf8-path)
        return std::move(*name);
    }

    /// Reads @p stream to its end, refusing it once it holds more than @p maxBytes: at most @p maxBytes + 1
    /// bytes are read, into a vector grown by doubling (IFileAccess::read).
    /// @param stream The stream.
    /// @param maxBytes The most bytes it may hold.
    /// @return Its bytes, or why it could not be read.
    std::expected<std::vector<std::byte>, FileError> readBounded(std::istream& stream, std::size_t maxBytes)
    {
        auto bytes = std::vector<std::byte> {};
        while (stream.good())
        {
            // One byte past the limit is enough to tell a stream too large.
            auto const room = maxBytes - bytes.size();
            auto const wanted = room < ChunkBytes ? room + 1 : ChunkBytes;
            auto const start = bytes.size();
            bytes.resize(start + wanted);
            stream.read(reinterpret_cast<char*>(bytes.data() + start), static_cast<std::streamsize>(wanted));
            bytes.resize(start + static_cast<std::size_t>(stream.gcount()));
            if (bytes.size() > maxBytes)
                return std::unexpected { FileError { .kind = FileErrorKind::TooLarge,
                                                     .reason = "larger than its limit" } };
        }
        // A read that ends at the end of the stream sets failbit with eofbit; one that fails sets
        // badbit, or failbit alone.
        if (stream.bad() || !stream.eof())
            return unavailable("read error");
        return bytes;
    }

    /// Writes @p bytes to @p stream and flushes it.
    /// @param stream The stream.
    /// @param bytes What to write.
    /// @return Nothing, or a write error when the stream did not take every byte.
    std::expected<void, FileError> writeAll(std::ostream& stream, std::span<std::byte const> bytes)
    {
        stream.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream.good())
            return unavailable("write error");
        return {};
    }

    /// Opens @p name for writing, truncated, writes @p bytes and closes it.
    /// @param fileSystem The file system.
    /// @param name The file.
    /// @param bytes What to write.
    /// @return Nothing, or why it could not be written.
    std::expected<void, FileError> writeFile(core::platform::FileSystem const& fileSystem,
                                             std::filesystem::path const& name,
                                             std::span<std::byte const> bytes)
    {
        auto stream = fileSystem.openWrite(name);
        if (!stream)
            return unavailable(stream.error());
        return writeAll(**stream, bytes);
    }

    /// The directory @p name is in: its parent, or the current directory for a bare name.
    /// @param name A path.
    /// @return The directory.
    std::filesystem::path directoryOf(std::filesystem::path const& name)
    {
        auto const parent = name.parent_path();
        return parent.empty() ? std::filesystem::path { "." } : parent;
    }

    /// A temporary file this process created, removed when it goes out of scope unless it was renamed
    /// away (release()) or removed already (remove()): so that no path, an exception's included, leaves
    /// it behind.
    class TemporaryFile
    {
      public:
        /// Owns the file at @p path.
        /// @param fileSystem The file system it is in; it must outlive this object.
        /// @param path The file.
        TemporaryFile(core::platform::FileSystem const& fileSystem, std::filesystem::path path):
            _fileSystem { fileSystem }, _path { std::move(path) }
        {
        }

        TemporaryFile(TemporaryFile const&) = delete;
        TemporaryFile(TemporaryFile&&) = delete;
        TemporaryFile& operator=(TemporaryFile const&) = delete;
        TemporaryFile& operator=(TemporaryFile&&) = delete;

        ~TemporaryFile()
        {
            // Unwinding: the removal's failure has no one to report to, and the file keeps a name the
            // next write steps past.
            if (_ownership == Ownership::Owned)
                (void) _fileSystem.remove(_path).has_value();
        }

        /// The file's path.
        /// @return The path.
        [[nodiscard]] std::filesystem::path const& path() const noexcept { return _path; }

        /// Gives the file up: it was renamed over its target.
        void release() noexcept { _ownership = Ownership::Released; }

        /// Removes the file now.
        /// @return Nothing, or why it could not be removed.
        [[nodiscard]] std::expected<bool, std::string> remove()
        {
            _ownership = Ownership::Released;
            return _fileSystem.remove(_path);
        }

      private:
        /// Whether the destructor still removes the file.
        enum class Ownership : uint8_t
        {
            Owned,    ///< It does.
            Released, ///< It was renamed away or removed.
        };

        core::platform::FileSystem const& _fileSystem; ///< The file system it is in.
        std::filesystem::path _path;                   ///< The file.
        Ownership _ownership = Ownership::Owned;       ///< Whether the destructor removes it.
    };

    /// Creates a new, empty temporary file beside @p name, of a name no file has.
    /// @param fileSystem The file system.
    /// @param name The file it will replace.
    /// @return The temporary file's path, or why none could be created.
    std::expected<std::filesystem::path, FileError> createTemporaryBeside(
        core::platform::FileSystem const& fileSystem, std::filesystem::path const& name)
    {
        auto reason = std::string {};
        for (auto const attempt: std::views::iota(0, TemporaryNameAttempts))
        {
            auto temporaryName = name;
            temporaryName.replace_filename(core::pathFromTrustedUtf8(
                std::format(".{}.isoline-{}", core::pathToUtf8(name.filename()), attempt)));
            auto const created = fileSystem.createNewFile(temporaryName);
            if (created)
                return temporaryName;
            reason = created.error();
        }
        return unavailable(std::move(reason));
    }
} // namespace

std::expected<std::vector<std::byte>, FileError> SystemFileAccess::read(std::string_view path,
                                                                        std::size_t maxBytes)
{
    if (path == StandardStreamPath)
        return readBounded(_streams.input, maxBytes);
    return nameOf(path).and_then(
        [&](std::filesystem::path const& name) -> std::expected<std::vector<std::byte>, FileError> {
            // A directory opens for reading on POSIX, and libc++ then reads it as an empty stream rather
            // than failing. This asks by name, not of the opened stream (which cannot say), and is only
            // about the message: a file swapped for a directory in between reads as empty, the input's own
            // author's race.
            if (_fileSystem.isDirectory(name))
                return unavailable("Is a directory");
            auto stream = _fileSystem.openRead(name);
            if (!stream)
                return unavailable(stream.error());
            return readBounded(**stream, maxBytes);
        });
}

std::expected<void, FileError> SystemFileAccess::write(std::string_view path,
                                                       std::span<std::byte const> bytes)
{
    if (path == StandardStreamPath)
        return writeAll(_streams.output, bytes);
    return nameOf(path).and_then([&](std::filesystem::path const& name) -> std::expected<void, FileError> {
        // A device or a FIFO is written in place: renaming over it would replace it with a file.
        if (_fileSystem.exists(name) && !_fileSystem.isRegularFile(name))
            return writeFile(_fileSystem, name, bytes);
        return replace(name, bytes);
    });
}

std::expected<void, FileError> SystemFileAccess::replace(std::filesystem::path const& name,
                                                         std::span<std::byte const> bytes) const
{
    auto mode = _newFileMode;
    if (_fileSystem.exists(name))
    {
        auto const existing = _fileSystem.permissions(name);
        if (!existing)
            return unavailable(existing.error());
        mode = *existing;
    }
    auto created = createTemporaryBeside(_fileSystem, name);
    if (!created)
        return std::unexpected { created.error() };
    auto temporary = TemporaryFile { _fileSystem, std::move(*created) };
    auto const replaced =
        writeFile(_fileSystem, temporary.path(), bytes).and_then([&]() -> std::expected<void, FileError> {
            if (auto set = _fileSystem.setPermissions(temporary.path(), mode); !set)
                return unavailable(std::move(set).error());
            if (auto synced = _fileSystem.syncFile(temporary.path()); !synced)
                return unavailable(std::move(synced).error());
            if (auto moved = _fileSystem.replaceFile(temporary.path(), name); !moved)
                return unavailable(std::move(moved).error());
            temporary.release();
            return {};
        });
    if (!replaced)
    {
        // The temporary file is ours, and the target was never touched. A removal that fails leaves
        // it behind, under a name the next write steps past, and the write's own failure is the one
        // reported.
        if (auto const removed = temporary.remove(); !removed)
            return unavailable(std::format(
                "{}; the temporary file is left behind: {}", replaced.error().reason, removed.error()));
        return replaced;
    }
    // The file is in place; this makes its new name survive a power loss.
    if (auto synced = _fileSystem.syncDirectory(directoryOf(name)); !synced)
        return unavailable(std::format("written, but not synced to the disk: {}", synced.error()));
    return {};
}

bool SystemFileAccess::isTerminal(std::string_view path) const
{
    return path == StandardStreamPath && _streams.outputDevice == OutputDevice::Terminal;
}

bool SystemFileAccess::isSameFile(std::string_view first, std::string_view second) const
{
    if (first == StandardStreamPath || second == StandardStreamPath)
        return false;
    auto const firstName = nameOf(first);
    auto const secondName = nameOf(second);
    if (!firstName || !secondName)
        return false; // uncovered(non-utf8-path)
    // Two existing files are the same when they are one file, through a link or not. core-cpp's
    // FileSystem has no such query, so this asks std::filesystem, of the host's files.
    auto error = std::error_code {};
    if (_fileSystem.exists(*firstName) && _fileSystem.exists(*secondName))
        if (auto const same = std::filesystem::equivalent(*firstName, *secondName, error); !error)
            return same;
    // An output to be is the input when both spell one path, relative ones from the current directory.
    auto const resolved = [this](std::filesystem::path const& name) {
        return _fileSystem.weaklyCanonical(name.is_absolute() ? name : _fileSystem.currentPath() / name);
    };
    return resolved(*firstName) == resolved(*secondName);
}

} // namespace isoline::cli
