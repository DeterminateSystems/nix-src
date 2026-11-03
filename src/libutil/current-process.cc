#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <ranges>

#include "nix/util/current-process.hh"
#include "nix/util/util.hh"
#include "nix/util/file-system.hh"
#include "nix/util/signals.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/sync.hh"
#include <math.h>

#ifndef _WIN32
#  include <signal.h>
#  include <sys/mman.h>
#  include <unistd.h>
#endif

#ifdef __APPLE__
#  include <mach-o/dyld.h>
#  include <mach/mach.h>
#endif

#ifdef __linux__
#  include "nix/util/cgroup.hh"
#  include "nix/util/linux-namespaces.hh"
#endif

#ifdef __FreeBSD__
#  include <sys/param.h>
#  include <sys/sysctl.h>
#endif

namespace nix {

unsigned int getMaxCPU()
{
#ifdef __linux__
    try {
        auto cgroupFS = linux::getCgroupFS();
        if (!cgroupFS)
            return 0;

        auto cpuFile = *cgroupFS / linux::getCurrentCgroup().rel() / "cpu.max";

        auto cpuMax = readFile(cpuFile);
        auto cpuMaxParts = tokenizeString<std::vector<std::string>>(cpuMax, " \n");

        if (cpuMaxParts.size() != 2) {
            return 0;
        }

        auto quota = cpuMaxParts[0];
        auto period = cpuMaxParts[1];
        if (quota != "max")
            return std::ceil(std::stoi(quota) / std::stof(period));
    } catch (Error &) {
        ignoreExceptionInDestructor(lvlDebug);
    }
#endif

    return 0;
}

//////////////////////////////////////////////////////////////////////

#ifndef _WIN32
size_t savedStackSize = 0;

void ensureStackSizeAtLeast(size_t stackSize)
{
    struct rlimit limit;
    if (getrlimit(RLIMIT_STACK, &limit) == 0 && static_cast<size_t>(limit.rlim_cur) < stackSize) {
        savedStackSize = limit.rlim_cur;
        if (limit.rlim_max < static_cast<rlim_t>(stackSize)) {
            if (getEnv("_NIX_TEST_NO_ENVIRONMENT_WARNINGS") != "1") {
                logger->log(
                    lvlWarn,
                    HintFmt(
                        "Stack size hard limit is %1%, which is less than the desired %2%. If possible, increase the hard limit, e.g. with 'ulimit -Hs %3%'.",
                        limit.rlim_max,
                        stackSize,
                        stackSize / 1024)
                        .str());
            }
        }
        auto requestedSize = std::min(static_cast<rlim_t>(stackSize), limit.rlim_max);
        limit.rlim_cur = requestedSize;
        if (setrlimit(RLIMIT_STACK, &limit) != 0) {
            logger->log(
                lvlError,
                HintFmt(
                    "Failed to increase stack size from %1% to %2% (desired: %3%, maximum allowed: %4%): %5%",
                    savedStackSize,
                    requestedSize,
                    stackSize,
                    limit.rlim_max,
                    std::strerror(errno))
                    .str());
        }
    }
}

namespace {

/**
 * The alternate signal stack of a thread, allocated by
 * `ensureSignalStack()`. It has a guard page on either side: the lower
 * one turns an overflow of the signal stack into a crash rather than
 * memory corruption, and both keep crash reporters (which capture a
 * thread's stack from the stack pointer to the end of its mapping,
 * including adjacent readable mappings) from capturing unrelated
 * memory when the stack pointer of the crashed thread is unusable.
 */
struct SignalStack
{
    static constexpr size_t size = 64 * 1024;

    /**
     * The start of the mapping, i.e. of the lower guard page.
     */
    char * mem = nullptr;

    ~SignalStack()
    {
        if (!mem)
            return;
        size_t pageSize = getpagesize();
        /* Stop using the stack before unmapping it, unless something
           else has replaced it in the meantime. */
        stack_t ss{}, old{};
        ss.ss_flags = SS_DISABLE;
        if (sigaltstack(&ss, &old) == 0 && old.ss_sp != mem + pageSize)
            sigaltstack(&old, nullptr);
        munmap(mem, size + 2 * pageSize);
    }
};

thread_local SignalStack signalStack;

} // namespace

void ensureSignalStack()
{
    size_t pageSize = getpagesize();

    if (!signalStack.mem) {
        auto mem = static_cast<char *>(
            mmap(nullptr, SignalStack::size + 2 * pageSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (mem == MAP_FAILED)
            throw SysError("allocating the alternate signal stack");
        if (mprotect(mem + pageSize, SignalStack::size, PROT_READ | PROT_WRITE) != 0) {
            SysError error("allocating the alternate signal stack");
            munmap(mem, SignalStack::size + 2 * pageSize);
            throw error;
        }
        signalStack.mem = mem;
    }

    stack_t ss{};
    ss.ss_sp = signalStack.mem + pageSize;
    ss.ss_size = SignalStack::size;
    ss.ss_flags = 0;
    if (sigaltstack(&ss, nullptr) != 0)
        throw SysError("setting the alternate signal stack");
}
#endif

void restoreProcessContext(bool restoreMounts)
{
#ifndef _WIN32
    unix::restoreSignals();
#endif
    if (restoreMounts) {
#ifdef __linux__
        restoreMountNamespace();
#endif
    }

#ifndef _WIN32
    if (savedStackSize) {
        struct rlimit limit;
        if (getrlimit(RLIMIT_STACK, &limit) == 0) {
            limit.rlim_cur = savedStackSize;
            setrlimit(RLIMIT_STACK, &limit);
        }
    }
#endif

#ifdef __APPLE__
    /* Reset the Mach exception ports. Otherwise, if a crashpad_handler is attached to this process, it will be
       inherited across execve() and receive spurious crash reports from unrelated programs (e.g. in `nix run`).
       FIXME: it would be better to have Sentry tell crashpad_handler to quit, but it doesn't appear to have an API for
       that. */
    task_set_exception_ports(
        mach_task_self(), EXC_MASK_ALL | EXC_MASK_CRASH, MACH_PORT_NULL, EXCEPTION_DEFAULT, THREAD_STATE_NONE);
#endif
}

//////////////////////////////////////////////////////////////////////

std::optional<std::filesystem::path> getSelfExe()
{
    static auto cached = []() -> std::optional<std::filesystem::path> {
#if defined(__linux__) || defined(__GNU__)
        return readLink(std::filesystem::path{"/proc/self/exe"});
#elif defined(__APPLE__)
        char buf[1024];
        uint32_t size = sizeof(buf);
        if (_NSGetExecutablePath(buf, &size) == 0)
            return buf;
        else
            return std::nullopt;
#elif defined(__FreeBSD__)
        int sysctlName[] = {
            CTL_KERN,
            KERN_PROC,
            KERN_PROC_PATHNAME,
            -1,
        };
        size_t pathLen = 0;
        if (sysctl(sysctlName, sizeof(sysctlName) / sizeof(sysctlName[0]), nullptr, &pathLen, nullptr, 0) < 0) {
            return std::nullopt;
        }

        std::vector<char> path(pathLen);
        if (sysctl(sysctlName, sizeof(sysctlName) / sizeof(sysctlName[0]), path.data(), &pathLen, nullptr, 0) < 0) {
            return std::nullopt;
        }

        // FreeBSD's sysctl(KERN_PROC_PATHNAME) includes the null terminator in
        // pathLen. Strip it to prevent Nix evaluation errors when the path is
        // serialized to JSON and evaluated as a Nix string.
        path.pop_back();

        return std::string(path.begin(), path.end());
#else
        return std::nullopt;
#endif
    }();
    return cached;
}

static Sync<std::vector<fun<void()>>> & quickExitHandlers()
{
    /* Leaked, so that it's never destroyed by static destructors. */
    static auto & handlers = *new Sync<std::vector<fun<void()>>>;
    return handlers;
}

void atQuickExit(fun<void()> f)
{
    quickExitHandlers().lock()->push_back(std::move(f));
}

void quickExit(int status)
{
    /* Take ownership of the handlers, so that they don't run again if
       a handler (or another thread) calls quickExit(). */
    auto handlers = std::exchange(*quickExitHandlers().lock(), {});

    for (auto & f : std::views::reverse(handlers)) {
        try {
            f();
        } catch (...) {
            ignoreExceptionInDestructor();
        }
    }

    std::cout.flush();
    std::cerr.flush();
    std::clog.flush();
    std::fflush(nullptr);

#ifdef __LLVM_INSTR_PROFILE_GENERATE
    /* In coverage builds, each instrumented library writes its
       profile from an `atexit()` handler, so we have to run them. */
    std::exit(status);
#else
    std::_Exit(status);
#endif
}

} // namespace nix
