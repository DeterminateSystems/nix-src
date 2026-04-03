#include "nix/util/os-string.hh"
#include "nix/util/processes.hh"
#include "nix/cmd/command.hh"
#include "nix/main/common-args.hh"
#include "nix/store/store-api.hh"
#include "nix/store/filetransfer.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/attr-path.hh"
#include "nix/store/names.hh"
#include "nix/util/executable-path.hh"
#include "nix/store/globals.hh"
#include "nix/util/config-global.hh"
#include "self-exe.hh"

using namespace nix;

/**
 * Check whether a path has a "profiles" component.
 */
static bool hasProfilesComponent(const std::filesystem::path & path)
{
    return std::ranges::contains(path, OS_STR("profiles"));
}

/**
 * Settings related to upgrading Nix itself.
 */
struct UpgradeSettings : Config
{
    /**
     * The URL of the file that contains the store paths of the latest Nix release.
     */
    Setting<std::string> storePathUrl{
        this,
        "",
        "upgrade-nix-store-path-url",
        R"(
          Deprecated. This option was used to configure how `nix upgrade-nix` operated.

          Using this setting has no effect. It will be removed in a future release of Determinate Nix.
        )"};
};

UpgradeSettings upgradeSettings;

static GlobalConfig::Register rSettings(&upgradeSettings);

struct CmdUpgradeNix : MixDryRun, StoreCommand
{
    /**
     * This command is stable before the others
     */
    std::optional<ExperimentalFeature> experimentalFeature() override
    {
        return std::nullopt;
    }

    std::string description() override
    {
        return "deprecated in favor of determinate-nixd upgrade";
    }

    std::string doc() override
    {
        return
#include "upgrade-nix.md"
            ;
    }

    Category category() override
    {
        return catNixInstallation;
    }

    void run(ref<Store> store) override
    {
        throw Error(
            "The upgrade-nix command isn't available in Determinate Nix; use %s instead",
            "sudo determinate-nixd upgrade");
    }
};

static auto rCmdUpgradeNix = registerCommand<CmdUpgradeNix>("upgrade-nix");
