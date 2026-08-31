#include "nix/util/global-paths.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/executable-path.hh"
#include "nix/util/file-system.hh"
#include "nix/util/users.hh"

#ifdef _WIN32
#  include "nix/util/windows-known-folders.hh"
#endif

#include "util-config-private.hh"

namespace nix {

/**
 * On Windows, NIX_CONF_DIR (and other directories like NIX_STATE_DIR, NIX_LOG_DIR)
 * are not defined at compile time, so we determine paths at runtime using the
 * Windows known folders API (FOLDERID_ProgramData). This allows Nix to work
 * correctly regardless of which drive Windows is installed on.
 */
const std::filesystem::path & nixConfDir()
{
    static const std::filesystem::path dir = getEnvOsNonEmpty(OS_STR("NIX_CONF_DIR"))
                                                 .transform([](auto && s) { return std::filesystem::path(s); })
                                                 .or_else([]() -> std::optional<std::filesystem::path> {
#ifdef _WIN32
#  ifdef NIX_CONF_DIR
#    error "NIX_CONF_DIR should not be defined on Windows"
#  endif
                                                     return windows::known_folders::getProgramData() / "nix" / "conf";
#else
                                                     return NIX_CONF_DIR;
#endif
                                                 })
                                                 .transform([](auto && s) { return canonPath(s); })
                                                 .value();
    return dir;
}

const std::vector<std::filesystem::path> & nixUserConfFiles()
{
    static const std::vector<std::filesystem::path> files = [] {
        // Use the paths specified in NIX_USER_CONF_FILES if it has been defined
        auto nixConfFiles = getEnvOs(OS_STR("NIX_USER_CONF_FILES"));
        if (nixConfFiles.has_value()) {
            return ExecutablePath::parse(*nixConfFiles).directories;
        }

        // Use the paths specified by the XDG spec
        std::vector<std::filesystem::path> files;
        auto dirs = getConfigDirs();
        for (auto & dir : dirs) {
            files.insert(files.end(), dir / "nix.conf");
        }
        return files;
    }();
    return files;
}

} // namespace nix
