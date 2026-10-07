#pragma once

#include "nix/store/store-api.hh"

namespace nix {

/**
 * Export multiple paths in the format expected by `nix-store
 * --import`. The paths will be sorted topologically.
 *
 * If `basePaths` is non-empty (only supported for version 2), the
 * receiver is assumed to have the closure of `basePaths`. Paths in
 * that closure are not exported (but recorded as being expected to
 * be present), and other paths are exported as binary diffs against
 * a path in that closure if a suitable one is found.
 */
void exportPaths(
    Store & store, const StorePathSet & paths, Sink & sink, unsigned int version, const StorePathSet & basePaths = {});

/**
 * Callbacks for the entries in a nario, used by `parseNario()`.
 */
struct NarioVisitor
{
    virtual ~NarioVisitor();

    /**
     * A path whose NAR is contained in the nario. The visitor must
     * read exactly `info.narSize` bytes from `nar`.
     */
    virtual void fullPath(const ValidPathInfo & info, Source & nar) = 0;

    /**
     * A path whose NAR is given as a binary diff (see
     * `makeZstdPatch()`) against the NAR of `basePath`, which is
     * expected to have NAR hash `baseNarHash`.
     */
    virtual void diffPath(
        const ValidPathInfo & info, const StorePath & basePath, const Hash & baseNarHash, std::string_view patch) = 0;

    /**
     * A path that is not contained in the nario, but that is expected
     * to already be present in the destination store.
     */
    virtual void presentPath(const ValidPathInfo & info) = 0;
};

/**
 * Parse a nario created by `exportPaths()`, calling `visitor` for
 * each entry.
 */
void parseNario(Store & store, Source & source, NarioVisitor & visitor);

/**
 * Import a sequence of NAR dumps created by `exportPaths()` into the
 * Nix store.
 */
StorePaths importPaths(Store & store, Source & source, CheckSigsFlag checkSigs = CheckSigs);

} // namespace nix
