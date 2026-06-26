#include "nix/store/derivations.hh"
#include "nix/util/fun.hh"
#include "nix/store/parsed-derivations.hh"
#include "nix/store/derivation-options.hh"
#include "nix/store/globals.hh"
#include "nix/store/store-open.hh"
#include "nix/store/nar-info.hh"
#include "nix/util/thread-pool.hh"
#include "nix/store/realisation.hh"
#include "nix/util/topo-sort.hh"
#include "nix/util/callback.hh"
#include "nix/util/closure.hh"
#include "nix/store/filetransfer.hh"
#include "nix/util/strings.hh"
#include "nix/util/json-utils.hh"

#include <boost/unordered/unordered_flat_set.hpp>

namespace nix {

void Store::computeFSClosure(
    const StorePathSet & startPaths,
    StorePathSet & paths_,
    bool flipDirection,
    bool includeOutputs,
    bool includeDerivers)
{
    std::function<asio::awaitable<StorePathSet>(const StorePath & path)> queryDeps;
    if (flipDirection)
        queryDeps = [this, includeOutputs, includeDerivers](const StorePath & path) -> asio::awaitable<StorePathSet> {
            StorePathSet res;
            StorePathSet referrers;
            queryReferrers(path, referrers);
            for (auto & ref : referrers)
                if (ref != path)
                    res.insert(ref);

            if (includeOutputs)
                for (auto & i : queryValidDerivers(path))
                    res.insert(i);

            if (includeDerivers && path.isDerivation())
                for (auto & [_, maybeOutPath] : queryPartialDerivationOutputMap(path))
                    if (maybeOutPath && isValidPath(*maybeOutPath))
                        res.insert(*maybeOutPath);
            co_return res;
        };
    else
        queryDeps = [this, includeOutputs, includeDerivers](const StorePath & path) -> asio::awaitable<StorePathSet> {
            StorePathSet res;
            auto info = co_await callbackToAwaitable<ref<const ValidPathInfo>>(
                [this, path](Callback<ref<const ValidPathInfo>> cb) { queryPathInfo(path, std::move(cb)); });

            for (auto & ref : info->references)
                if (ref != path)
                    res.insert(ref);

            if (includeOutputs && path.isDerivation())
                for (auto & [_, maybeOutPath] : queryPartialDerivationOutputMap(path))
                    if (maybeOutPath && isValidPath(*maybeOutPath))
                        res.insert(*maybeOutPath);

            if (includeDerivers && info->deriver && isValidPath(*info->deriver))
                res.insert(*info->deriver);
            co_return res;
        };

    computeClosure<StorePath>(startPaths, paths_, GetEdgesAsync<StorePath>(queryDeps));
}

void Store::computeFSClosure(
    const StorePath & startPath, StorePathSet & paths_, bool flipDirection, bool includeOutputs, bool includeDerivers)
{
    StorePathSet paths;
    paths.insert(startPath);
    computeFSClosure(paths, paths_, flipDirection, includeOutputs, includeDerivers);
}

const ContentAddress * getDerivationCA(const BasicDerivation & drv)
{
    auto out = drv.outputs.find("out");
    if (out == drv.outputs.end())
        return nullptr;
    if (auto dof = std::get_if<DerivationOutput::CAFixed>(&out->second.raw)) {
        return &dof->ca;
    }
    return nullptr;
}

static asio::awaitable<void>
querySubstitutablePathInfosAsync(Store & store, const StorePathCAMap & paths, SubstitutablePathInfos & infos)
{
    if (!settings.getWorkerSettings().useSubstitutes)
        co_return;

    co_await forEachAsync(paths, [&store, &infos](auto path) -> asio::awaitable<void> {
        std::optional<Error> lastStoresException = std::nullopt;
        for (auto & sub : getDefaultSubstituters()) {
            if (lastStoresException.has_value()) {
                logError(lastStoresException->info());
                lastStoresException.reset();
            }

            auto subPath(path.first);

            // Recompute store path so that we can use a different store root.
            if (path.second) {
                subPath = store.makeFixedOutputPathFromCA(
                    path.first.name(), ContentAddressWithReferences::withoutRefs(*path.second));
                if (sub->storeDir == store.storeDir)
                    assert(subPath == path.first);
                if (subPath != path.first)
                    debug(
                        "replaced path '%s' with '%s' for substituter '%s'",
                        store.printStorePath(path.first),
                        sub->printStorePath(subPath),
                        sub->config.getHumanReadableURI());
            } else if (sub->storeDir != store.storeDir)
                continue;

            debug(
                "checking substituter '%s' for path '%s'",
                sub->config.getHumanReadableURI(),
                sub->printStorePath(subPath));
            try {
                auto info = co_await callbackToAwaitable<ref<const ValidPathInfo>>(
                    [subPath, &sub](Callback<ref<const ValidPathInfo>> cb) {
                        sub->queryPathInfo(subPath, std::move(cb));
                    });

                if (sub->storeDir != store.storeDir && !(info->isContentAddressed(*sub) && info->references.empty()))
                    continue;

                auto narInfo = std::dynamic_pointer_cast<const NarInfo>(std::shared_ptr<const ValidPathInfo>(info));
                infos.insert_or_assign(
                    path.first,
                    SubstitutablePathInfo{
                        .deriver = info->deriver,
                        .references = info->references,
                        .downloadSize = narInfo ? narInfo->fileSize : 0,
                        .narSize = info->narSize,
                        .partialClosure = narInfo ? narInfo->partialClosure : StorePathSet{},
                    });

                break; /* We are done. */
            } catch (InvalidPath &) {
            } catch (SubstituterDisabled &) {
            } catch (Error & e) {
                lastStoresException = std::make_optional(std::move(e));
            }
        }
        if (lastStoresException.has_value()) {
            if (!settings.getWorkerSettings().tryFallback) {
                throw *lastStoresException;
            } else
                logError(lastStoresException->info());
        }
    });
}

// FIXME: remove this, queryMissing() no longer uses it.
void Store::querySubstitutablePathInfos(const StorePathCAMap & paths, SubstitutablePathInfos & infos)
{
    asio::io_context ctx;
    std::exception_ptr ex;
    asio::co_spawn(ctx, querySubstitutablePathInfosAsync(*this, paths, infos), [&](std::exception_ptr e) { ex = e; });
    ctx.run();
    if (ex)
        std::rethrow_exception(ex);
}

MissingPaths Store::queryMissing(const std::vector<DerivedPath> & targets)
{
    Activity act(*logger, lvlDebug, actUnknown, "querying info about missing paths");

    MissingPaths res;

    auto collectDerivedPaths = [&](this auto & collectDerivedPaths,
                                   std::set<DerivedPath> & out,
                                   ref<SingleDerivedPath> inputDrv,
                                   const DerivedPathMap<StringSet>::ChildNode & node) -> void {
        if (!node.value.empty())
            out.insert(DerivedPath::Built{inputDrv, node.value});
        for (const auto & [outputName, childNode] : node.childMap)
            collectDerivedPaths(
                out, make_ref<SingleDerivedPath>(SingleDerivedPath::Built{inputDrv, outputName}), childNode);
    };

    auto mustBuildDrv = [&](const StorePath & drvPath, const Derivation & drv, std::set<DerivedPath> & out) {
        res.willBuild.insert(drvPath);
        for (const auto & [inputDrv, inputNode] : drv.inputDrvs.map)
            collectDerivedPaths(out, makeConstantStorePathRef(inputDrv), inputNode);
    };

    asio::io_context ctx;
    std::exception_ptr ex;

    std::set<DerivedPath> done;

    auto subs = getDefaultSubstituters();

    std::function<asio::awaitable<void>(std::set<DerivedPath>)> doPaths;
    doPaths = [&](std::set<DerivedPath> paths) -> asio::awaitable<void> {
        debug("working on batch of %d paths", paths.size());

        std::set<StorePath> pathsToQuery;

        std::map<StorePath, std::map<StorePath, ref<Derivation>>> outPathsToDrvs;

        while (!paths.empty()) {
            auto p = *paths.begin();
            paths.erase(paths.begin());
            if (!done.insert(p).second)
                continue;
            std::visit(
                overloaded{
                    [&](const DerivedPath::Built & bfd) -> void {
                        auto drvPathP = std::get_if<DerivedPath::Opaque>(&*bfd.drvPath);
                        if (!drvPathP) {
                            // TODO make work in this case.
                            warn(
                                "Ignoring dynamic derivation %s while querying missing paths; not yet implemented",
                                bfd.drvPath->to_string(*this));
                            return;
                        }
                        auto & drvPath = drvPathP->path;

                        if (!isValidPath(drvPath)) {
                            // FIXME: we could try to substitute the derivation.
                            res.unknown.insert(drvPath);
                            return;
                        }

                        StorePathSet invalid;
                        /* true for regular derivations, and CA derivations for which we
                           have a trust mapping for all wanted outputs. */
                        auto knownOutputPaths = true;
                        for (auto & [outputName, pathOpt] : queryPartialDerivationOutputMap(drvPath)) {
                            if (!pathOpt) {
                                knownOutputPaths = false;
                                break;
                            }
                            if (bfd.outputs.contains(outputName) && !isValidPath(*pathOpt))
                                invalid.insert(*pathOpt);
                        }
                        if (knownOutputPaths && invalid.empty())
                            return;

                        auto drv = make_ref<Derivation>(derivationFromPath(drvPath));
                        DerivationOptions<SingleDerivedPath> drvOptions;
                        try {
                            // FIXME: this is a lot of work just to get the value
                            // of `allowSubstitutes`.
                            drvOptions = derivationOptionsFromStructuredAttrs(
                                *this, drv->inputDrvs, drv->env, get(drv->structuredAttrs));
                        } catch (Error & e) {
                            e.addTrace({}, "while parsing derivation '%s'", printStorePath(drvPath));
                            throw;
                        }

                        if (!knownOutputPaths && settings.getWorkerSettings().useSubstitutes
                            && drvOptions.substitutesAllowed(settings.getWorkerSettings())) {
                            experimentalFeatureSettings.require(Xp::CaDerivations);

                            // If there are unknown output paths, attempt to find if the
                            // paths are known to substituters through a realisation.
                            auto outputHashes = staticOutputHashes(*this, *drv);
                            knownOutputPaths = true;

                            for (auto [outputName, hash] : outputHashes) {
                                if (!bfd.outputs.contains(outputName))
                                    continue;

                                bool found = false;
                                for (auto & sub : getDefaultSubstituters()) {
                                    /* TODO: Asyncify this. */
                                    auto realisation = sub->queryRealisation({hash, outputName});
                                    if (!realisation)
                                        continue;
                                    found = true;
                                    if (!isValidPath(realisation->outPath))
                                        invalid.insert(realisation->outPath);
                                    break;
                                }
                                if (!found) {
                                    // Some paths did not have a realisation, this must be built.
                                    knownOutputPaths = false;
                                    break;
                                }
                            }
                        }

                        if (knownOutputPaths && settings.getWorkerSettings().useSubstitutes
                            && drvOptions.substitutesAllowed(settings.getWorkerSettings()) && !subs.empty()) {
                            for (auto & p : invalid) {
                                pathsToQuery.insert(p);
                                outPathsToDrvs[p].insert_or_assign(drvPath, drv);
                            }
                        } else
                            mustBuildDrv(drvPath, *drv, paths);
                    },
                    [&](const DerivedPath::Opaque & bo) -> void {
                        // FIXME: this should probably be an async call, but for a local store we probably don't want to
                        // bother.
                        if (!maybeQueryPathInfo(bo.path))
                            pathsToQuery.insert(bo.path);
                    },
                },
                p.raw());
        }

        if (pathsToQuery.empty())
            co_return;

        auto executor = co_await asio::this_coro::executor;

        std::unordered_map<StorePath, size_t> negativeResultsPerPath;

        /* Query all substituters concurrently. FIXME: this may not be desirable. */
        co_await forEachAsync(subs, [&](const ref<Store> & sub) -> asio::awaitable<void> {
            debug("querying %d paths on '%s'", pathsToQuery.size(), sub->config.getHumanReadableURI());
            return sub->queryPathInfos(
                pathsToQuery, [&](std::vector<std::pair<StorePath, std::shared_ptr<const ValidPathInfo>>> infos) {
                    debug("got %d paths from %s", infos.size(), sub->config.getHumanReadableURI());

                    std::set<DerivedPath> todo;

                    for (auto & [path, info] : infos) {
                        if (info) {
                            res.willSubstitute.insert(path);
                            res.narSize += info->narSize;

                            for (auto & ref : info->references)
                                todo.insert(DerivedPath::Opaque{ref});

                            if (auto narInfo = std::dynamic_pointer_cast<const NarInfo>(info)) {
                                res.downloadSize += narInfo->fileSize;

                                /* Recurse into the partial closure hint as well,
                                   so we don't have to wait for the narinfos of
                                   the direct references to discover the rest of
                                   the closure. */
                                for (auto & ref : narInfo->partialClosure)
                                    todo.insert(DerivedPath::Opaque{ref});
                            }
                        } else {
                            if (++negativeResultsPerPath[path] == subs.size()) {
                                if (auto i = outPathsToDrvs.find(path); i != outPathsToDrvs.end()) {
                                    for (auto & [drvPath, drv] : i->second)
                                        mustBuildDrv(drvPath, *drv, todo);
                                } else
                                    /* This path is not a derivation output, so there is no way to produce it. */
                                    res.unknown.insert(path);
                            }
                        }
                    }

                    if (!todo.empty())
                        asio::co_spawn(executor, std::bind(doPaths, todo), [&](std::exception_ptr e) {
                            if (e)
                                ex = e;
                        });
                });
        });

        co_return;
    };

    asio::co_spawn(
        ctx, std::bind(doPaths, std::set<DerivedPath>(targets.begin(), targets.end())), [&](std::exception_ptr e) {
            ex = e;
        });

    ctx.run();
    if (ex)
        std::rethrow_exception(ex);

    return res;
}

StorePaths Store::topoSortPaths(const StorePathSet & paths)
{
    auto result = topoSort(paths, [&](const StorePath & path) {
        try {
            return queryPathInfo(path)->references;
        } catch (InvalidPath &) {
            return StorePathSet();
        }
    });

    return std::visit(
        overloaded{
            [&](const Cycle<StorePath> & cycle) -> StorePaths {
                throw BuildError(
                    BuildResult::Failure::OutputRejected,
                    "cycle detected in the references of '%s' from '%s'",
                    printStorePath(cycle.path),
                    printStorePath(cycle.parent));
            },
            [](const auto & sorted) { return sorted; }},
        result);
}

OutputPathMap resolveDerivedPath(Store & store, const DerivedPath::Built & bfd, Store * evalStore_)
{
    auto drvPath = resolveDerivedPath(store, *bfd.drvPath, evalStore_);

    auto outputsOpt_ = store.queryPartialDerivationOutputMap(drvPath, evalStore_);

    auto outputsOpt = std::visit(
        overloaded{
            [&](const OutputsSpec::All &) {
                // Keep all outputs
                return std::move(outputsOpt_);
            },
            [&](const OutputsSpec::Names & names) {
                // Get just those mentioned by name
                std::map<std::string, std::optional<StorePath>> outputsOpt;
                for (auto & output : names) {
                    auto * pOutputPathOpt = get(outputsOpt_, output);
                    if (!pOutputPathOpt)
                        throw Error(
                            "the derivation '%s' doesn't have an output named '%s'",
                            bfd.drvPath->to_string(store),
                            output);
                    outputsOpt.insert_or_assign(output, std::move(*pOutputPathOpt));
                }
                return outputsOpt;
            },
        },
        bfd.outputs.raw);

    OutputPathMap outputs;
    for (auto & [outputName, outputPathOpt] : outputsOpt) {
        if (!outputPathOpt)
            throw MissingRealisation(bfd.drvPath->to_string(store), outputName);
        auto & outputPath = *outputPathOpt;
        outputs.insert_or_assign(outputName, outputPath);
    }
    return outputs;
}

StorePath resolveDerivedPath(Store & store, const SingleDerivedPath & req, Store * evalStore_)
{
    auto & evalStore = evalStore_ ? *evalStore_ : store;

    return std::visit(
        overloaded{
            [&](const SingleDerivedPath::Opaque & bo) { return bo.path; },
            [&](const SingleDerivedPath::Built & bfd) {
                auto drvPath = resolveDerivedPath(store, *bfd.drvPath, evalStore_);
                auto outputPaths = evalStore.queryPartialDerivationOutputMap(drvPath, evalStore_);
                if (outputPaths.count(bfd.output) == 0)
                    throw Error(
                        "derivation '%s' does not have an output named '%s'",
                        store.printStorePath(drvPath),
                        bfd.output);
                auto & optPath = outputPaths.at(bfd.output);
                if (!optPath)
                    throw MissingRealisation(bfd.drvPath->to_string(store), bfd.output);
                return *optPath;
            },
        },
        req.raw());
}

OutputPathMap resolveDerivedPath(Store & store, const DerivedPath::Built & bfd)
{
    auto drvPath = resolveDerivedPath(store, *bfd.drvPath);
    auto outputMap = store.queryDerivationOutputMap(drvPath);
    auto outputsLeft = std::visit(
        overloaded{
            [&](const OutputsSpec::All &) { return StringSet{}; },
            [&](const OutputsSpec::Names & names) { return static_cast<StringSet>(names); },
        },
        bfd.outputs.raw);
    for (auto iter = outputMap.begin(); iter != outputMap.end();) {
        auto & outputName = iter->first;
        if (bfd.outputs.contains(outputName)) {
            outputsLeft.erase(outputName);
            ++iter;
        } else {
            iter = outputMap.erase(iter);
        }
    }
    if (!outputsLeft.empty())
        throw Error(
            "derivation '%s' does not have an outputs %s",
            store.printStorePath(drvPath),
            concatStringsSep(", ", quoteStrings(std::get<OutputsSpec::Names>(bfd.outputs.raw))));
    return outputMap;
}

} // namespace nix

namespace nlohmann {

using namespace nix;

TrustedFlag adl_serializer<TrustedFlag>::from_json(const json & json)
{
    return getBoolean(json) ? TrustedFlag::Trusted : TrustedFlag::NotTrusted;
}

void adl_serializer<TrustedFlag>::to_json(json & json, const TrustedFlag & trustedFlag)
{
    json = static_cast<bool>(trustedFlag);
}

} // namespace nlohmann
