#include "nix/util/spawner.hh"
#include "nix/util/current-process.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/file-descriptor.hh"
#include "nix/util/finally.hh"
#include "nix/util/logging.hh"
#include "nix/util/signals.hh"
#include "nix/util/strings.hh"
#include "nix/util/sync.hh"
#include "nix/util/unix-domain-socket.hh"
#include "nix/util/util.hh"

#include <atomic>
#include <future>
#include <map>
#include <thread>

#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __linux__
#  include <sched.h>
#endif

using namespace std::chrono_literals;

namespace nix {

void SpawnerError::anchor() {}

/* Bump this when changing the protocol below. */
static constexpr uint64_t spawnerProtocolVersion = 1;

/* The file descriptor number of the control socket in the spawner. */
static constexpr int spawnerControlFd = 3;

static constexpr std::string_view spawnerFdEnvVar = "NIX_SPAWNER_FD";

enum class MessageType : uint64_t {
    /* Spawner -> parent. */
    Hello = 1,
    Spawned = 2,
    SpawnFailed = 3,
    Exited = 4,
    /* Parent -> spawner. */
    Spawn = 5,
    Release = 6,
};

struct MessageHeader
{
    uint64_t type;
    uint64_t nrFds;
    uint64_t payloadSize;
};

struct Message
{
    MessageType type;
    std::string payload;
    std::vector<AutoCloseFD> fds;
};

static void sendMessage(Descriptor fd, MessageType type, std::string_view payload, std::span<const Descriptor> fds = {})
{
    MessageHeader header{
        .type = (uint64_t) type,
        .nrFds = fds.size(),
        .payloadSize = payload.size(),
    };
    std::string buf;
    buf.reserve(sizeof(header) + payload.size());
    buf.append((const char *) &header, sizeof(header));
    buf.append(payload);
    unix::sendMessageWithFds(fd, std::as_bytes(std::span(buf)), fds);
}

/* Receive a message. Note that we don't use a buffered source on the
   socket, since that could read (and thus drop) the file descriptors
   of the next message. */
static Message receiveMessage(Descriptor fd)
{
    MessageHeader header;
    auto res = unix::receiveMessageWithFds(fd, std::as_writable_bytes(std::span(&header, 1)));
    if (res.bytesReceived < sizeof(header))
        readFull(fd, (char *) &header + res.bytesReceived, sizeof(header) - res.bytesReceived);
    if (res.fds.size() != header.nrFds)
        throw SpawnerError(
            "spawner message has %d file descriptors, expected %d", res.fds.size(), (uint64_t) header.nrFds);
    Message msg{
        .type = (MessageType) header.type,
        .fds = std::move(res.fds),
    };
    msg.payload.resize(header.payloadSize);
    readFull(fd, msg.payload.data(), msg.payload.size());
    return msg;
}

static std::map<std::string, RegisterSpawnOp::Main> & spawnOps()
{
    static std::map<std::string, RegisterSpawnOp::Main> ops;
    return ops;
}

static Strings spawnOpNames()
{
    Strings names;
    for (auto & [name, _] : spawnOps())
        names.push_back(name);
    return names;
}

RegisterSpawnOp::RegisterSpawnOp(std::string name, Main main)
    : name(name)
{
    auto [_, inserted] = spawnOps().emplace(std::move(name), std::move(main));
    assert(inserted);
}

static void writeSpawnOptions(Sink & sink, const SpawnOptions & options)
{
    sink << (uint64_t) options.dieWithParent << (uint64_t) options.setSid
         << (options.chdir ? options.chdir->string() : "") << (uint64_t) options.linuxCloneFlags
         << (uint64_t) (options.cgroupFd != INVALID_DESCRIPTOR) << (uint64_t) options.dropSupplementaryGroups
         << (uint64_t) options.requireDropSupplementaryGroups << options.errorPrefix;
}

/**
 * The options as received by the spawner. `cgroupFd` is passed as the
 * last file descriptor of the message.
 */
struct ReceivedSpawnOptions : SpawnOptions
{
    bool hasCgroupFd = false;
};

static ReceivedSpawnOptions readSpawnOptions(Source & source)
{
    ReceivedSpawnOptions options;
    options.dieWithParent = readNum<uint64_t>(source);
    options.setSid = readNum<uint64_t>(source);
    if (auto chdir = readString(source); !chdir.empty())
        options.chdir = chdir;
    options.linuxCloneFlags = readNum<int>(source);
    options.hasCgroupFd = readNum<uint64_t>(source);
    options.dropSupplementaryGroups = readNum<uint64_t>(source);
    options.requireDropSupplementaryGroups = readNum<uint64_t>(source);
    options.errorPrefix = readString(source);
    return options;
}

//////////////////////////////////////////////////////////////////////
// The spawner process.

static void dropSupplementaryGroups(bool require)
{
    if (setgroups(0, 0) == -1) {
        if (errno != EPERM || require)
            throw SysError("setgroups failed");
    }
}

/* Convert the result of waitid() to a status as returned by
   waitpid(). */
static int statusFromSigInfo(const siginfo_t & info)
{
    switch (info.si_code) {
    case CLD_EXITED:
        return (info.si_status & 0xff) << 8;
    case CLD_DUMPED:
        return (info.si_status & 0x7f) | 0x80;
    default:
        return info.si_status & 0x7f;
    }
}

static int sigChldWriteFd = -1;

static void sigChldHandler(int sigNo)
{
    int savedErrno = errno;
    [[maybe_unused]] auto res = ::write(sigChldWriteFd, "x", 1);
    errno = savedErrno;
}

namespace {

struct Server
{
    AutoCloseFD control;

    Pipe sigChld;

    struct Child
    {
        bool dieWithParent;

        /**
         * Whether the parent has detached the child, i.e. we should
         * reap it ourselves when it exits.
         */
        bool detached = false;

        /**
         * Whether the child has exited and we've sent its status to
         * the parent. It remains a zombie until the parent releases
         * it.
         */
        bool exited = false;
    };

    std::map<pid_t, Child> children;

    [[noreturn]] void run();

    void handleSpawn(Message & msg);

    pid_t startChild(const ReceivedSpawnOptions & options, Descriptor cgroupFd, fun<void()> childMain);

    void handleRelease(Message & msg);

    void reapChildren();

    void reap(pid_t pid);

    [[noreturn]] void shutdown();
};

void Server::run()
{
    sigChld.create(true);
    sigChldWriteFd = sigChld.writeSide.get();

    struct sigaction act;
    act.sa_handler = sigChldHandler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &act, nullptr))
        throw SysError("setting SIGCHLD handler");

    StringSink hello;
    hello << spawnerProtocolVersion << spawnOpNames();
    sendMessage(control.get(), MessageType::Hello, hello.s);

    while (true) {
        struct pollfd fds[2] = {
            {.fd = control.get(), .events = POLLIN, .revents = 0},
            {.fd = sigChld.readSide.get(), .events = POLLIN, .revents = 0},
        };

        if (poll(fds, 2, -1) == -1) {
            if (errno == EINTR)
                continue;
            throw SysError("polling in the spawner");
        }

        if (fds[1].revents) {
            char buf[64];
            while (::read(sigChld.readSide.get(), buf, sizeof(buf)) > 0)
                ;
            reapChildren();
        }

        if (fds[0].revents) {
            Message msg;
            try {
                msg = receiveMessage(control.get());
            } catch (Error &) {
                /* The parent has gone away. */
                shutdown();
            }

            switch (msg.type) {
            case MessageType::Spawn:
                handleSpawn(msg);
                break;
            case MessageType::Release:
                handleRelease(msg);
                break;
            case MessageType::Hello:
            case MessageType::Spawned:
            case MessageType::SpawnFailed:
            case MessageType::Exited:
            default:
                throw SpawnerError("unexpected message type %d received by the spawner", (uint64_t) msg.type);
            }
        }
    }
}

void Server::handleSpawn(Message & msg)
{
    StringSource source(msg.payload);
    auto requestId = readNum<uint64_t>(source);

    try {
        auto name = readString(source);
        auto childVerbosity = (Verbosity) readNum<unsigned int>(source);
        auto options = readSpawnOptions(source);
        auto args = readString(source);

        auto op = spawnOps().find(name);
        if (op == spawnOps().end())
            throw SpawnerError("unknown spawner operation '%s'", name);

        AutoCloseFD cgroupFd;
        if (options.hasCgroupFd) {
            if (msg.fds.empty())
                throw SpawnerError("missing cgroup file descriptor");
            cgroupFd = std::move(msg.fds.back());
            msg.fds.pop_back();
        }

        auto pid = startChild(options, cgroupFd.get(), [&]() {
            /* Undo the spawner's state. */
            signal(SIGCHLD, SIG_DFL);
            control.close();
            sigChld.readSide.close();
            sigChld.writeSide.close();
            cgroupFd.close();

            verbosity = childVerbosity;

            if (options.setSid && setsid() == -1)
                throw SysError("creating a new session");

            if (options.chdir && chdir(options.chdir->c_str()) == -1)
                throw SysError("changing directory to %s", PathFmt(*options.chdir));

            StringSource argsSource(args);
            op->second(argsSource, std::move(msg.fds));

            _exit(0);
        });

        children.emplace(pid, Child{.dieWithParent = options.dieWithParent});

        StringSink reply;
        reply << requestId << (uint64_t) pid;
        sendMessage(control.get(), MessageType::Spawned, reply.s);
    } catch (Error & e) {
        StringSink reply;
        reply << requestId << e;
        sendMessage(control.get(), MessageType::SpawnFailed, reply.s);
    }
}

pid_t Server::startChild(const ReceivedSpawnOptions & options, Descriptor cgroupFd, fun<void()> childMain)
{
    ProcessOptions processOptions;
    processOptions.errorPrefix = options.errorPrefix;
    processOptions.dieWithParent = options.dieWithParent;
    processOptions.cloneFlags = options.linuxCloneFlags;
    processOptions.cgroupFd = cgroupFd;

#ifdef __linux__
    /* Supplementary groups can't be dropped inside a new user
       namespace, but we don't want to drop our own. So drop them in
       an intermediate process that creates the child on our behalf
       (as a child of ours, using CLONE_PARENT). */
    if (options.dropSupplementaryGroups && (options.linuxCloneFlags & CLONE_NEWUSER)) {
        Pipe toSpawner;
        toSpawner.create();

        processOptions.cloneFlags |= CLONE_PARENT;

        Pid intermediate(startProcess([&]() {
            toSpawner.readSide.close();
            try {
                dropSupplementaryGroups(options.requireDropSupplementaryGroups);
                pid_t child = startProcess(
                    [&]() {
                        toSpawner.writeSide.close();
                        childMain();
                    },
                    processOptions);
                writeFull(toSpawner.writeSide.get(), fmt("%d", child));
                _exit(0);
            } catch (Error & e) {
                writeFull(toSpawner.writeSide.get(), e.msg());
                _exit(1);
            }
        }));

        toSpawner.writeSide.close();

        auto output = drainFD(toSpawner.readSide.get());

        if (auto status = intermediate.wait(); !statusOk(status)) {
            if (!output.empty())
                throw SpawnerError(output);
            throw SpawnerError("unable to start child process: %s", statusToString(status));
        }

        auto pid = string2Int<pid_t>(output);
        if (!pid)
            throw SpawnerError("got invalid PID '%s' from the intermediate process", output);
        return *pid;
    }
#endif

    return startProcess(
        [&]() {
            if (options.dropSupplementaryGroups)
                dropSupplementaryGroups(options.requireDropSupplementaryGroups);
            childMain();
        },
        processOptions);
}

void Server::handleRelease(Message & msg)
{
    StringSource source(msg.payload);
    auto pid = readNum<pid_t>(source);

    auto i = children.find(pid);
    if (i == children.end())
        return;

    if (i->second.exited) {
        reap(pid);
        children.erase(i);
    } else
        i->second.detached = true;
}

void Server::reapChildren()
{
    for (auto i = children.begin(); i != children.end();) {
        auto & [pid, child] = *i;

        if (child.exited) {
            ++i;
            continue;
        }

        /* Note: we use WNOWAIT to leave the child as a zombie, so
           that its PID doesn't get reused until the parent has
           released it. */
        siginfo_t info;
        info.si_pid = 0;
        if (waitid(P_PID, pid, &info, WEXITED | WNOHANG | WNOWAIT) == -1) {
            if (errno == EINTR)
                continue;
            throw SysError("waiting for PID %d", pid);
        }

        if (info.si_pid != pid) {
            ++i;
            continue;
        }

        if (child.detached) {
            reap(pid);
            i = children.erase(i);
            continue;
        }

        child.exited = true;

        StringSink msg;
        msg << (uint64_t) pid << (uint64_t) statusFromSigInfo(info);
        sendMessage(control.get(), MessageType::Exited, msg.s);

        ++i;
    }
}

void Server::reap(pid_t pid)
{
    while (waitpid(pid, nullptr, 0) == -1 && errno == EINTR)
        ;
}

void Server::shutdown()
{
    for (auto & [pid, child] : children)
        if (child.dieWithParent && !child.exited) {
            ::kill(-pid, SIGKILL);
            ::kill(pid, SIGKILL);
        }
    _exit(0);
}

} // namespace

static std::atomic<bool> spawnerCapable{false};

void runSpawnerIfRequested()
{
    spawnerCapable = true;

    auto fdStr = getEnv(std::string(spawnerFdEnvVar));
    if (!fdStr)
        return;

    try {
        unsetenv(std::string(spawnerFdEnvVar).c_str());

        if (string2Int<int>(*fdStr) != spawnerControlFd)
            throw SpawnerError("invalid value '%s' for %s", *fdStr, spawnerFdEnvVar);

        /* Don't keep any file descriptors of the parent other than
           stdio and the control socket. */
        unix::closeFDsFrom(spawnerControlFd + 1);

        Server server;
        server.control = AutoCloseFD(spawnerControlFd);
        server.run();
    } catch (std::exception & e) {
        try {
            writeFull(STDERR_FILENO, fmt("nix spawner: %s\n", e.what()), false);
        } catch (...) {
        }
    }

    _exit(1);
}

//////////////////////////////////////////////////////////////////////
// The parent side.

struct SpawnerConnection;

struct SpawnerChild
{
    const pid_t pid;

    const std::shared_ptr<SpawnerConnection> connection;

    struct State
    {
        std::optional<int> status;

        /**
         * Whether the spawner died before sending the exit status.
         */
        bool connectionLost = false;
    };

    Sync<State> state;

    std::condition_variable cv;

    SpawnerChild(pid_t pid, std::shared_ptr<SpawnerConnection> connection)
        : pid(pid)
        , connection(std::move(connection))
    {
    }
};

struct SpawnerConnection : std::enable_shared_from_this<SpawnerConnection>
{
    /**
     * The process that started the spawner.
     */
    const pid_t creator = getpid();

    std::string program;

    AutoCloseFD fd;

    /**
     * The spawner process.
     */
    Pid process;

    struct State
    {
        /**
         * Whether the connection to the spawner has been lost, and
         * the reason.
         */
        std::optional<std::string> error;

        uint64_t nextRequestId = 1;

        std::map<uint64_t, std::promise<std::shared_ptr<SpawnerChild>>> pendingSpawns;

        std::map<pid_t, std::weak_ptr<SpawnerChild>> children;
    };

    Sync<State> state;

    std::mutex sendMutex;

    void send(MessageType type, std::string_view payload, std::span<const Descriptor> fds = {})
    {
        std::lock_guard lock(sendMutex);
        sendMessage(fd.get(), type, payload, fds);
    }

    /**
     * Tell the spawner that we no longer need the child with the
     * specified PID, i.e. that it can reap it.
     */
    void release(pid_t pid)
    {
        {
            auto state(this->state.lock());
            state->children.erase(pid);
            if (state->error)
                return;
        }
        try {
            StringSink msg;
            msg << (uint64_t) pid;
            send(MessageType::Release, msg.s);
        } catch (Error &) {
            /* The spawner presumably died. */
        }
    }

    void readerLoop();
};

void SpawnerConnection::readerLoop()
{
    std::string error;

    try {
        while (true) {
            auto msg = receiveMessage(fd.get());
            StringSource source(msg.payload);

            switch (msg.type) {

            case MessageType::Spawned: {
                auto requestId = readNum<uint64_t>(source);
                auto pid = readNum<pid_t>(source);
                auto child = std::make_shared<SpawnerChild>(pid, shared_from_this());
                auto state(this->state.lock());
                state->children.insert_or_assign(pid, child);
                auto i = state->pendingSpawns.find(requestId);
                if (i == state->pendingSpawns.end())
                    throw SpawnerError("spawner sent reply to unknown request %d", requestId);
                i->second.set_value(child);
                state->pendingSpawns.erase(i);
                break;
            }

            case MessageType::SpawnFailed: {
                auto requestId = readNum<uint64_t>(source);
                auto e = readError(source);
                auto state(this->state.lock());
                auto i = state->pendingSpawns.find(requestId);
                if (i == state->pendingSpawns.end())
                    throw SpawnerError("spawner sent reply to unknown request %d", requestId);
                i->second.set_exception(std::make_exception_ptr(e));
                state->pendingSpawns.erase(i);
                break;
            }

            case MessageType::Exited: {
                auto pid = readNum<pid_t>(source);
                auto status = readNum<int>(source);
                auto state(this->state.lock());
                auto i = state->children.find(pid);
                if (i == state->children.end())
                    break;
                if (auto child = i->second.lock()) {
                    child->state.lock()->status = status;
                    child->cv.notify_all();
                }
                break;
            }

            case MessageType::Hello:
            case MessageType::Spawn:
            case MessageType::Release:
            default:
                throw SpawnerError("unexpected message type %d received from the spawner", (uint64_t) msg.type);
            }
        }
    } catch (EndOfFile &) {
        error = "the spawner process exited unexpectedly";
    } catch (std::exception & e) {
        error = fmt("error communicating with the spawner process: %s", e.what());
    }

    try {
        error += fmt(" (%s)", statusToString(process.wait()));
    } catch (...) {
    }

    auto state(this->state.lock());
    state->error = error;
    for (auto & [_, promise] : state->pendingSpawns)
        promise.set_exception(std::make_exception_ptr(SpawnerError(error)));
    state->pendingSpawns.clear();
    for (auto & [_, weakChild] : state->children)
        if (auto child = weakChild.lock()) {
            child->state.lock()->connectionLost = true;
            child->cv.notify_all();
        }
}

/* The current connection to the spawner. */
static Sync<std::shared_ptr<SpawnerConnection>> currentSpawner;

static std::shared_ptr<SpawnerConnection> startSpawner()
{
    auto conn = std::make_shared<SpawnerConnection>();

#ifdef __linux__
    /* Use /proc/self/exe to make sure we execute exactly the same
       executable, even if it has been replaced on disk. */
    conn->program = "/proc/self/exe";
#else
    auto selfExe = getSelfExe();
    if (!selfExe)
        throw SpawnerError("cannot determine the path of the current executable to start the spawner");
    conn->program = selfExe->string();
#endif

    int fds[2];
#ifdef SOCK_CLOEXEC
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == -1)
#else
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == -1)
#endif
        throw SysError("creating the spawner socket pair");
    conn->fd = AutoCloseFD(fds[0]);
    AutoCloseFD theirSide(fds[1]);
#ifndef SOCK_CLOEXEC
    unix::closeOnExec(conn->fd.get());
    unix::closeOnExec(theirSide.get());
#endif
#ifdef SO_NOSIGPIPE
    for (auto fd : fds) {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    }
#endif

    /* A dup2() onto the same file descriptor wouldn't clear
       FD_CLOEXEC. */
    if (theirSide.get() == spawnerControlFd) {
        theirSide = AutoCloseFD(fcntl(theirSide.get(), F_DUPFD_CLOEXEC, spawnerControlFd + 1));
        if (!theirSide)
            throw SysError("duplicating the spawner socket");
    }

    posix_spawn_file_actions_t actions;
    if (int err = posix_spawn_file_actions_init(&actions))
        throw SysError(err, "initialising spawn file actions");
    Finally destroyActions([&]() { posix_spawn_file_actions_destroy(&actions); });

    if (int err = posix_spawn_file_actions_adddup2(&actions, theirSide.get(), spawnerControlFd))
        throw SysError(err, "setting spawn file actions");
    if (int err = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0))
        throw SysError(err, "setting spawn file actions");
    if (int err = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0))
        throw SysError(err, "setting spawn file actions");

    posix_spawnattr_t attr;
    if (int err = posix_spawnattr_init(&attr))
        throw SysError(err, "initialising spawn attributes");
    Finally destroyAttr([&]() { posix_spawnattr_destroy(&attr); });

    /* Don't inherit our signal mask or ignored signals. */
    sigset_t noSignals, allSignals;
    sigemptyset(&noSignals);
    sigfillset(&allSignals);
    if (int err = posix_spawnattr_setsigmask(&attr, &noSignals))
        throw SysError(err, "setting spawn signal mask");
    if (int err = posix_spawnattr_setsigdefault(&attr, &allSignals))
        throw SysError(err, "setting spawn default signals");
    if (int err = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF))
        throw SysError(err, "setting spawn flags");

    auto env = getEnv();
    env.insert_or_assign(std::string(spawnerFdEnvVar), std::to_string(spawnerControlFd));
    Strings envStrs;
    for (auto & [name, value] : env)
        envStrs.push_back(name + "=" + value);

    Strings args = {"nix-spawner"};

    pid_t pid;
    if (int err = posix_spawn(
            &pid,
            conn->program.c_str(),
            &actions,
            &attr,
            stringsToCharPtrs(args).data(),
            stringsToCharPtrs(envStrs).data()))
        throw SysError(err, "starting the spawner process %s", conn->program);
    conn->process = pid;

    theirSide.close();

    try {
        auto msg = receiveMessage(conn->fd.get());
        if (msg.type != MessageType::Hello)
            throw SpawnerError("unexpected message type %d from the spawner", (uint64_t) msg.type);
        StringSource source(msg.payload);
        auto version = readNum<uint64_t>(source);
        if (version != spawnerProtocolVersion)
            throw SpawnerError(
                "the spawner has protocol version %d, but this process has version %d",
                version,
                spawnerProtocolVersion);
        if (readStrings<Strings>(source) != spawnOpNames())
            throw SpawnerError("the spawner has different operations than this process");
    } catch (EndOfFile &) {
        throw SpawnerError(
            "the spawner process %s failed to start (%s)", conn->program, statusToString(conn->process.wait()));
    } catch (Error & e) {
        e.addTrace({}, "while starting the spawner process %s", conn->program);
        throw;
    }

    std::thread([conn]() { conn->readerLoop(); }).detach();

    return conn;
}

static std::shared_ptr<SpawnerConnection> getSpawner()
{
    auto spawner(currentSpawner.lock());

    if (*spawner) {
        /* If we're a forked child of the process that started the
           spawner, we can't use the connection (its reader thread
           doesn't exist here). Neither can we destroy it, since
           that would kill the spawner of our parent, so leak it. */
        if ((*spawner)->creator != getpid())
            (void) new std::shared_ptr<SpawnerConnection>(std::move(*spawner));
        else if (!(*spawner)->state.lock()->error)
            return *spawner;
    }

    if (!spawnerCapable)
        throw SpawnerError(
            "this program cannot start the Nix spawner process; it must call 'nix::runSpawnerIfRequested()' at the start of 'main()'");

    *spawner = startSpawner();
    return *spawner;
}

Pid RegisterSpawnOp::spawn(
    fun<void(Sink & args)> writeArgs, std::span<const Descriptor> fds, const SpawnOptions & options) const
{
    StringSink args;
    writeArgs(args);

    auto spawner = getSpawner();

    uint64_t requestId;
    std::future<std::shared_ptr<SpawnerChild>> future;
    {
        auto state(spawner->state.lock());
        if (state->error)
            throw SpawnerError(*state->error);
        requestId = state->nextRequestId++;
        future = state->pendingSpawns[requestId].get_future();
    }

    StringSink payload;
    payload << requestId << name << (uint64_t) verbosity;
    writeSpawnOptions(payload, options);
    payload << args.s;

    std::vector<Descriptor> allFds(fds.begin(), fds.end());
    if (options.cgroupFd != INVALID_DESCRIPTOR)
        allFds.push_back(options.cgroupFd);

    try {
        spawner->send(MessageType::Spawn, payload.s, allFds);
    } catch (...) {
        spawner->state.lock()->pendingSpawns.erase(requestId);
        throw;
    }

    auto child = future.get();
    auto pid = child->pid;
    return Pid(pid, std::move(child));
}

int waitSpawnerChild(SpawnerChild & child, bool allowInterrupts)
{
    int status;

    {
        auto state(child.state.lock());
        while (!state->status && !state->connectionLost) {
            if (allowInterrupts) {
                state.wait_for(child.cv, 100ms);
                checkInterrupt();
            } else
                state.wait(child.cv);
        }
        if (!state->status)
            throw SpawnerError("cannot get exit status of PID %d because the spawner process died", child.pid);
        status = *state->status;
    }

    child.connection->release(child.pid);

    return status;
}

bool spawnerChildIsAlive(SpawnerChild & child)
{
    {
        auto state(child.state.lock());
        if (!state->status)
            return !state->connectionLost;
    }
    child.connection->release(child.pid);
    return false;
}

void detachSpawnerChild(SpawnerChild & child)
{
    child.connection->release(child.pid);
}

} // namespace nix
