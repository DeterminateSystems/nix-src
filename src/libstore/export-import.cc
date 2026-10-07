#include "nix/store/export-import.hh"
#include "nix/util/serialise.hh"
#include "nix/store/store-api.hh"
#include "nix/util/archive.hh"
#include "nix/store/common-protocol.hh"
#include "nix/store/common-protocol-impl.hh"
#include "nix/store/worker-protocol.hh"
#include "nix/store/worker-protocol-impl.hh"
#include "nix/store/names.hh"
#include "nix/util/thread-pool.hh"
#include "nix/util/zstd-patch.hh"

#include <cctype>

namespace nix {

static const uint32_t exportMagicV1 = 0x4558494e;
static const uint64_t exportMagicV2 = 0x324f4952414e; // = 'NARIO2'

/* Entry tags in version 2 narios. */
static const uint64_t narioTagEnd = 0;
static const uint64_t narioTagFull = 1;
static const uint64_t narioTagDiff = 2;
static const uint64_t narioTagPresent = 3;

/* Binary diff generation parameters. */

/* NARs larger than this are not diffed. */
static const uint64_t maxDiffNarSize = 1ULL << 30;

/* If a base differs in NAR size from the target by more than this
   factor, it's not considered as a diff base. This disambiguates
   e.g. between a package and a wrapper of that package. */
static const double maxDiffSizeRatio = 3.0;

/* A diff is only used if it's smaller than this fraction of the
   standalone zstd compression of the target NAR. Applying a zstd
   patch is cheap, so even a modest saving is worth it. */
static const double maxDiffFraction = 0.9;

static const int diffCompressionLevel = 19;

static const int baselineCompressionLevel = 3;

static WorkerProto::Version exportProtoVersion{
    .number =
        {
            .major = 1,
            .minor = 16,
        },
};

static void checkNarHash(Store & store, const ValidPathInfo & info, const Hash & hash)
{
    /* Refuse to export paths that have changed.  This prevents
       filesystem corruption from spreading to other machines.
       Don't complain if the stored hash is zero (unknown). */
    if (hash != info.narHash && info.narHash != Hash(info.narHash.algo))
        throw Error(
            "hash of path '%s' has changed from '%s' to '%s'!",
            store.printStorePath(info.path),
            info.narHash.to_string(HashFormat::Nix32, true),
            hash.to_string(HashFormat::Nix32, true));
}

/**
 * Return the key used to match store paths against potential diff
 * bases, namely the package name without the version, and the output
 * name, e.g. `openssl-3.0.13-dev` yields (`openssl`, `dev`).
 */
static std::pair<std::string, std::string> getDiffKey(std::string_view name)
{
    DrvName drvName(name);
    std::string outputName;
    auto i = drvName.version.rfind('-');
    if (i != std::string::npos && i + 1 < drvName.version.size() && isalpha(drvName.version[i + 1]))
        outputName = drvName.version.substr(i + 1);
    return {drvName.name, outputName};
}

/**
 * Return the length of the common prefix of two version strings, as
 * a crude measure of their similarity.
 */
static size_t versionSimilarity(std::string_view v1, std::string_view v2)
{
    return std::mismatch(v1.begin(), v1.end(), v2.begin(), v2.end()).first - v1.begin();
}

static double sizeRatio(uint64_t a, uint64_t b)
{
    if (a == 0 || b == 0)
        return a == b ? 1.0 : std::numeric_limits<double>::infinity();
    return a > b ? (double) a / b : (double) b / a;
}

struct NarioDiff
{
    StorePath basePath;
    Hash baseNarHash;
    std::string patch;
};

/**
 * Select the path in `baseClosure` that is most likely to produce a
 * small diff against each path in `paths` that is not in
 * `baseClosure`.
 */
static std::map<StorePath, StorePath>
selectDiffBases(Store & store, const StorePaths & paths, const StorePathSet & baseClosure)
{
    std::map<std::pair<std::string, std::string>, std::vector<StorePath>> basesByKey;
    for (auto & basePath : baseClosure)
        basesByKey[getDiffKey(basePath.name())].push_back(basePath);

    std::map<StorePath, StorePath> res;

    for (auto & path : paths) {
        if (baseClosure.contains(path))
            continue;

        auto i = basesByKey.find(getDiffKey(path.name()));
        if (i == basesByKey.end())
            continue;

        auto info = store.queryPathInfo(path);
        if (info->narSize > maxDiffNarSize)
            continue;

        auto version = DrvName(path.name()).version;

        std::optional<StorePath> bestPath;
        size_t bestVersionSimilarity = 0;
        double bestSizeRatio = 0;

        for (auto & basePath : i->second) {
            auto baseInfo = store.queryPathInfo(basePath);
            if (baseInfo->narSize > maxDiffNarSize || baseInfo->narSize + info->narSize > maxZstdPatchWindow())
                continue;
            auto ratio = sizeRatio(info->narSize, baseInfo->narSize);
            if (ratio >= maxDiffSizeRatio)
                continue;
            auto similarity = versionSimilarity(version, DrvName(basePath.name()).version);
            if (!bestPath || similarity > bestVersionSimilarity
                || (similarity == bestVersionSimilarity && ratio < bestSizeRatio)) {
                bestPath = basePath;
                bestVersionSimilarity = similarity;
                bestSizeRatio = ratio;
            }
        }

        if (bestPath)
            res.emplace(path, *bestPath);
    }

    return res;
}

/**
 * Compute a binary diff between the NARs of `basePath` and `path`.
 * Returns `std::nullopt` if the diff is not small enough to be worth
 * it.
 */
static std::optional<NarioDiff> makeDiff(Store & store, const StorePath & path, const StorePath & basePath)
{
    Activity act(
        *logger,
        lvlTalkative,
        actUnknown,
        fmt("computing diff of '%s' against '%s'", store.printStorePath(path), store.printStorePath(basePath)));

    auto getNar = [&](const ValidPathInfo & info) {
        StringSink nar;
        store.narFromPath(info.path, nar);
        auto hash = hashString(HashAlgorithm::SHA256, nar.s);
        checkNarHash(store, info, hash);
        return std::pair{std::move(nar.s), hash};
    };

    auto [baseNar, baseNarHash] = getNar(*store.queryPathInfo(basePath));
    auto [nar, narHash] = getNar(*store.queryPathInfo(path));

    auto patch = makeZstdPatch(baseNar, nar, diffCompressionLevel);
    auto baselineSize = makeZstdPatch("", nar, baselineCompressionLevel).size();

    printMsg(
        lvlTalkative,
        "diff of '%s' against '%s': %d bytes (NAR %d bytes, compressed %d bytes)",
        store.printStorePath(path),
        store.printStorePath(basePath),
        patch.size(),
        nar.size(),
        baselineSize);

    if (patch.size() >= maxDiffFraction * baselineSize)
        return std::nullopt;

    return NarioDiff{
        .basePath = basePath,
        .baseNarHash = baseNarHash,
        .patch = std::move(patch),
    };
}

void exportPaths(
    Store & store, const StorePathSet & paths, Sink & sink, unsigned int version, const StorePathSet & basePaths)
{
    auto sorted = store.topoSortPaths(paths);
    std::reverse(sorted.begin(), sorted.end());

    if (!basePaths.empty() && version != 2)
        throw Error("binary diffs are only supported in nario version 2");

    auto dumpNar = [&](const ValidPathInfo & info) {
        HashSink hashSink(HashAlgorithm::SHA256);
        TeeSink teeSink(sink, hashSink);

        store.narFromPath(info.path, teeSink);

        checkNarHash(store, info, hashSink.currentHash().hash);
    };

    switch (version) {

    case 1:
        for (auto & path : sorted) {
            sink << 1;
            auto info = store.queryPathInfo(path);
            dumpNar(*info);
            sink << exportMagicV1 << store.printStorePath(path);
            CommonProto::write(store, CommonProto::WriteConn{.to = sink}, info->references);
            sink << (info->deriver ? store.printStorePath(*info->deriver) : "") << 0;
        }
        sink << 0;
        break;

    case 2: {
        StorePathSet baseClosure;
        std::map<StorePath, NarioDiff> diffs;

        if (!basePaths.empty()) {
            store.computeFSClosure(basePaths, baseClosure);

            auto diffBases = selectDiffBases(store, sorted, baseClosure);

            Sync<std::map<StorePath, NarioDiff>> diffs_;
            ThreadPool pool;
            for (auto & [path, basePath] : diffBases)
                pool.enqueue([&, path, basePath]() {
                    if (auto diff = makeDiff(store, path, basePath))
                        diffs_.lock()->emplace(path, std::move(*diff));
                });
            pool.process();
            diffs = std::move(*diffs_.lock());
        }

        WorkerProto::WriteConn conn{.to = sink, .version = exportProtoVersion, .shortStorePaths = true};

        sink << exportMagicV2;

        for (auto & path : sorted) {
            auto info = store.queryPathInfo(path);

            if (baseClosure.contains(path)) {
                sink << narioTagPresent;
                WorkerProto::write(store, conn, *info);
                continue;
            }

            Activity act(*logger, lvlTalkative, actUnknown, fmt("exporting path '%s'", store.printStorePath(path)));

            if (auto diff = get(diffs, path)) {
                sink << narioTagDiff;
                WorkerProto::write(store, conn, *info);
                WorkerProto::write(store, conn, diff->basePath);
                sink << diff->baseNarHash.to_string(HashFormat::SRI, true) << diff->patch;
                continue;
            }

            sink << narioTagFull;
            // FIXME: move to CommonProto?
            WorkerProto::write(store, conn, *info);
            dumpNar(*info);
        }

        sink << narioTagEnd;
        break;
    }

    default:
        throw Error("unsupported nario version %d", version);
    }
}

NarioVisitor::~NarioVisitor() = default;

void parseNario(Store & store, Source & source, NarioVisitor & visitor)
{
    auto version = readNum<uint64_t>(source);

    /* Note: nario version 1 lacks an explicit header. The first
       integer denotes whether a store path follows or not. So look
       for 0 or 1. */
    switch (version) {

    case 0:
        /* Empty version 1 nario, nothing to do. */
        break;

    case 1: {
        /* Reuse a string buffer to avoid kernel overhead allocating
           memory for large strings. */
        StringSink saved;

        /* Non-empty version 1 nario. */
        while (true) {
            /* Extract the NAR from the source. */
            saved.s.clear();
            TeeSource tee{source, saved};
            NullFileSystemObjectSink ether;
            parseDump(ether, tee);

            uint32_t magic = readInt(source);
            if (magic != exportMagicV1)
                throw Error("nario cannot be imported; wrong format");

            auto path = store.parseStorePath(readString(source));

            auto references = CommonProto::Serialise<StorePathSet>::read(store, CommonProto::ReadConn{.from = source});
            auto deriver = readString(source);

            // Ignore optional legacy signature.
            if (readInt(source) == 1)
                readString(source);

            auto narHash = hashString(HashAlgorithm::SHA256, saved.s);

            ValidPathInfo info{path, {store, narHash}};
            if (deriver != "")
                info.deriver = store.parseStorePath(deriver);
            info.references = references;
            info.narSize = saved.s.size();

            // Can't use underlying source, which would have been exhausted.
            auto source2 = StringSource(saved.s);
            visitor.fullPath(info, source2);

            auto n = readNum<uint64_t>(source);
            if (n == 0)
                break;
            if (n != 1)
                throw Error("input doesn't look like a nario");
        }
        break;
    }

    case exportMagicV2: {
        WorkerProto::ReadConn conn{.from = source, .version = exportProtoVersion, .shortStorePaths = true};

        while (true) {
            auto tag = readNum<uint64_t>(source);
            if (tag == narioTagEnd)
                break;

            if (tag != narioTagFull && tag != narioTagDiff && tag != narioTagPresent)
                throw Error("input doesn't look like a nario");

            auto info = WorkerProto::Serialise<ValidPathInfo>::read(store, conn);

            if (tag == narioTagFull) {
                EnsureRead wrapper{source, info.narSize};
                visitor.fullPath(info, wrapper);
            }

            else if (tag == narioTagDiff) {
                auto basePath = WorkerProto::Serialise<StorePath>::read(store, conn);
                auto baseNarHash = Hash::parseAnyPrefixed(readString(source));
                auto patch = readString(source);
                visitor.diffPath(info, basePath, baseNarHash, patch);
            }

            else
                visitor.presentPath(info);
        }

        break;
    }

    default:
        throw Error("input doesn't look like a nario");
    }
}

StorePaths importPaths(Store & store, Source & source, CheckSigsFlag checkSigs)
{
    struct Importer : NarioVisitor
    {
        Store & store;
        CheckSigsFlag checkSigs;
        StorePaths res;

        Importer(Store & store, CheckSigsFlag checkSigs)
            : store(store)
            , checkSigs(checkSigs)
        {
        }

        void fullPath(const ValidPathInfo & info, Source & nar) override
        {
            if (!store.isValidPath(info.path)) {
                Activity act(
                    *logger, lvlTalkative, actUnknown, fmt("importing path '%s'", store.printStorePath(info.path)));

                store.addToStore(info, nar, NoRepair, checkSigs);
            }

            res.push_back(info.path);
        }

        void diffPath(
            const ValidPathInfo & info,
            const StorePath & basePath,
            const Hash & baseNarHash,
            std::string_view patch) override
        {
            if (!store.isValidPath(info.path)) {
                Activity act(
                    *logger,
                    lvlTalkative,
                    actUnknown,
                    fmt("importing path '%s' by patching '%s'",
                        store.printStorePath(info.path),
                        store.printStorePath(basePath)));

                if (!store.isValidPath(basePath))
                    throw Error(
                        "cannot import path '%s' because it's a binary diff against path '%s', which is not valid",
                        store.printStorePath(info.path),
                        store.printStorePath(basePath));

                StringSink baseNar;
                store.narFromPath(basePath, baseNar);

                auto actualBaseNarHash = hashString(baseNarHash.algo, baseNar.s);
                if (actualBaseNarHash != baseNarHash)
                    throw Error(
                        "cannot import path '%s' because it's a binary diff against path '%s', which has NAR hash '%s' instead of the expected '%s'",
                        store.printStorePath(info.path),
                        store.printStorePath(basePath),
                        actualBaseNarHash.to_string(HashFormat::SRI, true),
                        baseNarHash.to_string(HashFormat::SRI, true));

                auto nar = sinkToSource([&](Sink & sink) { applyZstdPatch(baseNar.s, patch, sink); });
                store.addToStore(info, *nar, NoRepair, checkSigs);
            }

            res.push_back(info.path);
        }

        void presentPath(const ValidPathInfo & info) override
        {
            auto localInfo = store.maybeQueryPathInfo(info.path);
            if (!localInfo)
                throw Error(
                    "nario requires path '%s' to already be valid in the store", store.printStorePath(info.path));

            if (localInfo->narHash != info.narHash && localInfo->narHash != Hash(localInfo->narHash.algo))
                throw Error(
                    "path '%s' in the store has NAR hash '%s', but the nario expects '%s'",
                    store.printStorePath(info.path),
                    localInfo->narHash.to_string(HashFormat::SRI, true),
                    info.narHash.to_string(HashFormat::SRI, true));

            res.push_back(info.path);
        }
    };

    Importer importer(store, checkSigs);
    parseNario(store, source, importer);
    return std::move(importer.res);
}

} // namespace nix
