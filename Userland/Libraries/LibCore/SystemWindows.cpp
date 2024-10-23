/*
 * Copyright (c) 2021-2022, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021-2022, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Matthias Zimmerman <matthias291999@gmail.com>
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2024, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/ScopeGuard.h>
#include <AK/String.h>
#include <LibCore/System.h>
#include <WinSock2.h>
#include <io.h>

namespace Core::System {

ErrorOr<int> open(StringView path, int options, mode_t mode)
{
    ByteString string_path = path;
    int rc = _open(string_path.characters(), options, mode);
    if (rc < 0)
        return Error::from_syscall("open"sv, -errno);
    return rc;
}

ErrorOr<void> close(int fd)
{
    if (_close(fd) < 0)
        return Error::from_syscall("close"sv, -errno);
    return {};
}

ErrorOr<ssize_t> read(int fd, Bytes buffer)
{
    int rc = _read(fd, buffer.data(), buffer.size());
    if (rc < 0)
        return Error::from_syscall("read"sv, -errno);
    return rc;
}

ErrorOr<ssize_t> write(int fd, ReadonlyBytes buffer)
{
    int rc = _write(fd, buffer.data(), buffer.size());
    if (rc < 0)
        return Error::from_syscall("write"sv, -errno);
    return rc;
}

ErrorOr<off_t> lseek(int fd, off_t offset, int whence)
{
    long rc = _lseek(fd, offset, whence);
    if (rc < 0)
        return Error::from_syscall("lseek"sv, -errno);
    return rc;
}

ErrorOr<void> ftruncate(int fd, off_t length)
{
    long position = _tell(fd);
    if (position == -1)
        return Error::from_errno(errno);

    ScopeGuard restore_position { [&] { _lseek(fd, position, SEEK_SET); } };

    auto result = lseek(fd, length, SEEK_SET);
    if (result.is_error())
        return result.release_error();

    if (SetEndOfFile((HANDLE)_get_osfhandle(fd)) == 0)
        return Error::from_windows_error(GetLastError());
    return {};
}

ErrorOr<struct stat> fstat(int fd)
{
    struct stat st = {};
    if (::fstat(fd, &st) < 0)
        return Error::from_syscall("fstat"sv, -errno);
    return st;
}

ErrorOr<void> ioctl(int, unsigned, ...)
{
    dbgln("Core::System::ioctl() is not implemented");
    VERIFY_NOT_REACHED();
}

ErrorOr<void> mkdir(StringView path, mode_t mode)
{
    (void)mode;
    int res = CreateDirectory(path.to_byte_string().characters(), {});
    if (res < 0)
        return Error::from_windows_error(GetLastError());
    return {};
}

ErrorOr<int> openat(int fd, StringView path, int options, mode_t mode)
{
    (void)fd;
    (void)path;
    (void)options;
    (void)mode;
    TODO();
}

ErrorOr<struct stat> fstatat(int fd, StringView path, int flags)
{
    (void)fd;
    (void)path;
    (void)flags;
    TODO();
}

ErrorOr<int> mkstemp(Span<char> pattern)
{
    (void)pattern;
    TODO();
}

ErrorOr<String> mkdtemp(Span<char> pattern)
{
    (void)pattern;
    TODO();
}

ErrorOr<ByteString> getcwd()
{
    TODO();
}

ErrorOr<struct stat> stat(StringView path)
{
    if (!path.characters_without_null_termination())
        return Error::from_syscall("stat"sv, -EFAULT);

    struct stat st = {};
    ByteString path_string = path;
    if (::stat(path_string.characters(), &st) < 0)
        return Error::from_syscall("stat"sv, -errno);
    return st;
}

ErrorOr<void> link(StringView old_path, StringView new_path)
{
    (void)old_path;
    (void)new_path;
    TODO();
}

ErrorOr<void> symlink(StringView target, StringView link_path)
{
    (void)target;
    (void)link_path;
    TODO();
}

ErrorOr<void> rename(StringView old_path, StringView new_path)
{
    (void)old_path;
    (void)new_path;
    TODO();
}

ErrorOr<void> unlink(StringView path)
{
    (void)path;
    TODO();
}

ErrorOr<void> rmdir(StringView path)
{
    (void)path;
    TODO();
}

ErrorOr<ByteString> readlink(StringView pathname)
{
    (void)pathname;
    TODO();
}

}
