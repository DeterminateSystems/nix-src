#include "nix/store/local-store.hh"
#include "nix/util/json-utils.hh"
#ifdef __linux__
#  include "nix/util/cgroup.hh"
#endif

#include <nlohmann/json.hpp>

namespace nix {

#ifdef __linux__
static ActiveBuildInfo::ProcessInfo getProcessInfo(pid_t pid)
{
    return {
        .pid = pid,
        .parentPid =
            string2Int<pid_t>(tokenizeString<std::vector<std::string>>(readFile(fmt("/proc/%d/stat", pid))).at(3))
                .value_or(0),
        .argv =
            tokenizeString<std::vector<std::string>>(readFile(fmt("/proc/%d/cmdline", pid)), std::string("\000", 1)),
    };
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
