#include "nix/store/active-builds.hh"
#include "nix/util/json-utils.hh"

#include <nlohmann/json.hpp>

namespace nix {

} // namespace nix

namespace nlohmann {

using namespace nix;

ActiveBuildInfo::ProcessInfo adl_serializer<ActiveBuildInfo::ProcessInfo>::from_json(const json & j)
{
    ActiveBuildInfo::ProcessInfo info;
    info.pid = j.at("pid").get<pid_t>();
    info.parentPid = j.at("parentPid").get<pid_t>();
    info.argv = j.at("argv").get<std::vector<std::string>>();

    // Deserialize CPU times from seconds (as float) to microseconds
    if (j.contains("cpuUser") && !j.at("cpuUser").is_null())
        info.cpuUser = std::chrono::microseconds(static_cast<int64_t>(j.at("cpuUser").get<double>() * 1'000'000));

    if (j.contains("cpuSystem") && !j.at("cpuSystem").is_null())
        info.cpuSystem = std::chrono::microseconds(static_cast<int64_t>(j.at("cpuSystem").get<double>() * 1'000'000));

    return info;
}

void adl_serializer<ActiveBuildInfo::ProcessInfo>::to_json(json & j, const ActiveBuildInfo::ProcessInfo & process)
{
    j = nlohmann::json{
        {"pid", process.pid},
        {"parentPid", process.parentPid},
        {"argv", process.argv},
    };

    // Serialize CPU times as seconds (as float)
    if (process.cpuUser)
        j["cpuUser"] = static_cast<double>(process.cpuUser->count()) / 1'000'000.0;
    else
        j["cpuUser"] = nullptr;

    if (process.cpuSystem)
        j["cpuSystem"] = static_cast<double>(process.cpuSystem->count()) / 1'000'000.0;
    else
        j["cpuSystem"] = nullptr;
}

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

    // Deserialize CPU times from seconds (as float) to microseconds
    if (j.contains("cpuUser") && !j.at("cpuUser").is_null())
        info.cpuUser = std::chrono::microseconds(static_cast<int64_t>(j.at("cpuUser").get<double>() * 1'000'000));

    if (j.contains("cpuSystem") && !j.at("cpuSystem").is_null())
        info.cpuSystem = std::chrono::microseconds(static_cast<int64_t>(j.at("cpuSystem").get<double>() * 1'000'000));

    return info;
}

void adl_serializer<ActiveBuildInfo>::to_json(json & j, const ActiveBuildInfo & build)
{
    adl_serializer<ActiveBuild>::to_json(j, build);
    j["processes"] = build.processes;

    // Serialize CPU times as seconds (as float)
    if (build.cpuUser)
        j["cpuUser"] = static_cast<double>(build.cpuUser->count()) / 1'000'000.0;
    else
        j["cpuUser"] = nullptr;

    if (build.cpuSystem)
        j["cpuSystem"] = static_cast<double>(build.cpuSystem->count()) / 1'000'000.0;
    else
        j["cpuSystem"] = nullptr;
}

} // namespace nlohmann
