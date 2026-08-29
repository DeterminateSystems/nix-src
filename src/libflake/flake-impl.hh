#pragma once
///@file
/// Internal declarations shared between the lock file
/// implementations. Not part of the public libflake API.

#include "nix/flake/flakeref.hh"
#include "nix/flake/input-attr-path.hh"
#include "nix/util/source-path.hh"

namespace nix::flake {

/**
 * Warn against the use of indirect flakerefs (but only for top-level
 * inputs, since we don't want to annoy users about flakes that are
 * not under their control). `ref` is the input's flakeref as declared
 * and `resolvedRef` its registry-resolved counterpart; `topFlakePath`
 * is the `flake.nix` of the top-level flake.
 */
void warnRegistry(
    const InputAttrPath & inputAttrPath,
    const FlakeRef & ref,
    const FlakeRef & resolvedRef,
    const SourcePath & topFlakePath);

} // namespace nix::flake
