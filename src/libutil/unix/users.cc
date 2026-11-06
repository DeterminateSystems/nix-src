#include "nix/util/users.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/file-system.hh"
#include "nix/util/logging.hh"

#include <grp.h>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

namespace nix {

static PasswdEntry toInfo(const struct passwd & pw)
{
    return {
        .name = pw.pw_name,
        .uid = pw.pw_uid,
        .gid = pw.pw_gid,
        .home = pw.pw_dir ? pw.pw_dir : "",
    };
}

static GroupEntry toInfo(const struct group & gr)
{
    GroupEntry info{.name = gr.gr_name, .gid = gr.gr_gid};
    for (char ** mem = gr.gr_mem; *mem; ++mem)
        info.members.push_back(*mem);
    return info;
}

/**
 * Call a `getpw*_r()` or `getgr*_r()` function, growing the buffer
 * until the entry fits.
 */
template<typename Entry>
static auto lookup(auto && getEntry) -> std::optional<decltype(toInfo(std::declval<const Entry &>()))>
{
    std::vector<char> buf(1024);
    while (true) {
        Entry entry;
        Entry * res = nullptr;
        int err = getEntry(&entry, buf.data(), buf.size(), &res);
        if (err == ERANGE) {
            buf.resize(buf.size() * 2);
            continue;
        }
        /* Like getpwnam() etc., treat errors as the entry not
           existing. Some systems return an error (e.g. ENOENT) for a
           missing entry. */
        if (err || !res)
            return std::nullopt;
        return toInfo(*res);
    }
}

std::optional<PasswdEntry> getUserByName(const std::string & name)
{
    return lookup<struct passwd>([&](auto... args) { return getpwnam_r(name.c_str(), args...); });
}

std::optional<PasswdEntry> getUserById(uid_t uid)
{
    return lookup<struct passwd>([&](auto... args) { return getpwuid_r(uid, args...); });
}

std::optional<GroupEntry> getGroupByName(const std::string & name)
{
    return lookup<struct group>([&](auto... args) { return getgrnam_r(name.c_str(), args...); });
}

std::optional<GroupEntry> getGroupById(gid_t gid)
{
    return lookup<struct group>([&](auto... args) { return getgrgid_r(gid, args...); });
}

std::string getUserName()
{
    auto user = getUserById(geteuid());
    std::string name = user ? user->name : getEnv("USER").value_or("");
    if (name.empty())
        throw Error("cannot figure out user name");
    return name;
}

std::filesystem::path getHomeOf(uid_t userId)
{
    auto user = getUserById(userId);
    if (!user || user->home.empty())
        throw Error("cannot determine user's home directory");
    return user->home;
}

std::filesystem::path getHome()
{
    static std::filesystem::path homeDir = []() {
        std::optional<std::filesystem::path> unownedUserHomeDir = {};
        /* Ignore an empty or relative $HOME, since that would cause
           files to be created relative to the current directory. */
        auto homeDir = getEnvOsAbsPath("HOME");
        if (homeDir) {
            // Only use $HOME if doesn't exist or is owned by the current user.
            try {
                auto st = maybeStat(*homeDir);
                if (st && st->st_uid != geteuid())
                    unownedUserHomeDir.swap(homeDir);
            } catch (SysError & e) {
                warn(
                    "couldn't stat $HOME ('%s') for reason other than not existing, falling back to the one defined in the 'passwd' file: %s",
                    homeDir->string(),
                    e.what());
                homeDir.reset();
            }
        }
        if (!homeDir) {
            homeDir = getHomeOf(geteuid());
            if (unownedUserHomeDir.has_value() && unownedUserHomeDir != homeDir) {
                warn(
                    "$HOME ('%s') is not owned by you, falling back to the one defined in the 'passwd' file ('%s')",
                    unownedUserHomeDir->string(),
                    homeDir->string());
            }
        }
        return *homeDir;
    }();
    return homeDir;
}

bool isRootUser()
{
    return getuid() == 0;
}

} // namespace nix
