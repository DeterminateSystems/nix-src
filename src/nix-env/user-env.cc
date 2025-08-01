#include "user-env.hh"
#include "nix/store/derivations.hh"
#include "nix/store/store-api.hh"
#include "nix/store/path-with-outputs.hh"
#include "nix/store/local-fs-store.hh"
#include "nix/store/globals.hh"
#include "nix/main/shared.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/store/profiles.hh"
#include "nix/expr/print-ambiguous.hh"

#include <limits>
#include <sstream>

namespace nix {

PackageInfos queryInstalled(EvalState & state, const Path & userEnv)
{
    PackageInfos elems;
    if (pathExists(userEnv + "/manifest.json"))
        throw Error("profile '%s' is incompatible with 'nix-env'; please use 'nix profile' instead", userEnv);
    auto manifestFile = userEnv + "/manifest.nix";
    if (pathExists(manifestFile)) {
        Value v;
        state.evalFile(state.rootPath(CanonPath(manifestFile)).resolveSymlinks(), v);
        Bindings & bindings(*state.allocBindings(0));
        getDerivations(state, v, "", bindings, elems, false);
    }
    return elems;
}

bool createUserEnv(
    EvalState & state, PackageInfos & elems, const Path & profile, bool keepDerivations, const std::string & lockToken)
{
    /* Build the components in the user environment, if they don't
       exist already. */
    std::vector<StorePathWithOutputs> drvsToBuild;
    for (auto & i : elems)
        if (auto drvPath = i.queryDrvPath()) {
            state.waitForPath(*drvPath);
            drvsToBuild.push_back({*drvPath});
        }

    debug("building user environment dependencies");
    state.store->buildPaths(toDerivedPaths(drvsToBuild), state.repair ? bmRepair : bmNormal);

    /* Construct the whole top level derivation. */
    StorePathSet references;
    auto list = state.buildList(elems.size());
    for (const auto & [n, i] : enumerate(elems)) {
        /* Create a pseudo-derivation containing the name, system,
           output paths, and optionally the derivation path, as well
           as the meta attributes. */
        std::optional<StorePath> drvPath = keepDerivations ? i.queryDrvPath() : std::nullopt;
        PackageInfo::Outputs outputs = i.queryOutputs(true, true);
        StringSet metaNames = i.queryMetaNames();

        auto attrs = state.buildBindings(7 + outputs.size());

        attrs.alloc(state.sType).mkString("derivation");
        attrs.alloc(state.sName).mkString(i.queryName());
        auto system = i.querySystem();
        if (!system.empty())
            attrs.alloc(state.sSystem).mkString(system);
        attrs.alloc(state.sOutPath).mkString(state.store->printStorePath(i.queryOutPath()));
        if (drvPath)
            attrs.alloc(state.sDrvPath).mkString(state.store->printStorePath(*drvPath));

        // Copy each output meant for installation.
        auto outputsList = state.buildList(outputs.size());
        for (const auto & [m, j] : enumerate(outputs)) {
            (outputsList[m] = state.allocValue())->mkString(j.first);
            auto outputAttrs = state.buildBindings(2);
            outputAttrs.alloc(state.sOutPath).mkString(state.store->printStorePath(*j.second));
            attrs.alloc(j.first).mkAttrs(outputAttrs);

            /* This is only necessary when installing store paths, e.g.,
               `nix-env -i /nix/store/abcd...-foo'. */
            state.store->addTempRoot(*j.second);
            state.store->ensurePath(*j.second);

            references.insert(*j.second);
        }
        attrs.alloc(state.sOutputs).mkList(outputsList);

        // Copy the meta attributes.
        auto meta = state.buildBindings(metaNames.size());
        for (auto & j : metaNames) {
            Value * v = i.queryMeta(j);
            if (!v)
                continue;
            meta.insert(state.symbols.create(j), v);
        }

        attrs.alloc(state.sMeta).mkAttrs(meta);

        (list[n] = state.allocValue())->mkAttrs(attrs);

        if (drvPath)
            references.insert(*drvPath);
    }

    Value manifest;
    manifest.mkList(list);

    /* Also write a copy of the list of user environment elements to
       the store; we need it for future modifications of the
       environment. */
    auto manifestFile = ({
        std::ostringstream str;
        printAmbiguous(state, manifest, str, nullptr, std::numeric_limits<int>::max());
        StringSource source{toView(str)};
        state.store->addToStoreFromDump(
            source,
            "env-manifest.nix",
            FileSerialisationMethod::Flat,
            ContentAddressMethod::Raw::Text,
            HashAlgorithm::SHA256,
            references);
    });

    /* Get the environment builder expression. */
    Value envBuilder;
    state.eval(
        state.parseExprFromString(
#include "buildenv.nix.gen.hh"
            , state.rootPath(CanonPath::root)),
        envBuilder);

    /* Construct a Nix expression that calls the user environment
       builder with the manifest as argument. */
    auto attrs = state.buildBindings(3);
    state.mkStorePathString(manifestFile, attrs.alloc("manifest"));
    attrs.insert(state.symbols.create("derivations"), &manifest);
    Value args;
    args.mkAttrs(attrs);

    Value topLevel;
    topLevel.mkApp(&envBuilder, &args);

    /* Evaluate it. */
    debug("evaluating user environment builder");
    state.forceValue(topLevel, topLevel.determinePos(noPos));
    NixStringContext context;
    auto & aDrvPath(*topLevel.attrs()->find(state.sDrvPath));
    auto topLevelDrv = state.coerceToStorePath(aDrvPath.pos, *aDrvPath.value, context, "");
    topLevelDrv.requireDerivation();
    auto & aOutPath(*topLevel.attrs()->find(state.sOutPath));
    auto topLevelOut = state.coerceToStorePath(aOutPath.pos, *aOutPath.value, context, "");

    /* Realise the resulting store expression. */
    debug("building user environment");
    std::vector<StorePathWithOutputs> topLevelDrvs;
    topLevelDrvs.push_back({topLevelDrv});
    state.waitForPath(topLevelDrv);
    state.store->buildPaths(toDerivedPaths(topLevelDrvs), state.repair ? bmRepair : bmNormal);

    /* Switch the current user environment to the output path. */
    auto store2 = state.store.dynamic_pointer_cast<LocalFSStore>();

    if (store2) {
        PathLocks lock;
        lockProfile(lock, profile);

        Path lockTokenCur = optimisticLockProfile(profile);
        if (lockToken != lockTokenCur) {
            printInfo("profile '%1%' changed while we were busy; restarting", profile);
            return false;
        }

        debug("switching to new user environment");
        Path generation = createGeneration(*store2, profile, topLevelOut);
        switchLink(profile, generation);
    }

    return true;
}

} // namespace nix
