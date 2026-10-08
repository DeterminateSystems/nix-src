#pragma once

#include "nix/store/store-api.hh"
#include "nix/util/compression-algo.hh"

namespace nix {

/**
 * Methods for selecting the base path against which to diff a path
 * being exported.
 */
enum class BaseSelectionMethod {
    /**
     * Select a base path with the same name (ignoring the version).
     */
    byName,
};

std::string showBaseSelectionMethod(BaseSelectionMethod method);

BaseSelectionMethod parseBaseSelectionMethod(std::string_view s);

struct NarioExportOptions
{
    /**
     * The nario format version (1 or 2).
     */
    unsigned int version;

    /**
     * If non-empty (only supported for version 2), the receiver is
     * assumed to have the closure of these paths. Paths in that
     * closure are not exported (but recorded as being expected to be
     * present), and other paths are exported as binary diffs against
     * a path in that closure if a suitable one is found.
     */
    StorePathSet basePaths;

    /**
     * How to find suitable base paths.
     */
    BaseSelectionMethod baseSelectionMethod = BaseSelectionMethod::byName;

    /**
     * If not `none` (only supported for version 2), NARs that are not
     * exported as binary diffs are compressed.
     */
    CompressionAlgo compression = CompressionAlgo::none;

    /**
     * The compression level to use for `compression`. If unset, the
     * default is 9 for zstd, and the compression method's default
     * otherwise.
     */
    std::optional<int> compressionLevel;
};

/**
 * Export multiple paths in the format expected by `nix-store
 * --import`. The paths will be sorted topologically.
 */
void exportPaths(Store & store, const StorePathSet & paths, Sink & sink, const NarioExportOptions & options);

/**
 * How a NAR is compressed inside a nario.
 */
struct NarioCompression
{
    CompressionAlgo algo;

    /**
     * Size of the compressed NAR.
     */
    uint64_t size;
};

/**
 * Binary diff algorithms supported in narios.
 */
enum class NarioDiffAlgo {
    /**
     * zstd compression of the target NAR, using the base NAR as a
     * prefix dictionary (see `makeZstdPatch()`).
     */
    zstd,
};

std::string showNarioDiffAlgo(NarioDiffAlgo algo);

NarioDiffAlgo parseNarioDiffAlgo(std::string_view s);

/**
 * Callbacks for the entries in a nario, used by `parseNario()`.
 */
struct NarioVisitor
{
    virtual ~NarioVisitor();

    /**
     * A path whose NAR is contained in the nario. The visitor must
     * read exactly `info.narSize` bytes from `nar`. If the NAR is
     * stored in compressed form, `compression` describes how; `nar`
     * returns the decompressed NAR in any case.
     */
    virtual void fullPath(const ValidPathInfo & info, Source & nar, std::optional<NarioCompression> compression) = 0;

    /**
     * A path whose NAR is given as a binary diff (computed using
     * `algo`) against the NAR of `basePath`, which is expected to
     * have NAR hash `baseNarHash`.
     */
    virtual void diffPath(
        const ValidPathInfo & info,
        NarioDiffAlgo algo,
        const StorePath & basePath,
        const Hash & baseNarHash,
        std::string_view patch) = 0;

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
