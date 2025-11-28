#include "nix/store/active-builds.hh"
#include "nix/util/json-utils.hh"

#include <nlohmann/json.hpp>

namespace nix {

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ActiveBuildInfo::ProcessInfo, pid, parentPid, argv)

} // namespace nix

namespace nlohmann {

using namespace nix;

ActiveBuild adl_serializer<ActiveBuild>::from_json(const json & j)
{
    return ActiveBuild{
        .nixPid = j.at("nixPid").get<pid_t>(),
        .clientPid = j.at("clientPid").get<std::optional<pid_t>>(),
        .clientUid = j.at("clientUid").get<std::optional<uid_t>>(),
        .mainPid = j.at("mainPid").get<pid_t>(),
        .mainUid = j.at("mainUid").get<uid_t>(),
        .cgroup = j.at("cgroup").get<std::optional<Path>>(),
        .startTime = j.at("startTime").get<time_t>(),
        .derivation = StorePath{getString(j.at("derivation"))},
    };
}

void adl_serializer<ActiveBuild>::to_json(json & j, const ActiveBuild & build)
{
    j = nlohmann::json{
        {"nixPid", build.nixPid},
        {"clientPid", build.clientPid},
        {"clientUid", build.clientUid},
        {"mainPid", build.mainPid},
        {"mainUid", build.mainUid},
        {"cgroup", build.cgroup},
        {"startTime", build.startTime},
        {"derivation", build.derivation.to_string()},
    };
}

ActiveBuildInfo adl_serializer<ActiveBuildInfo>::from_json(const json & j)
{
    ActiveBuildInfo info(adl_serializer<ActiveBuild>::from_json(j));
    info.processes = j.at("processes").get<std::vector<ActiveBuildInfo::ProcessInfo>>();
    return info;
}

void adl_serializer<ActiveBuildInfo>::to_json(json & j, const ActiveBuildInfo & build)
{
    adl_serializer<ActiveBuild>::to_json(j, build);
    j["processes"] = build.processes;
}

} // namespace nlohmann
