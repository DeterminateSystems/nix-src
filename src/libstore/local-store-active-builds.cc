#include "nix/store/local-store.hh"
#include "nix/util/json-utils.hh"
#ifdef __linux__
#  include "nix/util/cgroup.hh"
#  include <regex>
#  include <unistd.h>
#  include <pwd.h>
#endif

#ifdef __APPLE__
#  include <libproc.h>
#  include <sys/sysctl.h>
#  include <mach/mach_time.h>
#endif

#include <nlohmann/json.hpp>
#include <queue>

namespace nix {

#ifdef __linux__
static ActiveBuildInfo::ProcessInfo getProcessInfo(pid_t pid)
{
    ActiveBuildInfo::ProcessInfo info;
    info.pid = pid;

    auto procPath = fmt("/proc/%d", pid);

    AutoCloseFD procFd = open(procPath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (!procFd)
        throw SysError("opening '%s'", procPath);

    {
        AutoCloseFD cmdlineFd = openat(procFd.get(), "cmdline", O_RDONLY | O_CLOEXEC);
        if (!cmdlineFd)
            throw SysError("opening '%s/cmdline'", procPath);
        info.argv = tokenizeString<std::vector<std::string>>(readFile(cmdlineFd.get()), std::string("\000", 1));
    }

    // Get the UID from the ownership of the /proc/[pid] directory. Note that we can't use the files in that directory
    // (like /proc/[pid]/stat) because the kernel makes those owned by root if the process is a zombie or not
    // dumpable.
    struct stat st;
    if (fstat(procFd.get(), &st) == -1)
        throw SysError("getting ownership of '%s'", procPath);
    info.user = UserInfo::fromUid(st.st_uid);

    AutoCloseFD statFd = openat(procFd.get(), "stat", O_RDONLY | O_CLOEXEC);
    if (!statFd)
        throw SysError("opening '%s/stat'", procPath);

    // Read /proc/[pid]/stat for parent PID and CPU times.
    // Format: pid (comm) state ppid ...
    // Note that the comm field can contain spaces, so use a regex to parse it.
    auto statContent = trim(readFile(statFd.get()));
    static std::regex statRegex(R"((\d+) \(([^)]*)\) (.*))");
    std::smatch match;
    if (!std::regex_match(statContent, match, statRegex))
        throw Error("failed to parse /proc/%d/stat", pid);

    // Parse the remaining fields after (comm).
    auto remainingFields = tokenizeString<std::vector<std::string>>(match[3].str());

    if (remainingFields.size() > 1)
        info.parentPid = string2Int<pid_t>(remainingFields[1]).value_or(0);

    static long clkTck = sysconf(_SC_CLK_TCK);
    if (remainingFields.size() > 14 && clkTck > 0) {
        if (auto utime = string2Int<uint64_t>(remainingFields[11]))
            info.utime = std::chrono::microseconds((*utime * 1'000'000) / clkTck);
        if (auto stime = string2Int<uint64_t>(remainingFields[12]))
            info.stime = std::chrono::microseconds((*stime * 1'000'000) / clkTck);
        if (auto cutime = string2Int<uint64_t>(remainingFields[13]))
            info.cutime = std::chrono::microseconds((*cutime * 1'000'000) / clkTck);
        if (auto cstime = string2Int<uint64_t>(remainingFields[14]))
            info.cstime = std::chrono::microseconds((*cstime * 1'000'000) / clkTck);
    }

    // Get the resident set size from /proc/[pid]/statm. Note that we don't use the proportional set size (PSS) from
    // /proc/[pid]/smaps_rollup, even though it would avoid double-counting shared pages when summing over processes,
    // because computing it requires the kernel to walk all page tables of the process while holding its mmap lock,
    // which is expensive for large processes and stalls them. RSS on the other hand is a cheap counter read. For
    // the build total, we use the cgroup's memory statistics if available, which don't double-count.
    {
        AutoCloseFD statmFd = openat(procFd.get(), "statm", O_RDONLY | O_CLOEXEC);
        if (!statmFd)
            throw SysError("opening '%s/statm'", procPath);
        // Format: size resident shared text lib data dt (in pages).
        auto statmFields = tokenizeString<std::vector<std::string>>(readFile(statmFd.get()));
        static long pageSize = sysconf(_SC_PAGESIZE);
        if (statmFields.size() > 1 && pageSize > 0)
            if (auto resident = string2Int<uint64_t>(statmFields[1]))
                info.rss = *resident * pageSize;
    }

    return info;
}

/**
 * Recursively get all descendant PIDs of a given PID using /proc/[pid]/task/[pid]/children.
 */
static std::set<pid_t> getDescendantPids(pid_t pid)
{
    std::set<pid_t> descendants;

    [&](this auto self, pid_t pid) -> void {
        try {
            descendants.insert(pid);
            for (const auto & childPidStr :
                 tokenizeString<std::vector<std::string>>(readFile(fmt("/proc/%d/task/%d/children", pid, pid))))
                if (auto childPid = string2Int<pid_t>(childPidStr))
                    self(*childPid);
        } catch (...) {
            // Process may have exited.
            ignoreExceptionExceptInterrupt(lvlDebug);
        }
    }(pid);

    return descendants;
}
#endif

#ifdef __APPLE__
static ActiveBuildInfo::ProcessInfo getProcessInfo(pid_t pid)
{
    ActiveBuildInfo::ProcessInfo info;
    info.pid = pid;

    // Get basic process info including ppid and uid.
    struct proc_bsdinfo procInfo;
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &procInfo, sizeof(procInfo)) != sizeof(procInfo))
        throw SysError("getting process info for pid %d", pid);

    info.parentPid = procInfo.pbi_ppid;
    info.user = UserInfo::fromUid(procInfo.pbi_uid);

    // Get CPU times.
    struct proc_taskinfo taskInfo;
    if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &taskInfo, sizeof(taskInfo)) == sizeof(taskInfo)) {

        mach_timebase_info_data_t timebase;
        mach_timebase_info(&timebase);
        auto nanosecondsPerTick = (double) timebase.numer / (double) timebase.denom;

        // Convert nanoseconds to microseconds.
        info.utime =
            std::chrono::microseconds((uint64_t) ((double) taskInfo.pti_total_user * nanosecondsPerTick / 1000));
        info.stime =
            std::chrono::microseconds((uint64_t) ((double) taskInfo.pti_total_system * nanosecondsPerTick / 1000));

        info.rss = taskInfo.pti_resident_size;
    }

    // Get argv using sysctl.
    int mib[3] = {CTL_KERN, KERN_PROCARGS2, pid};
    size_t size = 0;

    // First call to get size.
    if (sysctl(mib, 3, nullptr, &size, nullptr, 0) == 0 && size > 0) {
        std::vector<char> buffer(size);
        if (sysctl(mib, 3, buffer.data(), &size, nullptr, 0) == 0) {
            // Format: argc (int), followed by executable path, followed by null-terminated args
            if (size >= sizeof(int)) {
                int argc;
                memcpy(&argc, buffer.data(), sizeof(argc));

                // Skip past argc and executable path (null-terminated).
                size_t pos = sizeof(int);
                while (pos < size && buffer[pos] != '\0')
                    pos++;
                pos++; // Skip the null terminator

                // Parse the arguments.
                while (pos < size && info.argv.size() < (size_t) argc) {
                    size_t argStart = pos;
                    while (pos < size && buffer[pos] != '\0')
                        pos++;

                    if (pos > argStart)
                        info.argv.emplace_back(buffer.data() + argStart, pos - argStart);

                    pos++; // Skip the null terminator
                }
            }
        }
    }

    return info;
}

/**
 * Recursively get all descendant PIDs using sysctl with KERN_PROC.
 */
static std::set<pid_t> getDescendantPids(pid_t startPid)
{
    // Get all processes.
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
    size_t size = 0;

    if (sysctl(mib, 4, nullptr, &size, nullptr, 0) == -1)
        return {startPid};

    std::vector<struct kinfo_proc> procs(size / sizeof(struct kinfo_proc));
    if (sysctl(mib, 4, procs.data(), &size, nullptr, 0) == -1)
        return {startPid};

    // Get the children of all processes.
    std::map<pid_t, std::set<pid_t>> children;
    size_t count = size / sizeof(struct kinfo_proc);
    for (size_t i = 0; i < count; i++) {
        pid_t childPid = procs[i].kp_proc.p_pid;
        pid_t parentPid = procs[i].kp_eproc.e_ppid;
        children[parentPid].insert(childPid);
    }

    // Get all children of `pid`.
    std::set<pid_t> descendants;
    std::queue<pid_t> todo;
    todo.push(startPid);
    while (auto pid = pop(todo)) {
        if (!descendants.insert(*pid).second)
            continue;
        for (auto & child : children[*pid])
            todo.push(child);
    }

    return descendants;
}
#endif

std::vector<ActiveBuildInfo> LocalStore::queryActiveBuilds()
{
    std::vector<ActiveBuildInfo> result;

    for (auto & entry : DirectoryIterator{activeBuildsDir}) {
        auto path = entry.path();

        try {
            // Open the file. If we can lock it, the build is not active.
            auto fd = openLockFile(path, false);
            if (!fd || lockFile(fd.get(), ltRead, false)) {
                AutoDelete(path, false);
                continue;
            }

            ActiveBuildInfo info(nlohmann::json::parse(readFile(fd.get())).get<ActiveBuild>());

#if defined(__linux__) || defined(__APPLE__)
            /* Read process information. */
            try {
                std::set<pid_t> pids;
#  ifdef __linux__
                if (info.cgroup) {
                    pids = linux::getPidsInCgroup(*info.cgroup);

                    /* Read CPU statistics from the cgroup. */
                    auto stats = linux::getCgroupStats(*info.cgroup);
                    info.utime = stats.cpuUser;
                    info.stime = stats.cpuSystem;
                    info.rss = stats.memory;
                } else
#  endif
                    pids = getDescendantPids(info.mainPid);

                for (auto pid : pids) {
                    try {
                        auto process = getProcessInfo(pid);
                        /* Skip processes not owned by the build's UID(s). These are daemon processes that have
                           forked to start the builder but haven't switched to the build user yet. */
                        if (process.user.uid < info.mainUser.uid
                            || process.user.uid - info.mainUser.uid >= info.mainUidCount)
                            continue;
                        info.processes.push_back(std::move(process));
                    } catch (...) {
                        // Process may have exited.
                        ignoreExceptionExceptInterrupt(lvlDebug);
                    }
                }
            } catch (...) {
                ignoreExceptionExceptInterrupt();
            }
#endif

            result.push_back(std::move(info));
        } catch (...) {
            ignoreExceptionExceptInterrupt();
        }
    }

    return result;
}

LocalStore::BuildHandle LocalStore::buildStarted(const ActiveBuild & build)
{
    // Write info about the active build to the active-builds directory where it can be read by `queryBuilds()`.
    static std::atomic<uint64_t> nextId{1};

    auto id = nextId++;

    auto infoFileName = fmt("%d-%d", getpid(), id);
    auto infoFilePath = activeBuildsDir / infoFileName;

    auto infoFd = openLockFile(infoFilePath, true);

    // Lock the file to denote that the build is active.
    lockFile(infoFd.get(), ltWrite, true);

    // Don't fsync: the lock above is what marks the build as active, and queryActiveBuilds() deletes any file it
    // can lock, so durability buys nothing here. An fsync (F_FULLFSYNC on macOS) would also delay the parent
    // between starting the builder and reading its setup messages.
    writeFile(infoFilePath, nlohmann::json(build).dump(), 0600, FsSync::No);

    activeBuilds.lock()->emplace(
        id,
        ActiveBuildFile{
            .fd = std::move(infoFd),
            .del = AutoDelete(infoFilePath, false),
        });

    return BuildHandle(*this, id);
}

void LocalStore::buildFinished(const BuildHandle & handle)
{
    activeBuilds.lock()->erase(handle.id);
}

} // namespace nix
