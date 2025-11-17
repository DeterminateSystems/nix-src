#include "nix/fetchers/fetch-settings.hh"
#include "nix/util/config-global.hh"
#include "nix/util/logging.hh"

namespace nix::fetchers {

Settings::Settings()
{
    if (getenv("TMP_DEBUG"))
        printError("Settings() %p %p %p", this, &_cache, &fetchSettings);
}

} // namespace nix::fetchers

namespace nix {

fetchers::Settings fetchSettings;

static GlobalConfig::Register rFetchSettings(&fetchSettings);

} // namespace nix
