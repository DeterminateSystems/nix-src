#include "flake-command.hh"
#include "nix/fetchers/fetch-to-store.hh"
#include "nix/util/thread-pool.hh"
#include "nix/store/filetransfer.hh"
#include "nix/util/exit.hh"

#include <nlohmann/json.hpp>

namespace nix {

struct CmdFlakePrefetchInputs : FlakeCommand
{
    std::string description() override
    {
        return "fetch the inputs of a flake";
    }

    std::string doc() override
    {
        return
#include "flake-prefetch-inputs.md"
            ;
    }

    void run(nix::ref<nix::Store> store) override
    {
        auto flake = lockFlake();

        /* Gather the locked references of all transitive inputs,
           skipping build-time inputs and their dependencies. */
        std::vector<FlakeRef> lockedRefs;

        flake->visit([&](const flake::InputAttrPath & inputAttrPath, const auto & input) {
            auto inputInfo = std::get_if<flake::LockedFlake::InputInfo>(&input);

            /* Skip "follows" inputs and build-time inputs (and their
               dependencies). */
            if (!inputInfo || inputInfo->buildTime)
                return false;

            /* Skip the root flake, which we've fetched already. */
            if (!inputAttrPath.empty())
                lockedRefs.push_back(inputInfo->lockedRef);

            return true;
        });

        /* Fetch the inputs in parallel. */
        ThreadPool pool{fileTransferSettings.httpConnections};

        std::atomic<size_t> nrFailed{0};

        for (auto & lockedRef : lockedRefs) {
            pool.enqueue([&, lockedRef]() {
                try {
                    Activity act(*logger, lvlInfo, actUnknown, fmt("fetching '%s'", lockedRef));
                    auto accessor = lockedRef.input.getAccessor(fetchSettings, *store).first;
                    if (!evalSettings.lazyTrees)
                        fetchToStore(fetchSettings, *store, accessor, FetchMode::Copy, lockedRef.input.getName());
                } catch (Error & e) {
                    printError("%s", e.what());
                    nrFailed++;
                }
            });
        }

        pool.process();

        throw Exit(nrFailed ? 1 : 0);
    }
};

static auto rCmdFlakePrefetchInputs = registerCommand2<CmdFlakePrefetchInputs>({"flake", "prefetch-inputs"});

} // namespace nix
