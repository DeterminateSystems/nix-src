#pragma once
///@file

#include "nix/flake/flake.hh"

#include <nlohmann/json_fwd.hpp>

namespace nix::flake {

/**
 * Parse a lock file in the old graph-based format (versions 5-7).
 * `json` must be null if the lock file doesn't exist.
 */
std::unique_ptr<LockedFlake> parseLockFileV7(
    const fetchers::Settings & fetchSettings, Flake flake, const nlohmann::json & json, std::string_view path);

/**
 * Compute a version 7 lock file for `flake`, reusing entries from
 * `oldLockFile` (which must have been produced by `parseLockFileV7()`)
 * where possible. Note: this does not write the new lock file.
 */
std::unique_ptr<LockedFlake> lockFlakeV7(
    const Settings & settings,
    EvalState & state,
    const LockFlags & lockFlags,
    Flake flake,
    const LockedFlake & oldLockFile);

} // namespace nix::flake
