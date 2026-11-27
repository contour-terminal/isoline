// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/FileSystem.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// How the isoline tool reaches files: through IFileAccess, so that its verbs run against an in-memory
// file system in tests and against the real one in the tool.
namespace isoline::cli
{

/// The path that names a standard stream: standard input to read, standard output to write.
inline constexpr std::string_view StandardStreamPath = "-";

/// Why a file could not be read or written.
enum class FileErrorKind : uint8_t
{
    Unavailable, ///< It could not be opened, read or written: missing, a directory, unreadable, a full disk.
    TooLarge,    ///< It holds more bytes than its reader accepts.
};

/// A file that could not be read or written, and why.
struct FileError
{
    FileErrorKind kind; ///< What went wrong.
    std::string reason; ///< A phrase for the user, such as "No such file or directory".
};

/// The files a verb reads and writes, by path.
class IFileAccess
{
  public:
    virtual ~IFileAccess() = default;

    /// Reads a whole file, refusing it once it is seen to exceed @p maxBytes: no more than @p maxBytes + 1
    /// of its bytes are read, into a vector grown by doubling, so that reading holds at most about three
    /// times that, while the vector reallocates.
    /// @param path The file, as given on the command line (UTF-8); StandardStreamPath is standard input.
    /// @param maxBytes The most bytes the file may hold.
    /// @return Its bytes, or why it could not be read: TooLarge past @p maxBytes.
    [[nodiscard]] virtual std::expected<std::vector<std::byte>, FileError> read(std::string_view path,
                                                                                std::size_t maxBytes) = 0;

    /// Writes a whole file, replacing what it held. Either the whole new content is there afterwards,
    /// or, when the write fails, what was there before.
    /// @param path The file, as given on the command line (UTF-8); StandardStreamPath is standard output.
    /// @param bytes What to write.
    /// @return Nothing, or why it could not be written.
    [[nodiscard]] virtual std::expected<void, FileError> write(std::string_view path,
                                                               std::span<std::byte const> bytes) = 0;

    /// Whether writing @p path writes to a terminal.
    /// @param path An output path.
    /// @return Whether it is standard output and standard output is a terminal.
    [[nodiscard]] virtual bool isTerminal(std::string_view path) const = 0;

    /// Whether two paths name one file, so that writing one would overwrite the other.
    /// @param first A path; StandardStreamPath names no file.
    /// @param second Another path.
    /// @return Whether both name the same existing file, or the same file to be.
    [[nodiscard]] virtual bool isSameFile(std::string_view first, std::string_view second) const = 0;
};

/// What standard output is connected to.
enum class OutputDevice : uint8_t
{
    Other,    ///< A file or a pipe.
    Terminal, ///< A terminal, where a binary would print as noise.
};

/// The process's standard streams, as SystemFileAccess reaches them.
struct StandardStreams
{
    std::istream& input;       ///< What reading StandardStreamPath reads.
    std::ostream& output;      ///< What writing StandardStreamPath writes.
    OutputDevice outputDevice; ///< What @p output is connected to.
};

/// The host's file system, through a core::platform::FileSystem, and the process's standard streams.
///
/// A regular file is written atomically: to a new temporary file beside it, synced, then renamed over
/// it (FileSystem's createNewFile, syncFile, replaceFile and syncDirectory), so that a failed write
/// leaves the old file as it was. The new file keeps the mode of the file it replaces, or gets the
/// mode a new file gets (the umask's default), not createNewFile's 0600: core-cpp has no call that
/// creates with a mode, so the mode is set before the rename. Owner and group are not kept. An
/// existing path that is not a regular file (a device such as /dev/null, a FIFO) is written in place,
/// as renaming over it would replace the device with a file. A symbolic link is replaced itself, by a
/// file of the mode of the file it named, which is left untouched: writing through the link would
/// write wherever it points, and not atomically.
class SystemFileAccess final: public IFileAccess
{
  public:
    /// The file system and the standard streams.
    /// @param fileSystem The file system; it must outlive this object.
    /// @param streams The standard streams; they must outlive this object.
    /// @param newFileMode The mode of a file that replaces none: defaultNewFileMode() in the tool.
    SystemFileAccess(core::platform::FileSystem const& fileSystem,
                     StandardStreams streams,
                     std::filesystem::perms newFileMode) noexcept:
        _fileSystem { fileSystem }, _streams { streams }, _newFileMode { newFileMode }
    {
    }

    [[nodiscard]] std::expected<std::vector<std::byte>, FileError> read(std::string_view path,
                                                                        std::size_t maxBytes) override;
    [[nodiscard]] std::expected<void, FileError> write(std::string_view path,
                                                       std::span<std::byte const> bytes) override;
    [[nodiscard]] bool isTerminal(std::string_view path) const override;
    [[nodiscard]] bool isSameFile(std::string_view first, std::string_view second) const override;

  private:
    /// Writes @p bytes over the regular file @p name, or creates it, atomically.
    /// @param name The file.
    /// @param bytes What to write.
    /// @return Nothing, or why it could not be written; then the old file is unchanged.
    [[nodiscard]] std::expected<void, FileError> replace(std::filesystem::path const& name,
                                                         std::span<std::byte const> bytes) const;

    core::platform::FileSystem const& _fileSystem; ///< The file system.
    StandardStreams _streams;                      ///< The standard streams.
    std::filesystem::perms _newFileMode;           ///< The mode of a new file.
};

} // namespace isoline::cli
