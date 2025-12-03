#include "nix/store/active-builds.hh"
#include "nix/util/json-utils.hh"

#include <nlohmann/json.hpp>

#ifndef _WIN32
#  include <pwd.h>
#endif

namespace nix {

UserInfo UserInfo::fromUid(uid_t uid)
{
    UserInfo info;
    info.uid = uid;

#ifndef _WIN32
    // Look up the user name for the UID (thread-safe)
    struct passwd pwd;
    struct passwd * result;
    std::vector<char> buf(16384);
    if (getpwuid_r(uid, &pwd, buf.data(), buf.size(), &result) == 0 && result)
        info.name = result->pw_name;
#endif

    return info;
}

} // namespace nix

namespace nlohmann {

using namespace nix;

UserInfo adl_serializer<UserInfo>::from_json(const json & j)
{
    return UserInfo{
        .uid = j.at("uid").get<uid_t>(),
        .name = j.contains("name") && !j.at("name").is_null()
                    ? std::optional<std::string>(j.at("name").get<std::string>())
                    : std::nullopt,
    };
}

void adl_serializer<UserInfo>::to_json(json & j, const UserInfo & info)
{
    j = nlohmann::json{
        {"uid", info.uid},
        {"name", info.name},
    };
}

ActiveBuildInfo::ProcessInfo adl_serializer<ActiveBuildInfo::ProcessInfo>::from_json(const json & j)
{
    std::optional<std::chrono::microseconds> utime, stime;

    // Deserialize CPU times from seconds (as float) to microseconds
    if (j.contains("utime") && !j.at("utime").is_null())
        utime = std::chrono::microseconds(static_cast<int64_t>(j.at("utime").get<double>() * 1'000'000));

    if (j.contains("stime") && !j.at("stime").is_null())
        stime = std::chrono::microseconds(static_cast<int64_t>(j.at("stime").get<double>() * 1'000'000));

    return ActiveBuildInfo::ProcessInfo{
        .pid = j.at("pid").get<pid_t>(),
        .parentPid = j.at("parentPid").get<pid_t>(),
        .user = j.at("user").get<UserInfo>(),
        .argv = j.at("argv").get<std::vector<std::string>>(),
        .utime = utime,
        .stime = stime,
    };
}

void adl_serializer<ActiveBuildInfo::ProcessInfo>::to_json(json & j, const ActiveBuildInfo::ProcessInfo & process)
{
    j = nlohmann::json{
        {"pid", process.pid},
        {"parentPid", process.parentPid},
        {"user", process.user},
        {"argv", process.argv},
    };

    // Serialize CPU times as seconds (as float)
    if (process.utime)
        j["utime"] = static_cast<double>(process.utime->count()) / 1'000'000.0;
    else
        j["utime"] = nullptr;

    if (process.stime)
        j["stime"] = static_cast<double>(process.stime->count()) / 1'000'000.0;
    else
        j["stime"] = nullptr;
}

ActiveBuild adl_serializer<ActiveBuild>::from_json(const json & j)
{
    return ActiveBuild{
        .nixPid = j.at("nixPid").get<pid_t>(),
        .clientPid = j.at("clientPid").get<std::optional<pid_t>>(),
        .clientUid = j.at("clientUid").get<std::optional<uid_t>>(),
        .mainPid = j.at("mainPid").get<pid_t>(),
        .mainUser = j.at("mainUser").get<UserInfo>(),
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
        {"mainUser", build.mainUser},
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
    if (j.contains("utime") && !j.at("utime").is_null())
        info.utime = std::chrono::microseconds(static_cast<int64_t>(j.at("utime").get<double>() * 1'000'000));

    if (j.contains("stime") && !j.at("stime").is_null())
        info.stime = std::chrono::microseconds(static_cast<int64_t>(j.at("stime").get<double>() * 1'000'000));

    return info;
}

void adl_serializer<ActiveBuildInfo>::to_json(json & j, const ActiveBuildInfo & build)
{
    adl_serializer<ActiveBuild>::to_json(j, build);
    j["processes"] = build.processes;

    // Serialize CPU times as seconds (as float)
    if (build.utime)
        j["utime"] = static_cast<double>(build.utime->count()) / 1'000'000.0;
    else
        j["utime"] = nullptr;

    if (build.stime)
        j["stime"] = static_cast<double>(build.stime->count()) / 1'000'000.0;
    else
        j["stime"] = nullptr;
}

} // namespace nlohmann
