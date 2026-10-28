#pragma once

#include "nix/util/util.hh"
#include "nix/util/json-impls.hh"
#include "nix/store/path.hh"

#include <chrono>
#include <sys/types.h>

namespace nix {

/**
 * A uid and optional corresponding user name.
 */
struct UserInfo
{
    uid_t uid = -1;
    std::optional<std::string> name;

    /**
     * Create a UserInfo from a UID, looking up the username if possible.
     */
    static UserInfo fromUid(uid_t uid);
};

struct ActiveBuild
{
    pid_t nixPid;

    std::optional<pid_t> clientPid;
    std::optional<uid_t> clientUid;

    pid_t mainPid;
    UserInfo mainUser;

    /**
     * The number of UIDs allocated to the build, starting at `mainUser.uid`. This is greater than 1 for builds that
     * use a UID range (`auto-allocate-uids` with the `uid-range` feature).
     */
    uid_t mainUidCount = 1;

    std::optional<std::filesystem::path> cgroup;

    time_t startTime;

    StorePath derivation;
};

struct ActiveBuildInfo : ActiveBuild
{
    struct ProcessInfo
    {
        pid_t pid = 0;
        pid_t parentPid = 0;
        UserInfo user;
        std::vector<std::string> argv;
        std::optional<std::chrono::microseconds> utime, stime, cutime, cstime;
        // Resident set size in bytes.
        std::optional<uint64_t> rss;

        /**
         * The total CPU time of this process, including that of its reaped children, or `std::nullopt` if not
         * available.
         */
        std::optional<std::chrono::microseconds> getCpuTime() const;

        /**
         * The command line of this process, with whitespace normalized. If `stripProgramPath` is set, only show the
         * base name of argv[0].
         */
        std::string formatArgv(bool stripProgramPath = false) const;
    };

    // User/system CPU time for the entire cgroup, if available.
    std::optional<std::chrono::microseconds> utime, stime;

    // Resident memory in bytes for the entire cgroup, if available.
    std::optional<uint64_t> rss;

    std::vector<ProcessInfo> processes;

    /**
     * The CPU time of this build. Use cgroup stats if available, otherwise sum process times.
     */
    std::chrono::microseconds getCpuTime() const;

    /**
     * The resident memory of this build. Use cgroup stats if available, otherwise sum process RSS. Note that the
     * latter double-counts shared pages.
     */
    std::optional<uint64_t> getRss() const;
};

/**
 * Abbreviate the hash parts of store paths in `s`, e.g. `/nix/store/2limljgc50jy76vg388ldgggcjhhkhav-gcc-15.2.0`
 * becomes `/nix/store/2limlj…-gcc-15.2.0`.
 */
std::string abbreviateStorePaths(std::string_view s, std::string_view storeDir);

struct TrackActiveBuildsStore
{
    struct BuildHandle
    {
        TrackActiveBuildsStore & tracker;
        uint64_t id;

        BuildHandle(TrackActiveBuildsStore & tracker, uint64_t id)
            : tracker(tracker)
            , id(id)
        {
        }

        BuildHandle(BuildHandle && other) noexcept
            : tracker(other.tracker)
            , id(other.id)
        {
            other.id = 0;
        }

        ~BuildHandle()
        {
            if (id) {
                try {
                    tracker.buildFinished(*this);
                } catch (...) {
                    ignoreExceptionInDestructor();
                }
            }
        }
    };

    virtual ~TrackActiveBuildsStore() = default;

    virtual BuildHandle buildStarted(const ActiveBuild & build) = 0;

    virtual void buildFinished(const BuildHandle & handle) = 0;

    virtual void anchor();
};

struct QueryActiveBuildsStore
{
    inline static std::string operationName = "Querying active builds";

    virtual ~QueryActiveBuildsStore() = default;

    virtual std::vector<ActiveBuildInfo> queryActiveBuilds() = 0;

    virtual void anchor();
};

} // namespace nix

JSON_IMPL(UserInfo)
JSON_IMPL(ActiveBuild)
JSON_IMPL(ActiveBuildInfo)
JSON_IMPL(ActiveBuildInfo::ProcessInfo)
