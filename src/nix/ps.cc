#include "nix/cmd/command.hh"
#include "nix/main/shared.hh"
#include "nix/store/store-api.hh"
#include "nix/store/active-builds.hh"

using namespace nix;

struct CmdPs : StoreCommand
{
    std::string description() override
    {
        return "list active builds";
    }

    Category category() override
    {
        return catUtility;
    }

    std::string doc() override
    {
        return
#include "ps.md"
            ;
    }

    void run(ref<Store> store) override
    {
        auto tracker = store.dynamic_pointer_cast<ActiveBuildsTracker>();

        if (!tracker)
            throw Error("Store does not support tracking active builds.");

        auto builds = tracker->queryBuilds();

        if (builds.empty()) {
            notice("No active builds.");
            return;
        }

        for (const auto & build : builds) {
            std::cout << fmt(
                ANSI_BOLD "%s" ANSI_NORMAL " (uid=%d)\n", store->printStorePath(build.derivation), build.mainUid);
            if (build.processes.empty())
                std::cout << fmt("%s%9d " ANSI_ITALIC "(no process info)" ANSI_NORMAL "\n", treeLast, build.mainPid);
            else {
                for (auto & process : build.processes) {
                    std::cout << fmt("%s%9d %s\n", treeLast, process.pid, concatStringsSep(" ", process.argv));
                }
            }
        }
    }
};

static auto rCmdPs = registerCommand2<CmdPs>({"ps"});
