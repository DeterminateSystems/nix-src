#include "nix/store/local-store.hh"
#include "nix/util/json-utils.hh"
#ifdef __linux__
#  include "nix/util/cgroup.hh"
#  include <unistd.h>
#  include <pwd.h>
#endif

#include <nlohmann/json.hpp>

namespace nix {

#ifdef __linux__
static ActiveBuildInfo::ProcessInfo getProcessInfo(pid_t pid)
{
    ActiveBuildInfo::ProcessInfo info;
    info.pid = pid;
    info.argv =
        tokenizeString<std::vector<std::string>>(readFile(fmt("/proc/%d/cmdline", pid)), std::string("\000", 1));

    auto statPath = fmt("/proc/%d/stat", pid);

    AutoCloseFD statFd = open(statPath.c_str(), O_RDONLY | O_CLOEXEC);
    if (!statFd)
        throw SysError("opening '%s'", statPath);

    // Get the UID from the ownership of the stat file.
    struct stat st;
    if (fstat(statFd.get(), &st) == -1)
        throw SysError("getting ownership of '%s'", statPath);
    info.user = UserInfo::fromUid(st.st_uid);

    // Read /proc/[pid]/stat for parent PID and CPU times
    auto statFields = tokenizeString<std::vector<std::string>>(readFile(statFd.get()));

    // Field 3 (index 3) is ppid
    if (statFields.size() > 3)
        info.parentPid = string2Int<pid_t>(statFields[3]).value_or(0);

    // Fields 13 and 14 (indices 13, 14) are utime and stime in clock ticks
    static long clkTck = sysconf(_SC_CLK_TCK);
    if (statFields.size() > 14 && clkTck > 0) {
        auto utime = string2Int<uint64_t>(statFields[13]);
        auto stime = string2Int<uint64_t>(statFields[14]);

        if (utime) {
            // Convert clock ticks to microseconds: (ticks / clkTck) * 1000000
            info.cpuUser = std::chrono::microseconds((*utime * 1'000'000) / clkTck);
        }
        if (stime) {
            info.cpuSystem = std::chrono::microseconds((*stime * 1'000'000) / clkTck);
        }
    }

    return info;
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

#ifdef __linux__
            /* Read process information from /proc. */
            try {
                if (info.cgroup) {
                    for (auto pid : getPidsInCgroup(*info.cgroup))
                        info.processes.push_back(getProcessInfo(pid));

                    /* Read CPU statistics from the cgroup. */
                    auto stats = getCgroupStats(*info.cgroup);
                    info.cpuUser = stats.cpuUser;
                    info.cpuSystem = stats.cpuSystem;
                } else
                    info.processes.push_back(getProcessInfo(info.mainPid));
            } catch (...) {
                ignoreExceptionExceptInterrupt();
            }
#endif

            result.push_back(std::move(info));
        } catch (Error &) {
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

    writeFile(infoFilePath, nlohmann::json(build).dump(), 0600, FsSync::Yes);

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
