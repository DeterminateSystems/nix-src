#pragma once

#include "nix/store/path.hh"
#include "nix/util/json-impls.hh"

#include <sys/types.h>

namespace nix {

struct ActiveBuild
{
    pid_t nixPid;

    std::optional<pid_t> clientPid;
    std::optional<uid_t> clientUid;

    pid_t mainPid;
    uid_t mainUid;
    std::optional<Path> cgroup;

    time_t startTime;

    StorePath derivation;
};

struct ActiveBuildInfo : ActiveBuild
{
    struct ProcessInfo
    {
        pid_t pid = 0;
        pid_t parentPid = 0;
        std::vector<std::string> argv;
    };

    std::vector<ProcessInfo> processes;
};

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
            if (id)
                tracker.buildFinished(*this);
        }
    };

    virtual BuildHandle buildStarted(const ActiveBuild & build) = 0;

    virtual void buildFinished(const BuildHandle & handle) = 0;
};

struct QueryActiveBuildsStore
{
    inline static std::string operationName = "Querying active builds";

    virtual std::vector<ActiveBuildInfo> queryActiveBuilds() = 0;
};

} // namespace nix

JSON_IMPL(ActiveBuild)
JSON_IMPL(ActiveBuildInfo)
