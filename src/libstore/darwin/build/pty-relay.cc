#include "pty-relay.hh"
#include "nix/util/error.hh"
#include "nix/util/finally.hh"
#include "nix/util/util.hh"

#include <atomic>
#include <chrono>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace nix {

namespace {

struct Relay
{
    /**
     * A file descriptor for the slave side that we keep open while
     * we cannot read the master side, or -1.
     */
    int slave = -1;

    AutoCloseFD master, out, cancel;

    std::string slaveName;

    /**
     * Output that has been read from `master` but not yet written to
     * `out`.
     */
    std::string pending;

    /**
     * A soft limit on the size of `pending`: we stop reading the
     * master side when it's reached, so `pending` can hold up to
     * 64 KiB plus what one read returns.
     */
    static constexpr size_t maxPending = 64 * 1024;

    /**
     * Whether we got EOF on `master`.
     */
    bool eof = false;

    ~Relay()
    {
        /* Close the master side first, so that closing the slave
           side doesn't wait for it to be read. */
        try {
            master.close();
        } catch (...) {
            ignoreExceptionInDestructor();
        }
        if (slave != -1)
            ::close(slave);
    }

    enum struct Wait { Ready, Timeout, Cancelled };

    /**
     * Wait for `events` on `fd`.
     */
    Wait wait(int fd, short events, int timeout)
    {
        struct pollfd fds[2] = {
            {.fd = fd, .events = events, .revents = 0},
            {.fd = cancel.get(), .events = POLLIN, .revents = 0},
        };
        while (poll(fds, 2, timeout) == -1)
            if (errno != EINTR && errno != EAGAIN)
                throw SysError("waiting for the output of a builder");
        if (fds[1].revents)
            return Wait::Cancelled;
        return fds[0].revents ? Wait::Ready : Wait::Timeout;
    }

    /**
     * Read from `master`, which must be ready for that.
     */
    void readMaster()
    {
        char buf[4096];
        auto n = ::read(master.get(), buf, sizeof(buf));
        if (n > 0)
            pending.append(buf, n);
        else if (n == 0 || (errno != EINTR && errno != EAGAIN))
            /* The master side fails with EIO when the slave side
               has been closed. */
            eof = true;
    }

    /**
     * Call `fn`, which opens or closes the slave side, on another
     * thread and keep reading the master side until it returns.
     *
     * Opening the slave side blocks while another process is in its
     * last close of the slave side, and closing it blocks if that is
     * the last close. Both wait at most 0.6 s for the master side to
     * be read and then discard the output that is still queued, so
     * the master side has to be read in the meantime. No other
     * process has the slave side open in that case, so there is at
     * most as much to read as fits in the queue of the
     * pseudoterminal.
     *
     * @return false if we have been cancelled.
     */
    bool whileReading(auto && fn)
    {
        std::atomic_bool done{false};
        std::thread helper([&]() {
            fn();
            done = true;
        });
        Finally joinHelper([&]() { helper.join(); });

        bool cancelled = false;
        while (!done) {
            if (!cancelled && !eof && pending.size() < maxPending) {
                auto res = wait(master.get(), POLLIN, 1);
                if (res == Wait::Ready)
                    readMaster();
                else if (res == Wait::Cancelled)
                    cancelled = true;
            } else
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        return !cancelled;
    }

    void run()
    {
        while (true) {
            if (!pending.empty()) {
                auto n = ::write(out.get(), pending.data(), pending.size());
                if (n >= 0)
                    pending.erase(0, n);
                else if (errno == EAGAIN) {
                    /* The pipe is full, so we have to stop reading
                       the master side until the worker has read
                       from the pipe. Until then, a builder that
                       writes more output blocks, as it would if the
                       worker read the master side itself. But if it
                       exits instead, its last output must not be
                       discarded, so keep the slave side open
                       ourselves. */
                    if (slave == -1) {
                        int error = 0;
                        if (!whileReading([&]() {
                                slave = open(slaveName.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
                                error = errno;
                            }))
                            return;
                        if (slave == -1)
                            throw SysError(error, "opening pseudoterminal slave");
                    }
                    if (wait(out.get(), POLLOUT, -1) == Wait::Cancelled)
                        return;
                } else if (errno != EINTR)
                    /* The read side has been closed. */
                    return;
                continue;
            }

            if (eof)
                return;

            if (slave != -1) {
                /* We can read the master side again. Close the slave
                   side once there is nothing to read, since the
                   master side doesn't get EOF while we have it
                   open. */
                auto res = wait(master.get(), POLLIN, 0);
                if (res == Wait::Cancelled)
                    return;
                if (res == Wait::Ready)
                    readMaster();
                else if (!whileReading([&]() {
                             ::close(slave);
                             slave = -1;
                         }))
                    return;
                continue;
            }

            if (wait(master.get(), POLLIN, -1) == Wait::Cancelled)
                return;
            readMaster();
        }
    }
};

} // namespace

PtyRelay::PtyRelay(AutoCloseFD master, std::string slaveName)
{
    Pipe outPipe, cancelPipe;
    outPipe.create();
    cancelPipe.create();

    if (fcntl(outPipe.writeSide.get(), F_SETFL, O_NONBLOCK) == -1)
        throw SysError("making pipe non-blocking");
    if (fcntl(outPipe.writeSide.get(), F_SETNOSIGPIPE, 1) == -1)
        throw SysError("disabling SIGPIPE on pipe");

    out = std::move(outPipe.readSide);
    cancel = std::move(cancelPipe.writeSide);

    auto relay = std::make_unique<Relay>();
    relay->master = std::move(master);
    relay->out = std::move(outPipe.writeSide);
    relay->cancel = std::move(cancelPipe.readSide);
    relay->slaveName = std::move(slaveName);

    thread = std::thread([this, relay{std::move(relay)}]() mutable {
        try {
            relay->run();
        } catch (...) {
            error = std::current_exception();
        }
        /* This closes the pipe, so the worker gets EOF. */
        relay.reset();
    });
}

void PtyRelay::stop()
{
    if (cancel) {
        /* Closing the pipe is not enough, since a child process that
           we forked may still have a copy of this file
           descriptor. */
        char c = 0;
        [[maybe_unused]] auto n = ::write(cancel.get(), &c, 1);
        cancel.close();
    }

    if (thread.joinable())
        thread.join();
}

void PtyRelay::finish()
{
    stop();

    if (auto e = std::exchange(error, nullptr))
        std::rethrow_exception(e);
}

PtyRelay::~PtyRelay()
{
    try {
        stop();
    } catch (...) {
        ignoreExceptionInDestructor();
    }
}

} // namespace nix
