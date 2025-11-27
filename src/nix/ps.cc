#include "nix/cmd/command.hh"
#include "nix/main/shared.hh"
#include "nix/store/store-api.hh"
#include "nix/store/store-cast.hh"
#include "nix/store/active-builds.hh"
#include "nix/util/terminal.hh"

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
        auto & tracker = require<QueryActiveBuildsStore>(*store);

        auto builds = tracker.queryActiveBuilds();

        if (builds.empty()) {
            notice("No active builds.");
            return;
        }

        for (const auto & build : builds) {
            std::cout << fmt(
                ANSI_BOLD "%s" ANSI_NORMAL " (uid=%d)\n", store->printStorePath(build.derivation), build.mainUid);
            if (build.processes.empty())
                std::cout << fmt(
                    "%s%9d %9d " ANSI_ITALIC "(no process info)" ANSI_NORMAL "\n", treeLast, build.mainPid);
            else {
                /* Recover the tree structure of the processes. */
                std::set<pid_t> pids;
                for (auto & process : build.processes)
                    pids.insert(process.pid);

                using Processes = std::set<const ActiveBuildInfo::ProcessInfo *>;
                std::map<pid_t, Processes> children;
                Processes rootProcesses;
                for (auto & process : build.processes) {
                    if (pids.contains(process.parentPid))
                        children[process.parentPid].insert(&process);
                    else
                        rootProcesses.insert(&process);
                }

                /* Render the process tree. */
                auto width = isTTY() ? getWindowWidth() : std::numeric_limits<unsigned int>::max();
                [&](this const auto & visit, const Processes & processes, std::string_view prefix) -> void {
                    for (const auto & [n, process] : enumerate(processes)) {
                        bool last = n + 1 == processes.size();
                        std::cout << filterANSIEscapes(
                            fmt("%s%s%d %s",
                                prefix,
                                last ? treeLast : treeConn,
                                process->pid,
                                // Use tokenizeString() to remove newlines / consecutive whitespace.
                                concatStringsSep(
                                    " ",
                                    tokenizeString<std::vector<std::string>>(concatStringsSep(" ", process->argv)))),
                            false,
                            width) << "\n";
                        visit(children[process->pid], last ? prefix + treeNull : prefix + treeLine);
                    }
                }(rootProcesses, "");
            }
        }
    }
};

static auto rCmdPs = registerCommand2<CmdPs>({"ps"});
