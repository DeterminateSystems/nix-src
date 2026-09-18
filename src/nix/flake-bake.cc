#include "flake-command.hh"
#include "nix/cmd/flake-schemas.hh"
#include "nix/util/file-system.hh"

#include <nlohmann/json.hpp>

using namespace nix;
using namespace nix::flake;

struct CmdFlakeBake : FlakeCommand, MixFlakeSchemas
{
    std::filesystem::path destDir;

    flake_schemas::FlakeInventoryOptions options{
        .showLegacy = true,
        .showOutputPaths = true,
        .showDrvNames = true,
        .bake = true,
    };

    CmdFlakeBake()
    {
        addFlag({
            .longName = "dest-dir",
            .description = "Directory in which to write the baked `flake.nix`.",
            .labels = {"path"},
            .handler = {&destDir},
            .completer = completePath,
            .required = true,
        });
        addFlag({
            .longName = "all-systems",
            .description = "Bake the outputs for all systems, not just the current system.",
            .handler = {&options.showAllSystems, true},
        });
    }

    std::string description() override
    {
        return "bake a flake";
    }

    std::optional<ExperimentalFeature> experimentalFeature() override
    {
        return Xp::BakedDerivation;
    }

    std::string doc() override
    {
        return
#include "flake-bake.md"
            ;
    }

    void run(nix::ref<nix::Store> store) override
    {
        auto state = getEvalState();
        auto evalStore = getEvalStore();
        auto flake = make_ref<LockedFlake>(lockFlake());

        auto cache = flake_schemas::call(*state, flake, getDefaultFlakeSchemas());

        auto inv = flake_schemas::getFlakeInventory(*state, *getEvalStore(), *flake, cache, options);

        std::filesystem::create_directories(destDir);
        writeFile(destDir / "outputs.json", inv.dump());
        writeFile(
            destDir / "flake.nix",
#include "baked-flake.nix.gen.hh"
        );
    }
};

static auto rCmdFlakeBake = registerCommand2<CmdFlakeBake>({"flake", "bake"});
