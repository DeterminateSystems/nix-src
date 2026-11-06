#include "nix/util/file-system.hh"
#include "nix/util/file-system-at.hh"
#include "nix/util/signals.hh"
#include "nix/util/util.hh"

#ifdef __APPLE__
#  include <libproc.h> /* For closeExtraFDs. */
#endif
#include <fcntl.h>
#include <unistd.h>

#include <span>
#include <atomic>
#include <vector>

#include "util-unix-config-private.hh"
#include "../file-descriptor-private.hh"

namespace nix {

std::make_unsigned_t<off_t> getFileSize(Descriptor fd)
{
    auto st = nix::fstat(fd);
    return st.st_size;
}

size_t read(Descriptor fd, std::span<std::byte> buffer)
{
    ssize_t n;
    do {
        checkInterrupt();
        n = ::read(fd, buffer.data(), buffer.size());
    } while (n == -1 && errno == EINTR);
    if (n == -1)
        throw SysError("read of %1% bytes", buffer.size());
    return static_cast<size_t>(n);
}

size_t readOffset(Descriptor fd, off_t offset, std::span<std::byte> buffer)
{
    ssize_t n;
    do {
        checkInterrupt();
        n = ::pread(fd, buffer.data(), buffer.size(), offset);
    } while (n == -1 && errno == EINTR);
    if (n == -1)
        throw SysError("pread of %1% bytes at offset %2%", buffer.size(), offset);
    return static_cast<size_t>(n);
}

size_t write(Descriptor fd, std::span<const std::byte> buffer, bool allowInterrupts)
{
    ssize_t n;
    do {
        if (allowInterrupts)
            checkInterrupt();
        n = ::write(fd, buffer.data(), buffer.size());
    } while (n == -1 && errno == EINTR);
    if (n == -1)
        throw SysError("write of %1% bytes", buffer.size());
    return static_cast<size_t>(n);
}

AutoCloseFD dupDescriptor(Descriptor fd)
{
    int newFd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (newFd == -1)
        throw SysError("duplicating file descriptor");
    return AutoCloseFD{newFd};
}

//////////////////////////////////////////////////////////////////////

void Pipe::create(bool nonBlocking)
{
    int fds[2];
#if HAVE_PIPE2
    if (pipe2(fds, O_CLOEXEC | (nonBlocking ? O_NONBLOCK : 0)) != 0)
        throw SysError("creating pipe");
#else
    if (pipe(fds) != 0)
        throw SysError("creating pipe");
    for (auto fd : fds) {
        unix::closeOnExec(fd);
        if (nonBlocking && ::fcntl(fd, F_SETFL, O_NONBLOCK) == -1)
            throw SysError("making pipe non-blocking");
    }
#endif
    readSide = fds[0];
    writeSide = fds[1];
}

//////////////////////////////////////////////////////////////////////

#if defined(__linux__) || defined(__FreeBSD__)
static int unix_close_range(unsigned int first, unsigned int last, int flags)
{
#  if !HAVE_CLOSE_RANGE
    return syscall(SYS_close_range, first, last, (unsigned int) flags);
#  else
    return close_range(first, last, flags);
#  endif
}
#endif

void unix::closeExtraFDs()
{
    constexpr int MAX_KEPT_FD = 2;
    static_assert(std::max({STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) == MAX_KEPT_FD);

    /* The logic here is a bit convoluted and platform-dependent.
     * Keep in mind that this function is free to assume that it's the only thread running
     * in the process (it's usually called right after fork()-ing).
     *
     * - We always keep stdio file descriptors, and close the [MAX_KEPT_FD + 1, ...) range.
     *
     * - Attempt platform-specific ways to close the above:
     *   On Linux (>= 5.9) and FreeBSD, we try close_range syscall.
     *   If that doesn't work, try /proc/self/fd/ iteration.
     *   On Darwin, we use libproc to list all open FDs.
     *
     * - If none of the above works, do the silliest possible thing and attempt
     *   closing all FDs below _SC_OPEN_MAX. Keep in mind that it's huge for the
     *   daemon process, so we really should use something more optimised.
     */

#if defined(__linux__) || defined(__FreeBSD__)
    if (unix_close_range(MAX_KEPT_FD + 1, ~0U, 0) == 0)
        return;
#endif

    /* Platform-specific ways of listing open FDs. */
#if defined(__linux__) || defined(__APPLE__)
    try {
#  ifdef __linux__
        /* Don't close while iterating: the listing includes the FD of the directory itself, and
           closing that would make the next read fail. By the time we do the closing below, that
           FD is already closed by the iterator, so closing it again is a harmless no-op. */
        std::vector<int> openFds;
        for (const auto & s : DirectoryIterator{"/proc/self/fd/"}) {
            auto fd = string2Int<int>(s.path().filename().native());
            if (!fd)
                continue;
            if (*fd > MAX_KEPT_FD)
                openFds.push_back(*fd);
        }
        for (auto fd : openFds)
            ::close(fd); /* ignore result */
#  elifdef __APPLE__
        const auto pid = ::getpid();
        errno = 0;
        /* We are the only thread now, so the needed size shouldn't change. The kernel also leaves a bit
           of slack for the needed space. */
        int bufSize = ::proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
        /* libproc wrapper converts -1 to 0. */
        if (bufSize <= 0 && errno != 0)
            throw SysError("listing open file descriptors");
        std::vector<::proc_fdinfo> fds;
        fds.resize(bufSize / sizeof(::proc_fdinfo));
        errno = 0;
        bufSize = ::proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds.data(), fds.size() * sizeof(::proc_fdinfo));
        if (bufSize <= 0 && errno != 0)
            throw SysError("listing open file descriptors");
        fds.resize(bufSize / sizeof(::proc_fdinfo));
        for (const auto & fdinfo : fds)
            if (fdinfo.proc_fd > MAX_KEPT_FD)
                ::close(fdinfo.proc_fd); /* ignore result */
#  endif
        return;
    } catch (const SystemError & e) {
        /* Try the last resort fallback. */
    }
#endif

    int maxFD = 0;
#if HAVE_SYSCONF
    maxFD = sysconf(_SC_OPEN_MAX);
#endif
    for (int fd = MAX_KEPT_FD + 1; fd < maxFD; ++fd)
        close(fd); /* ignore result */
}

void unix::closeOnExec(int fd)
{
    int prev;
    if ((prev = fcntl(fd, F_GETFD, 0)) == -1 || fcntl(fd, F_SETFD, prev | FD_CLOEXEC) == -1)
        throw SysError("setting close-on-exec flag");
}

void syncDescriptor(Descriptor fd)
{
    int result =
#if defined(__APPLE__)
        ::fcntl(fd, F_FULLFSYNC)
#else
        ::fsync(fd)
#endif
        ;
    if (result == -1)
        throw NativeSysError("fsync file descriptor %1%", fd);
}

void unix::SelfPipe::create()
{
    pipe.create(/*nonBlocking=*/true);
}

void unix::SelfPipe::notify()
{
    /* Write to the self-pipe. If we get EAGAIN that means the notify pipe is full
       and we don't need to do anything. */
    ssize_t res;
    do {
        res = ::write(pipe.writeSide.get(), "x", 1);
    } while (res == -1 && errno == EINTR);
    if (res == -1 && errno != EAGAIN)
        throw SysError("writing to the self-pipe");
}

void unix::SelfPipe::drain()
{
    /* Drain the self-pipe. */
    std::array<char, 128> buf;
    while (true) {
        if (::read(pipe.readSide.get(), buf.data(), buf.size()) == -1) {
            if (errno == EAGAIN)
                break;
            else if (errno == EINTR)
                continue;
            else
                throw SysError("reading from self-pipe");
        }
    }
}

bool tryCopyFdRangeFast(Descriptor from, Descriptor to, off_t offset, size_t nbytes, size_t & written)
{
#if HAVE_COPY_FILE_RANGE

    /* Otherwise trying block cloning is pretty pointless. Also used to error
       out on partial copies (unless we happen to hit block alignment boundary,
       but that seems unlikely to be useful). */
    assert(written == 0);
    size_t left = nbytes;
    static std::atomic_flag copyFileRangeUnsupported = {};

    if (copyFileRangeUnsupported.test(std::memory_order_relaxed))
        return false;

    while (left) {
        checkInterrupt();

        ssize_t n = ::copy_file_range(
            from,
            &offset,
            to,
            nullptr,
            left,
            0
#  ifdef COPY_FILE_RANGE_CLONE /* FreeBSD */
                | COPY_FILE_RANGE_CLONE
#  endif
        );

        /* Reached EOF too early. */
        if (n == 0)
            throw EndOfFile(
                "unexpected end-of-file copying from %1% to %2%",
                PathFmt(descriptorToPath(from)),
                PathFmt(descriptorToPath(to)));

        if (n == -1) {
            if (errno == EINTR)
                continue; /* Loop over to hit checkInterrupt. */

            if (written == 0 && (errno == EINVAL || errno == EOPNOTSUPP || errno == EXDEV || errno == EBADF)) {
                /* Retry with fallback. */
            } else if (written == 0 && errno == ENOSYS) {
                /* Cache ENOSYS. */
                copyFileRangeUnsupported.test_and_set();
            } else {
                throw SysError([&] {
                    return HintFmt(
                        "copying contents of %1% to %2% via copy_file_range",
                        PathFmt(descriptorToPath(from)),
                        PathFmt(descriptorToPath(to)));
                });
            }

            return false;
        }

        assert(static_cast<uint64_t>(n) <= left);
        left -= n;
        written += n;
    }

    return true;
#else
    return false;
#endif
}

} // namespace nix
