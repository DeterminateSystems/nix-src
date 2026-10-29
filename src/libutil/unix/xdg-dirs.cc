#include "nix/util/strings.hh"
#include "nix/util/users.hh"
#include "nix/util/environment-variables.hh"

namespace nix::unix::xdg {

std::filesystem::path getCacheHome()
{
    auto xdgDir = getEnvOsAbsPath("XDG_CACHE_HOME");
    if (xdgDir) {
        return *xdgDir;
    } else {
        return getHome() / ".cache";
    }
}

std::filesystem::path getConfigHome()
{
    auto xdgDir = getEnvOsAbsPath("XDG_CONFIG_HOME");
    if (xdgDir) {
        return *xdgDir;
    } else {
        return getHome() / ".config";
    }
}

std::vector<std::filesystem::path> getConfigDirs()
{
    auto configDirs = getEnv("XDG_CONFIG_DIRS").value_or("/etc/xdg");
    auto tokens = tokenizeString<std::vector<std::string>>(configDirs, ":");
    std::vector<std::filesystem::path> result;
    for (auto & token : tokens) {
        std::filesystem::path dir{token};
        // Per the XDG spec, relative paths must be ignored.
        if (dir.is_absolute())
            result.push_back(std::move(dir));
    }
    return result;
}

std::filesystem::path getDataHome()
{
    auto xdgDir = getEnvOsAbsPath("XDG_DATA_HOME");
    if (xdgDir) {
        return *xdgDir;
    } else {
        return getHome() / ".local" / "share";
    }
}

std::filesystem::path getStateHome()
{
    auto xdgDir = getEnvOsAbsPath("XDG_STATE_HOME");
    if (xdgDir) {
        return *xdgDir;
    } else {
        return getHome() / ".local" / "state";
    }
}

} // namespace nix::unix::xdg
