#pragma once
///@file

#include "nix/util/serialise.hh"

#include <string>
#include <string_view>

namespace nix {

/**
 * Compute a binary patch that transforms `base` into `target` using
 * HDiffPatch (https://github.com/sisong/HDiffPatch), compressed using
 * zstd at level `level`. The result is in HDiffPatch's "single
 * compressed stream" format and can be applied by
 * `applyHdiffPatch()` given the same `base`.
 *
 * `base` and `target` are taken by value because HDiffPatch
 * overwrites them during matching.
 */
std::string makeHdiffPatch(std::string base, std::string target, int level);

/**
 * Apply a patch produced by `makeHdiffPatch()` to `base`, writing the
 * reconstructed target to `sink`. The output is streamed, so only
 * `base` and `patch` need to be held in memory.
 */
void applyHdiffPatch(std::string_view base, std::string_view patch, Sink & sink);

} // namespace nix
