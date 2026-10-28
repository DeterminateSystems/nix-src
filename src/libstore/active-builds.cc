#include "nix/store/active-builds.hh"
#include "nix/util/base-nix-32.hh"
#include "nix/util/file-system.hh"
#include "nix/util/json-utils.hh"
#include "nix/util/strings.hh"

#include <nlohmann/json.hpp>

#include <algorithm>

#ifndef _WIN32
#  include <pwd.h>
#endif

namespace nix {

void QueryActiveBuildsStore::anchor() {}

void TrackActiveBuildsStore::anchor() {}

std::optional<std::chrono::microseconds> ActiveBuildInfo::ProcessInfo::getCpuTime() const
{
    if (!utime && !stime && !cutime && !cstime)
        return std::nullopt;
    return utime.value_or(std::chrono::microseconds(0)) + stime.value_or(std::chrono::microseconds(0))
           + cutime.value_or(std::chrono::microseconds(0)) + cstime.value_or(std::chrono::microseconds(0));
}

std::string ActiveBuildInfo::ProcessInfo::formatArgv(bool stripProgramPath) const
{
    auto argv = this->argv;
    /* Don't touch argv[0] if it contains whitespace, since some processes overwrite their argv with a
       free-form title (like "sshd: user"). */
    if (stripProgramPath && !argv.empty() && argv[0].find_first_of(" \t\n") == std::string::npos)
        argv[0] = baseNameOf(argv[0]);
    return concatStringsSep(" ", tokenizeString<std::vector<std::string>>(concatStringsSep(" ", argv)));
}

std::chrono::microseconds ActiveBuildInfo::getCpuTime() const
{
    if (utime && stime)
        return *utime + *stime;
    std::chrono::microseconds total{0};
    for (const auto & process : processes)
        total += process.getCpuTime().value_or(std::chrono::microseconds(0));
    return total;
}

std::optional<uint64_t> ActiveBuildInfo::getRss() const
{
    if (rss)
        return rss;
    std::optional<uint64_t> total;
    for (const auto & process : processes)
        if (process.rss)
            total = total.value_or(0) + *process.rss;
    return total;
}

std::string abbreviateStorePaths(std::string_view s, std::string_view storeDir)
{
    constexpr size_t keep = 6;
    auto prefix = std::string(storeDir) + "/";
    std::string res;
    size_t pos = 0;
    while (true) {
        auto i = s.find(prefix, pos);
        if (i == s.npos)
            break;
        auto hashStart = i + prefix.size();
        auto hashEnd = hashStart + StorePath::HashLen;
        if (hashEnd < s.size() && s[hashEnd] == '-'
            && std::ranges::all_of(
                s.substr(hashStart, StorePath::HashLen),
                [](char c) { return BaseNix32::lookupReverse(c).has_value(); })) {
            res += s.substr(pos, hashStart + keep - pos);
            res += "…";
            pos = hashEnd;
        } else {
            res += s.substr(pos, hashStart - pos);
            pos = hashStart;
        }
    }
    res += s.substr(pos);
    return res;
}

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

// Durations are serialized as floats representing seconds.
static std::optional<std::chrono::microseconds> parseDuration(const json & j, const char * key)
{
    if (j.contains(key) && !j.at(key).is_null())
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::duration<float, std::chrono::seconds::period>(j.at(key).get<double>()));
    else
        return std::nullopt;
}

static nlohmann::json printDuration(const std::optional<std::chrono::microseconds> & duration)
{
    return duration
               ? nlohmann::json(
                     std::chrono::duration_cast<std::chrono::duration<float, std::chrono::seconds::period>>(*duration)
                         .count())
               : nullptr;
}

// Optional because older daemons don't send these fields.
static std::optional<uint64_t> parseOptionalUInt64(const json & j, const char * key)
{
    if (j.contains(key) && !j.at(key).is_null())
        return getUnsigned(j.at(key));
    else
        return std::nullopt;
}

ActiveBuildInfo::ProcessInfo adl_serializer<ActiveBuildInfo::ProcessInfo>::from_json(const json & j)
{
    return ActiveBuildInfo::ProcessInfo{
        .pid = j.at("pid").get<pid_t>(),
        .parentPid = j.at("parentPid").get<pid_t>(),
        .user = j.at("user").get<UserInfo>(),
        .argv = j.at("argv").get<std::vector<std::string>>(),
        .utime = parseDuration(j, "utime"),
        .stime = parseDuration(j, "stime"),
        .cutime = parseDuration(j, "cutime"),
        .cstime = parseDuration(j, "cstime"),
        .rss = parseOptionalUInt64(j, "rss"),
    };
}

void adl_serializer<ActiveBuildInfo::ProcessInfo>::to_json(json & j, const ActiveBuildInfo::ProcessInfo & process)
{
    j = nlohmann::json{
        {"pid", process.pid},
        {"parentPid", process.parentPid},
        {"user", process.user},
        {"argv", process.argv},
        {"utime", printDuration(process.utime)},
        {"stime", printDuration(process.stime)},
        {"cutime", printDuration(process.cutime)},
        {"cstime", printDuration(process.cstime)},
        {"rss", process.rss},
    };
}

ActiveBuild adl_serializer<ActiveBuild>::from_json(const json & j)
{
    auto type = j.at("type").get<std::string>();
    if (type != "build")
        throw Error("invalid active build JSON: expected type 'build' but got '%s'", type);
    std::optional<std::filesystem::path> cgroup;
    if (!j.at("cgroup").is_null())
        cgroup = j.at("cgroup").get<std::filesystem::path>();
    return ActiveBuild{
        .nixPid = j.at("nixPid").get<pid_t>(),
        .clientPid = j.at("clientPid").get<std::optional<pid_t>>(),
        .clientUid = j.at("clientUid").get<std::optional<uid_t>>(),
        .mainPid = j.at("mainPid").get<pid_t>(),
        .mainUser = j.at("mainUser").get<UserInfo>(),
        .mainUidCount = j.contains("mainUidCount") ? j.at("mainUidCount").get<uid_t>() : 1,
        .cgroup = std::move(cgroup),
        .startTime = (time_t) j.at("startTime").get<double>(),
        .derivation = StorePath{getString(j.at("derivation"))},
    };
}

void adl_serializer<ActiveBuild>::to_json(json & j, const ActiveBuild & build)
{
    j = nlohmann::json{
        {"type", "build"},
        {"nixPid", build.nixPid},
        {"clientPid", build.clientPid},
        {"clientUid", build.clientUid},
        {"mainPid", build.mainPid},
        {"mainUser", build.mainUser},
        {"mainUidCount", build.mainUidCount},
        {"cgroup", build.cgroup ? nlohmann::json(*build.cgroup) : nlohmann::json(nullptr)},
        {"startTime", (double) build.startTime},
        {"derivation", build.derivation.to_string()},
    };
}

ActiveBuildInfo adl_serializer<ActiveBuildInfo>::from_json(const json & j)
{
    ActiveBuildInfo info(adl_serializer<ActiveBuild>::from_json(j));
    info.processes = j.at("processes").get<std::vector<ActiveBuildInfo::ProcessInfo>>();
    info.utime = parseDuration(j, "utime");
    info.stime = parseDuration(j, "stime");
    info.rss = parseOptionalUInt64(j, "rss");
    return info;
}

void adl_serializer<ActiveBuildInfo>::to_json(json & j, const ActiveBuildInfo & build)
{
    adl_serializer<ActiveBuild>::to_json(j, build);
    j["processes"] = build.processes;
    j["utime"] = printDuration(build.utime);
    j["stime"] = printDuration(build.stime);
    j["rss"] = build.rss;
}

} // namespace nlohmann
