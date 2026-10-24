#pragma once
///@file

#include <sys/types.h>
#include <string>

#include "nix/util/configuration.hh"

namespace nix {
// Forward declarations
struct EvalSettings;

} // namespace nix

namespace nix::flake {

struct Settings : public Config
{
    Settings();

    void configureEvalSettings(nix::EvalSettings & evalSettings) const;

    Setting<bool> useRegistries{
        this, true, "use-registries", "Whether to use flake registries to resolve flake references.", {}, true};

    Setting<bool> acceptFlakeConfig{
        this,
        false,
        "accept-flake-config",
        "Whether to accept Nix configuration settings from a flake without prompting.",
        {},
        true};

    Setting<std::string> commitLockFileSummary{
        this,
        "",
        "commit-lock-file-summary",
        R"(
          The commit summary to use when committing changed flake lock files. If
          empty, the summary is generated based on the action performed.
        )",
        {"commit-lockfile-summary"},
        true};

    Setting<unsigned int> lockFileFormat{
        this,
        7,
        "lock-file-format",
        R"(
          The lock file format version to use when creating a new lock
          file (7 or 8). An existing lock file keeps its version
          unless `--recreate-lock-file` is passed. Note: version 8
          requires the `lock-file-v8` experimental feature.
        )",
        {},
        true};
};

} // namespace nix::flake
