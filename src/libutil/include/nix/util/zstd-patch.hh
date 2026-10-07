#pragma once
///@file

#include "nix/util/serialise.hh"

#include <string>
#include <string_view>

namespace nix {

/**
 * Compute a binary patch that transforms `base` into `target`, using
 * zstd's "patch-from" mode (i.e. `base` is used as a prefix
 * dictionary). The result is a single zstd frame that can be
 * decompressed by `applyZstdPatch()` given the same `base`.
 *
 * With an empty `base`, this is just ordinary zstd compression of
 * `target`.
 */
std::string makeZstdPatch(std::string_view base, std::string_view target, int level);

/**
 * Apply a patch produced by `makeZstdPatch()` to `base`, writing the
 * reconstructed target to `sink`. The output is streamed, so only
 * `base` and `patch` need to be held in memory.
 */
void applyZstdPatch(std::string_view base, std::string_view patch, Sink & sink);

/**
 * The largest combined size of base and target that a zstd patch can
 * fully cover (i.e. the maximum zstd window size).
 */
uint64_t maxZstdPatchWindow();

} // namespace nix
