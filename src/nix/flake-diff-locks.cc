#include "flake-command.hh"
#include "nix/expr/eval.hh"
#include "nix/fetchers/fetch-settings.hh"

#include <filesystem>
#include <nlohmann/json.hpp>

namespace nix {

using namespace flake;

struct CmdFlakeDiffLocks : EvalCommand
{
    std::string oldFlakeUrl, newFlakeUrl = ".";

    bool transitive = false;

    CmdFlakeDiffLocks()
    {
        expectArgs(
            {.label = "old-flake",
             .handler = {&oldFlakeUrl},
             .completer = {[&](AddCompletions & completions, size_t, std::string_view prefix) {
                 completeFlakeRef(completions, getStore(), prefix);
             }}});

        expectArgs(
            {.label = "new-flake",
             .optional = true,
             .handler = {&newFlakeUrl},
             .completer = {[&](AddCompletions & completions, size_t, std::string_view prefix) {
                 completeFlakeRef(completions, getStore(), prefix);
             }}});

        addFlag({
            .longName = "transitive",
            .description = "Include the transitive locks of inputs that have a lock file of their own. "
                           "This may require fetching those inputs.",
            .handler = {&transitive, true},
        });
    }

    std::string description() override
    {
        return "show the differences between the lock files of two flakes";
    }

    std::string doc() override
    {
        return
#include "flake-diff-locks.md"
            ;
    }

    void run(nix::ref<nix::Store> store) override
    {
        auto state = getEvalState();

        auto readLockedFlake = [&](const std::string & flakeUrl) {
            auto flake = getFlake(
                *state,
                parseFlakeRef(fetchSettings, flakeUrl, std::filesystem::current_path().string()),
                fetchers::UseRegistries::All,
                false);

            nlohmann::json json;

            auto lockFilePath = flake.lockFilePath();
            if (lockFilePath.pathExists()) {
                try {
                    json = nlohmann::json::parse(lockFilePath.readFile());
                } catch (const nlohmann::json::parse_error & e) {
                    throw Error("Could not parse '%s': %s", lockFilePath, e.what());
                }
            } else
                warn("flake '%s' does not have a lock file", flake.originalRef);

            return parseLockFile(state->fetchSettings, std::move(flake), json, fmt("%s", lockFilePath));
        };

        auto oldLockedFlake = readLockedFlake(oldFlakeUrl);
        auto newLockedFlake = readLockedFlake(newFlakeUrl);

        auto diff = diffLockedFlakes(*oldLockedFlake, *newLockedFlake, transitive);

        if (diff.empty())
            logger->cout("No changes.");
        else
            logger->cout("%s", chomp(diff));
    }
};

static auto rCmdFlakeDiffLocks = registerCommand2<CmdFlakeDiffLocks>({"flake", "diff-locks"});

} // namespace nix
