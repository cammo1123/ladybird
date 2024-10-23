/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "AK/Assertions.h"
#include "AK/Error.h"
#include <AK/LexicalPath.h>
#include <AK/ScopeGuard.h>
#include <LibCore/DirIterator.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <io.h>
#include <limits.h>
#include <stdlib.h>
#include <windows.h>

#define realpath(N, R) _fullpath((R), (N), _MAX_PATH)

namespace FileSystem {

ErrorOr<ByteString> current_working_directory()
{
    return Core::System::getcwd();
}

ErrorOr<ByteString> absolute_path(StringView path)
{
    if (exists(path))
        return real_path(path);

    if (path.starts_with("/"sv))
        return LexicalPath::canonicalized_path(path);

    auto working_directory = TRY(current_working_directory());
    return LexicalPath::absolute_path(working_directory, path);
}

ErrorOr<ByteString> real_path(StringView path)
{
    if (path.is_null())
        return Error::from_errno(ENOENT);

    ByteString dep_path = path;
    char* real_path = realpath(dep_path.characters(), nullptr);
    ScopeGuard free_path = [real_path]() { free(real_path); };

    if (!real_path)
        return Error::from_syscall("realpath"sv, -errno);

    return ByteString { real_path, strlen(real_path) };
}

bool exists(StringView path)
{
    return !Core::System::stat(path).is_error();
}

bool exists(int fd)
{
    return !Core::System::fstat(fd).is_error();
}

bool is_directory(StringView path)
{
    DWORD attributes = GetFileAttributesA(path.to_byte_string().characters());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return false;
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool is_directory(int fd)
{
    HANDLE hFile = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    BY_HANDLE_FILE_INFORMATION fileInfo;
    if (!GetFileInformationByHandle(hFile, &fileInfo)) {
        return false;
    }

    return (fileInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool is_link(StringView path)
{
    DWORD attributes = GetFileAttributesA(path.to_byte_string().characters());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return false;
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool is_link(int fd)
{
    HANDLE hFile = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    BY_HANDLE_FILE_INFORMATION fileInfo;
    if (!GetFileInformationByHandle(hFile, &fileInfo)) {
        return false;
    }

    return (fileInfo.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

static ErrorOr<ByteString> get_duplicate_file_name(StringView path)
{
    int duplicate_count = 0;
    LexicalPath lexical_path(path);
    auto parent_path = LexicalPath::canonicalized_path(lexical_path.dirname());
    auto basename = lexical_path.basename();
    auto current_name = LexicalPath::join(parent_path, basename).string();

    while (exists(current_name)) {
        ++duplicate_count;
        current_name = LexicalPath::join(parent_path, ByteString::formatted("{} ({})", basename, duplicate_count)).string();
    }

    return current_name;
}

ErrorOr<void> copy_file(StringView destination_path, StringView source_path, struct stat const& source_stat, Core::File& source, PreserveMode preserve_mode)
{
    (void)destination_path;
    (void)source_path;
    (void)source_stat;
    (void)source;
    (void)preserve_mode;
    TODO();
}

ErrorOr<void> copy_directory(StringView destination_path, StringView source_path, struct stat const& source_stat, LinkMode link, PreserveMode preserve_mode)
{
    (void)destination_path;
    (void)source_path;
    (void)source_stat;
    (void)link;
    (void)preserve_mode;
    TODO();
}

ErrorOr<void> copy_file_or_directory(StringView destination_path, StringView source_path, RecursionMode recursion_mode, LinkMode link_mode, AddDuplicateFileMarker add_duplicate_file_marker, PreserveMode preserve_mode)
{
    ByteString final_destination_path;
    if (add_duplicate_file_marker == AddDuplicateFileMarker::Yes)
        final_destination_path = TRY(get_duplicate_file_name(destination_path));
    else
        final_destination_path = destination_path;

    auto source = TRY(Core::File::open(source_path, Core::File::OpenMode::Read));

    auto source_stat = TRY(Core::System::fstat(source->fd()));

    if (is_directory(source_path)) {
        if (recursion_mode == RecursionMode::Disallowed) {
            return Error::from_errno(EISDIR);
        }

        return copy_directory(final_destination_path, source_path, source_stat);
    }

    if (link_mode == LinkMode::Allowed)
        return TRY(Core::System::link(source_path, final_destination_path));

    return copy_file(final_destination_path, source_path, source_stat, *source, preserve_mode);
}

ErrorOr<void> move_file(StringView destination_path, StringView source_path, PreserveMode preserve_mode)
{
    auto maybe_error = Core::System::rename(source_path, destination_path);
    if (!maybe_error.is_error())
        return {};

    if (!maybe_error.error().is_errno() || maybe_error.error().code() != EXDEV)
        return maybe_error;

    auto source = TRY(Core::File::open(source_path, Core::File::OpenMode::Read));

    auto source_stat = TRY(Core::System::fstat(source->fd()));

    TRY(copy_file(destination_path, source_path, source_stat, *source, preserve_mode));

    return Core::System::unlink(source_path);
}

ErrorOr<void> remove(StringView path, RecursionMode mode)
{
    if (is_directory(path) && mode == RecursionMode::Allowed) {
        auto di = Core::DirIterator(path, Core::DirIterator::SkipParentAndBaseDir);
        if (di.has_error())
            return di.error();

        while (di.has_next())
            TRY(remove(di.next_full_path(), RecursionMode::Allowed));

        TRY(Core::System::rmdir(path));
    } else {
        TRY(Core::System::unlink(path));
    }

    return {};
}

ErrorOr<off_t> size_from_stat(StringView path)
{
    auto st = TRY(Core::System::stat(path));
    return st.st_size;
}

ErrorOr<off_t> size_from_fstat(int fd)
{
    auto st = TRY(Core::System::fstat(fd));
    return st.st_size;
}

ErrorOr<off_t> block_device_size_from_ioctl(StringView path)
{
    if (!path.characters_without_null_termination())
        return Error::from_syscall("ioctl"sv, -EFAULT);

    ByteString path_string = path;
    int fd = open(path_string.characters(), O_RDONLY);

    if (fd < 0)
        return Error::from_errno(errno);

    off_t size = TRY(block_device_size_from_ioctl(fd));

    if (close(fd) != 0)
        return Error::from_errno(errno);

    return size;
}

ErrorOr<off_t> block_device_size_from_ioctl(int fd)
{
#if defined(AK_OS_MACOS)
    u64 block_count = 0;
    u32 block_size = 0;
    TRY(Core::System::ioctl(fd, DKIOCGETBLOCKCOUNT, &block_count));
    TRY(Core::System::ioctl(fd, DKIOCGETBLOCKSIZE, &block_size));
    return static_cast<off_t>(block_count * block_size);
#elif defined(AK_OS_FREEBSD) || defined(AK_OS_NETBSD)
    off_t size = 0;
    TRY(Core::System::ioctl(fd, DIOCGMEDIASIZE, &size));
    return size;
#elif defined(AK_OS_LINUX)
    u64 size = 0;
    TRY(Core::System::ioctl(fd, BLKGETSIZE64, &size));
    return static_cast<off_t>(size);
#else
    // FIXME: Add support for more platforms.
    (void)fd;
    return Error::from_string_literal("Platform does not support getting block device size");
#endif
}

bool can_delete_or_move(StringView path)
{
    (void) path;
    TODO();
}

ErrorOr<ByteString> read_link(StringView link_path)
{
    return Core::System::readlink(link_path);
}

ErrorOr<void> link_file(StringView destination_path, StringView source_path)
{
    return TRY(Core::System::symlink(source_path, TRY(get_duplicate_file_name(destination_path))));
}

bool looks_like_shared_library(StringView path)
{
    return path.ends_with(".so"sv) || path.contains(".so."sv);
}

}
