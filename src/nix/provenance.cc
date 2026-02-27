#include "nix/cmd/command.hh"
#include "nix/store/store-api.hh"
#include "nix/store/store-open.hh"
#include "nix/expr/provenance.hh"
#include "nix/store/provenance.hh"
#include "nix/flake/provenance.hh"
#include "nix/fetchers/provenance.hh"
#include "nix/util/provenance.hh"
#include "nix/util/json-utils.hh"
#include "nix/fetchers/fetch-to-store.hh"
#include "nix/util/exit.hh"
#include "nix/cmd/installable-flake.hh"
#include "nix/store/derivations.hh"
#include "nix/store/filetransfer.hh"
#include "nix/util/callback.hh"

#include <memory>
#include <string>
#include <nlohmann/json.hpp>
#include <boost/unordered/concurrent_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#define TAB "    "

using namespace nix;

struct CmdProvenance : NixMultiCommand
{
    CmdProvenance()
        : NixMultiCommand("provenance", RegisterCommand::getCommandsFor({"provenance"}))
    {
    }

    std::string description() override
    {
        return "query and check the provenance of store paths";
    }

    std::optional<ExperimentalFeature> experimentalFeature() override
    {
        return Xp::Provenance;
    }

    Category category() override
    {
        return catUtility;
    }
};

static auto rCmdProvenance = registerCommand<CmdProvenance>("provenance");

struct CmdProvenanceShow : StorePathsCommand
{
    std::string description() override
    {
        return "show the provenance chain of store paths";
    }

    std::string doc() override
    {
        return
#include "provenance-show.md"
            ;
    }

    void displayProvenance(Store & store, const StorePath & path, std::shared_ptr<const Provenance> provenance)
    {
        while (provenance) {
            if (auto copied = std::dynamic_pointer_cast<const CopiedProvenance>(provenance)) {
                logger->cout("← copied from " ANSI_BOLD "%s" ANSI_NORMAL, copied->from);
                provenance = copied->next;
            }

            else if (auto build = std::dynamic_pointer_cast<const BuildProvenance>(provenance)) {
                logger->cout(
                    "← built from derivation " ANSI_BOLD "%s" ANSI_NORMAL " (output " ANSI_BOLD "%s" ANSI_NORMAL
                    ") on " ANSI_BOLD "%s" ANSI_NORMAL " for " ANSI_BOLD "%s" ANSI_NORMAL,
                    store.printStorePath(build->drvPath),
                    build->output,
                    build->buildHost.value_or("unknown host").c_str(),
                    build->system);
                for (auto & [tagName, tagValue] : build->tags)
                    logger->cout("  tag " ANSI_BOLD "%s" ANSI_NORMAL ": %s", tagName, tagValue);
                provenance = build->next;
            }

            else if (auto flake = std::dynamic_pointer_cast<const FlakeProvenance>(provenance)) {
                // Collapse subpath/tree provenance into the flake provenance for legibility.
                auto next = flake->next;
                CanonPath flakePath("/flake.nix");
                if (auto subpath = std::dynamic_pointer_cast<const SubpathProvenance>(next)) {
                    next = subpath->next;
                    flakePath = subpath->subpath;
                }
                if (auto tree = std::dynamic_pointer_cast<const TreeProvenance>(next)) {
                    FlakeRef flakeRef(
                        fetchers::Input::fromAttrs(fetchSettings, fetchers::jsonToAttrs(*tree->attrs)),
                        Path(flakePath.parent().value_or(CanonPath::root).rel()));
                    logger->cout(
                        "← %sinstantiated from %sflake output " ANSI_BOLD "%s#%s" ANSI_NORMAL,
                        flake->pure ? "" : ANSI_RED "impurely" ANSI_NORMAL " ",
                        flakeRef.input.isLocked(fetchSettings) ? "" : ANSI_RED "unlocked" ANSI_NORMAL " ",
                        flakeRef.to_string(),
                        flake->flakeOutput);
                    break;
                } else {
                    logger->cout("← instantiated from flake output " ANSI_BOLD "%s" ANSI_NORMAL, flake->flakeOutput);
                    provenance = flake->next;
                }
            }

            else if (auto tree = std::dynamic_pointer_cast<const TreeProvenance>(provenance)) {
                auto input = fetchers::Input::fromAttrs(fetchSettings, fetchers::jsonToAttrs(*tree->attrs));
                logger->cout(
                    "← from %stree " ANSI_BOLD "%s" ANSI_NORMAL,
                    input.isLocked(fetchSettings) ? "" : ANSI_RED "unlocked" ANSI_NORMAL " ",
                    input.to_string());
                break;
            }

            else if (auto subpath = std::dynamic_pointer_cast<const SubpathProvenance>(provenance)) {
                logger->cout("← from file " ANSI_BOLD "%s" ANSI_NORMAL, subpath->subpath.abs());
                provenance = subpath->next;
            }

            else if (auto drv = std::dynamic_pointer_cast<const DerivationProvenance>(provenance)) {
                logger->cout("← with derivation metadata");
                std::istringstream stream((*drv->meta).dump(2));
                for (std::string line; std::getline(stream, line);) {
                    logger->cout("  %s", line);
                }
                provenance = drv->next;
            }

            else if (auto fetchurl = std::dynamic_pointer_cast<const FetchurlProvenance>(provenance)) {
                logger->cout("← fetched from URL " ANSI_BOLD "%s" ANSI_NORMAL, fetchurl->url);
                break;
            }

            else {
                // Unknown or unhandled provenance type
                auto json = provenance->to_json();
                auto typeIt = json.find("type");
                if (typeIt != json.end() && typeIt->is_string())
                    logger->cout("← " ANSI_RED "unknown provenance type '%s'" ANSI_NORMAL, typeIt->get<std::string>());
                else
                    logger->cout("← " ANSI_RED "unknown provenance type" ANSI_NORMAL);
                break;
            }
        }
    }

    void run(ref<Store> store, StorePaths && storePaths) override
    {
        bool first = true;

        for (auto & storePath : storePaths) {
            auto info = store->queryPathInfo(storePath);
            if (!first)
                logger->cout("");
            first = false;
            logger->cout(ANSI_BOLD "%s" ANSI_NORMAL, store->printStorePath(info->path));

            if (info->provenance)
                displayProvenance(*store, storePath, info->provenance);
            else
                logger->cout(ANSI_RED "  (no provenance information available)" ANSI_NORMAL);
        }
    }
};

static auto rCmdProvenanceShow = registerCommand2<CmdProvenanceShow>({"provenance", "show"});

/**
 * A wrapper around an arbitrary store that intercepts `addToStore()`
 * and `addToStoreFromDump()` calls to keep track of added paths.
 */
struct TrackingStore : public Store
{
    ref<Store> next;
    boost::unordered_flat_set<StorePath> instantiatedPaths;

    TrackingStore(ref<Store> next)
        : Store(next->config)
        , next(next)
    {
    }

    void addToStore(const ValidPathInfo & info, Source & narSource, RepairFlag repair, CheckSigsFlag checkSigs) override
    {
        next->addToStore(info, narSource, repair, checkSigs);
        instantiatedPaths.insert(info.path);
        // FIXME: we should really just disable the path info cache, since the underlying store already does caching.
        invalidatePathInfoCacheFor(info.path);
    }

    StorePath addToStore(
        std::string_view name,
        const SourcePath & path,
        ContentAddressMethod method,
        HashAlgorithm hashAlgo,
        const StorePathSet & references,
        PathFilter & filter,
        RepairFlag repair) override
    {
        auto storePath = next->addToStore(name, path, method, hashAlgo, references, filter, repair);
        instantiatedPaths.insert(storePath);
        invalidatePathInfoCacheFor(storePath);
        return storePath;
    }

    StorePath addToStoreFromDump(
        Source & dump,
        std::string_view name,
        FileSerialisationMethod dumpMethod,
        ContentAddressMethod hashMethod,
        HashAlgorithm hashAlgo,
        const StorePathSet & references,
        RepairFlag repair,
        std::shared_ptr<const Provenance> provenance) override
    {
        auto storePath =
            next->addToStoreFromDump(dump, name, dumpMethod, hashMethod, hashAlgo, references, repair, provenance);
        instantiatedPaths.insert(storePath);
        invalidatePathInfoCacheFor(storePath);
        return storePath;
    }

    void queryPathInfoUncached(
        const StorePath & path, Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept override
    {
        try {
            callback(std::make_shared<ValidPathInfo>(*next->queryPathInfo(path)));
        } catch (InvalidPath &) {
            callback(nullptr);
        } catch (...) {
            callback.rethrow();
        }
    }

    void queryRealisationUncached(
        const DrvOutput & output, Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept override
    {
        next->queryRealisation(output, std::move(callback));
    }

    std::optional<StorePath> queryPathFromHashPart(const std::string & hashPart) override
    {
        return next->queryPathFromHashPart(hashPart);
    }

    void registerDrvOutput(const Realisation & output) override
    {
        next->registerDrvOutput(output);
    }

    ref<SourceAccessor> getFSAccessor(bool requireValidPath) override
    {
        return next->getFSAccessor(requireValidPath);
    }

    std::shared_ptr<SourceAccessor> getFSAccessor(const StorePath & path, bool requireValidPath) override
    {
        return next->getFSAccessor(path, requireValidPath);
    }

    std::optional<TrustedFlag> isTrustedClient() override
    {
        return next->isTrustedClient();
    }
};

struct CmdProvenanceVerify : StorePathsCommand
{
    bool noRebuild = false;

    CmdProvenanceVerify()
    {
        addFlag({
            .longName = "no-rebuild",
            .description = "Skip rebuilding derivations to verify reproducibility.",
            .handler = {&noRebuild, true},
        });
    }

    std::string description() override
    {
        return "verify the provenance of store paths";
    }

    std::string doc() override
    {
        return
#include "provenance-verify.md"
            ;
    }

    bool verifySourcePath(Store & store, const StorePath & expectedPath, const SourcePath & sourcePath)
    {
        auto computedPath = fetchToStore2(fetchSettings, store, sourcePath, FetchMode::Copy, expectedPath.name()).first;
        if (computedPath != expectedPath) {
            logger->cout(
                "❌ " ANSI_RED "store path mismatch for source '%s': expected '%s' but got '%s'" ANSI_NORMAL,
                sourcePath.to_string(),
                store.printStorePath(expectedPath),
                store.printStorePath(computedPath));
            return false;
        } else {
            logger->cout("✅ verified store path for source '%s'", sourcePath.to_string());
            return true;
        }
    }

    using CheckResult = std::variant<
        std::pair<fetchers::Input, ref<SourceAccessor>>,
        std::pair<fetchers::Input, SourcePath>,
        std::monostate>;

    std::pair<bool, CheckResult>
    verify(Store & store, std::optional<StorePath> path, std::shared_ptr<const Provenance> provenance)
    {
        if (auto copied = std::dynamic_pointer_cast<const CopiedProvenance>(provenance)) {
            if (!path) {
                logger->cout("❌ " ANSI_RED "cannot verify copied provenance without a store path" ANSI_NORMAL);
                return {false, std::monostate{}};
            }
            bool success = true;
            auto fromStore = openStore(copied->from);
            auto localInfo = store.queryPathInfo(*path);
            auto fromInfo = fromStore->queryPathInfo(*path);
            if (localInfo->narHash != fromInfo->narHash) {
                logger->cout(
                    "❌ " ANSI_RED "NAR hash mismatch in origin store '%s': should be '%s' but is '%s'" ANSI_NORMAL,
                    copied->from,
                    localInfo->narHash.to_string(HashFormat::SRI, true),
                    fromInfo->narHash.to_string(HashFormat::SRI, true));
                success = false;
            } else
                logger->cout("✅ verified NAR hash in origin store '%s'", copied->from);
            auto [nextSuccess, result] = verify(store, path, copied->next);
            return {success && nextSuccess, std::move(result)};
        }

        else if (auto build = std::dynamic_pointer_cast<const BuildProvenance>(provenance)) {
            auto success = verify(store, build->drvPath, build->next).first;

            // Verify that `path` is the expected output of the derivation.
            auto outputMap = store.queryPartialDerivationOutputMap(build->drvPath);
            auto it = outputMap.find(build->output);
            if (it == outputMap.end()) {
                logger->cout(
                    "❌ " ANSI_RED "derivation '%s' does not have expected output '%s'" ANSI_NORMAL,
                    store.printStorePath(build->drvPath),
                    build->output);
                return {false, std::monostate{}};
            } else if (!it->second) {
                // Note: this is not an error, should we even print a message?
                logger->cout(
                    "❓ output '%s' of derivation '%s' is not statically known",
                    build->output,
                    store.printStorePath(build->drvPath));
            } else if (*it->second != path) {
                logger->cout(
                    "❌ " ANSI_RED "output '%s' of derivation '%s' is '%s', expected '%s'" ANSI_NORMAL,
                    build->output,
                    store.printStorePath(build->drvPath),
                    store.printStorePath(*it->second),
                    store.printStorePath(*path));
                return {false, std::monostate{}};
            }

            // Do a check rebuild to verify that the derivation
            // produces the same output.
            if (noRebuild) {
                logger->cout(
                    "⏭️ skipped rebuild of derivation '%s^%s'", store.printStorePath(build->drvPath), build->output);
            } else {
                try {
                    store.buildPaths(
                        {DerivedPath::Built{
                            .drvPath = make_ref<const SingleDerivedPath>(SingleDerivedPath::Opaque{build->drvPath}),
                            .outputs = OutputsSpec::Names{build->output},
                        }},
                        bmCheck);
                    logger->cout("✅ rebuilt derivation '%s^%s'", store.printStorePath(build->drvPath), build->output);
                } catch (Error & e) {
                    logger->cout(
                        "❌ " ANSI_RED "rebuild of derivation '%s^%s' failed: %s" ANSI_NORMAL,
                        store.printStorePath(build->drvPath),
                        build->output,
                        e.what());
                    success = false;
                }
            }

            return {success, std::monostate{}};
        }

        else if (auto flake = std::dynamic_pointer_cast<const FlakeProvenance>(provenance)) {
            // Fetch the flake source.
            auto [success, _res] = verify(store, {}, flake->next);

            auto res = std::get_if<std::pair<fetchers::Input, SourcePath>>(&_res);
            if (!res)
                return {false, std::monostate{}};

            // Evaluate the flake output.
            flake::LockFlags lockFlags{
                .updateLockFile = false,
                .failOnUnlocked = true,
                .useRegistries = false,
                .allowUnlocked = false,
            };

            if (res->second.path.baseName() != "flake.nix") {
                logger->cout(
                    "❌ " ANSI_RED "expected flake source to be a 'flake.nix' file, but got '%s'" ANSI_NORMAL,
                    res->second.path.abs());
                return {false, std::monostate{}};
            }

            auto trackingStore = make_ref<TrackingStore>(getEvalStore());

            auto evalState =
                ref(std::allocate_shared<EvalState>(
                    traceable_allocator<EvalState>(),
                    LookupPath{},
                    ref<Store>(trackingStore),
                    fetchSettings,
                    evalSettings,
                    getStore()));

            InstallableFlake installable{
                nullptr,
                evalState,
                FlakeRef{std::move(res->first), std::string(res->second.path.parent().value().rel())},
                "." + flake->flakeOutput,
                ExtendedOutputsSpec::Default{}, // FIXME: record this in the provenance?
                {},
                lockFlags,
                {}};

            // We have to disable the eval cache to ensure that we see which store paths get instantiated.
            installable.useEvalCache = false;

            installable.toDerivedPaths();

            evalState->waitForAllPaths();

            logger->cout("✅ evaluated '%s#%s'", installable.flakeRef.to_string(true), flake->flakeOutput);

            if (path) {
                if (!trackingStore->instantiatedPaths.contains(*path)) {
                    logger->cout(
                        "❌ " ANSI_RED "evaluation did not re-instantiate path '%s'" ANSI_NORMAL,
                        store.printStorePath(*path));
                    return {false, std::monostate{}};
                }

                logger->cout("✅ re-instantiated path '%s'", store.printStorePath(*path));
            }

            return {success, std::monostate{}};
        }

        else if (auto tree = std::dynamic_pointer_cast<const TreeProvenance>(provenance)) {
            auto input = fetchers::Input::fromAttrs(fetchSettings, fetchers::jsonToAttrs(*tree->attrs));
            try {
                auto [accessor, final] = input.getAccessor(fetchSettings, store);
                if (!input.isLocked(fetchSettings))
                    logger->cout("❓ fetched tree '%s', but it's unlocked", input.to_string());
                else
                    // FIXME: check NAR hash?
                    logger->cout("✅ fetched tree '%s'", input.to_string());

                bool success = !path || verifySourcePath(store, *path, SourcePath(accessor, CanonPath::root));

                return {success, std::make_pair(std::move(final), accessor)};
            } catch (Error & e) {
                logger->cout("❌ " ANSI_RED "failed to fetch tree '%s': %s" ANSI_NORMAL, input.to_string(), e.what());
                return {false, std::monostate{}};
            }
        }

        else if (auto subpath = std::dynamic_pointer_cast<const SubpathProvenance>(provenance)) {
            auto [success, result] = verify(store, {}, subpath->next);
            if (auto p = std::get_if<std::pair<fetchers::Input, ref<SourceAccessor>>>(&result)) {

                auto sourcePath = SourcePath(p->second, subpath->subpath);

                if (path && !verifySourcePath(store, *path, sourcePath))
                    success = false;

                return {success, std::make_pair(std::move(p->first), std::move(sourcePath))};
            } else
                return {false, std::monostate{}};
        }

        else if (auto drv = std::dynamic_pointer_cast<const DerivationProvenance>(provenance))
            return verify(store, path, drv->next);

        else if (auto fetchurl = std::dynamic_pointer_cast<const FetchurlProvenance>(provenance)) {
            if (!path)
                return {false, std::monostate{}};

            auto info = store.queryPathInfo(*path);

            if (!info->ca) {
                logger->cout(
                    "❌ " ANSI_RED "cannot verify URL '%s' without a content address for path '%s'" ANSI_NORMAL,
                    fetchurl->url,
                    store.printStorePath(*path));
                return {false, std::monostate{}};
            }

            if (info->ca->method != ContentAddressMethod::Raw::Flat) {
                logger->cout(
                    "❌ " ANSI_RED
                    "cannot verify URL '%s' with unsupported content address method for path '%s'" ANSI_NORMAL,
                    fetchurl->url,
                    store.printStorePath(*path));
                return {false, std::monostate{}};
            }

            HashSink hashSink{info->ca->hash.algo};
            FileTransferRequest req(fetchurl->url);
            req.decompress = false;
            getFileTransfer()->download(std::move(req), hashSink);
            auto hash = hashSink.finish().hash;

            if (hash != info->ca->hash) {
                logger->cout(
                    "❌ " ANSI_RED "hash mismatch for URL '%s': expected '%s' but got '%s'" ANSI_NORMAL,
                    fetchurl->url,
                    info->ca->hash.to_string(HashFormat::SRI, true),
                    hash.to_string(HashFormat::SRI, true));
                return {false, std::monostate{}};
            }

            logger->cout("✅ verified hash of URL '%s'", fetchurl->url);
            return {true, std::monostate{}};
        }

        else if (!provenance) {
            logger->cout("❓ " ANSI_RED "missing further provenance" ANSI_NORMAL);
            return {false, std::monostate{}};
        }

        else {
            logger->cout("❓ " ANSI_RED "unknown provenance type" ANSI_NORMAL);
            return {false, std::monostate{}};
        }
    }

    void run(ref<Store> store, StorePaths && storePaths) override
    {
        bool first = true;
        bool success = true;

        for (auto & storePath : storePaths) {
            auto info = store->queryPathInfo(storePath);
            if (!first)
                logger->cout("");
            first = false;
            logger->cout(ANSI_BOLD "%s" ANSI_NORMAL, store->printStorePath(info->path));

            if (info->provenance)
                success &= verify(*store, storePath, info->provenance).first;
            else {
                logger->cout(ANSI_RED "  (no provenance information available)" ANSI_NORMAL);
                success = false;
            }
        }

        if (!success)
            throw Exit(1);
    }
};

static auto rCmdProvenanceVerify = registerCommand2<CmdProvenanceVerify>({"provenance", "verify"});
