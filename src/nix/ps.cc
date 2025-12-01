#include "nix/cmd/command.hh"
#include "nix/main/shared.hh"
#include "nix/store/store-api.hh"
#include "nix/store/store-cast.hh"
#include "nix/store/active-builds.hh"
#include "nix/util/table.hh"
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

        /* Helper to format user info: show name if available, else UID */
        auto formatUser = [](const UserInfo & user) -> std::string {
            return user.name ? *user.name : std::to_string(user.uid);
        };

        /* Print column headers. */
        std::cout << fmt("%9s %7s %5s %s\n", "USER", "PID", "CPU", "DERIVATION/COMMAND");

        for (const auto & build : builds) {
            std::cout << fmt(
                "%9s %7d %5s " ANSI_BOLD "%s" ANSI_NORMAL " (wall=%ds)\n",
                formatUser(build.mainUser),
                build.mainPid,
                build.cpuUser && build.cpuSystem
                    ? fmt("%ss",
                          std::chrono::duration_cast<std::chrono::seconds>(*build.cpuUser + *build.cpuSystem).count())
                    : "",
                store->printStorePath(build.derivation),
                time(nullptr) - build.startTime);
            if (build.processes.empty())
                std::cout << fmt(
                    "%9s %7d      %s" ANSI_ITALIC "(no process info)" ANSI_NORMAL "\n",
                    formatUser(build.mainUser),
                    build.mainPid,
                    treeLast);
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

                        // Format CPU time if available
                        std::string cpuInfo;
                        if (process->cpuUser && process->cpuSystem) {
                            auto totalCpu = *process->cpuUser + *process->cpuSystem;
                            auto totalSecs = std::chrono::duration_cast<std::chrono::seconds>(totalCpu).count();
                            cpuInfo = fmt("%ds", totalSecs);
                        }

                        // Format left-aligned info (user, pid, cpu)
                        auto leftInfo = fmt("%9s %7d %5s ", formatUser(process->user), process->pid, cpuInfo);

                        // Format argv with tree structure
                        auto argv = concatStringsSep(
                            " ", tokenizeString<std::vector<std::string>>(concatStringsSep(" ", process->argv)));

                        std::cout << filterANSIEscapes(
                            fmt("%s%s%s%s", leftInfo, prefix, last ? treeLast : treeConn, argv), false, width)
                                  << "\n";
                        visit(children[process->pid], last ? prefix + treeNull : prefix + treeLine);
                    }
                }(rootProcesses, "");
            }
        }
    }
};

static auto rCmdPs = registerCommand2<CmdPs>({"ps"});
