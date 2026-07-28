#include <nlohmann/json.hpp>
#include <assert.h>
#include <stdint.h>
#include <boost/container/detail/std_fwd.hpp>
#include <boost/core/pointer_traits.hpp>
#include <boost/unordered/detail/foa/table.hpp>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "nix/util/terminal.hh"
#include "nix/util/ref.hh"
#include "nix/util/environment-variables.hh"
#include "nix/flake/flake.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-cache.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/flake/lockfile-v7.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/store/store-api.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/util/finally.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/flake/settings.hh"
#include "nix/expr/value-to-json.hh"
#include "nix/fetchers/fetch-to-store.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/mounted-source-accessor.hh"
#include "nix/fetchers/input-cache.hh"
#include "nix/expr/attr-set.hh"
#include "nix/expr/eval-error.hh"
#include "nix/expr/fetch-tree.hh"
#include "nix/expr/nixexpr.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/expr/value.hh"
#include "nix/expr/value/context.hh"
#include "nix/fetchers/attrs.hh"
#include "nix/fetchers/registry.hh"
#include "nix/flake/flakeref.hh"
#include "nix/store/path.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/configuration.hh"
#include "nix/util/error.hh"
#include "nix/util/experimental-features.hh"
#include "nix/util/file-system.hh"
#include "nix/util/fmt.hh"
#include "nix/util/hash.hh"
#include "nix/util/logging.hh"
#include "nix/util/pos-idx.hh"
#include "nix/util/pos-table.hh"
#include "nix/util/source-path.hh"
#include "nix/util/types.hh"
#include "nix/util/util.hh"

namespace nix {
struct SourceAccessor;

using namespace fetchers;

namespace flake {

static void forceTrivialValue(EvalState & state, Value & value, const PosIdx pos)
{
    if (value.isTrivial())
        state.forceValue(value, pos);
}

static void expectType(EvalState & state, ValueType type, Value & value, const PosIdx pos)
{
    forceTrivialValue(state, value, pos);
    auto t = value.type();
    if (t != type)
        throw Error("expected %s but got %s at %s", showType(type), showType(t), state.positions[pos]);
}

static std::pair<std::map<FlakeId, FlakeInput>, fetchers::Attrs> parseFlakeInputs(
    EvalState & state,
    Value * value,
    const PosIdx pos,
    const InputAttrPath & lockRootAttrPath,
    const SourcePath & flakeDir,
    bool allowSelf);

static void parseFlakeInputAttr(EvalState & state, const nix::Attr & attr, fetchers::Attrs & attrs)
{
// Allow selecting a subset of enum values
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (attr.value->type()) {
    case nString:
        attrs.emplace(state.symbols[attr.name], std::string(attr.value->string_view()));
        break;
    case nBool:
        attrs.emplace(state.symbols[attr.name], Explicit<bool>{attr.value->boolean()});
        break;
    case nInt: {
        auto intValue = attr.value->integer().value;
        if (intValue < 0)
            state
                .error<EvalError>(
                    "negative value given for flake input attribute %1%: %2%", state.symbols[attr.name], intValue)
                .debugThrow();
        attrs.emplace(state.symbols[attr.name], uint64_t(intValue));
        break;
    }
    default:
        if (attr.name == state.symbols.create("publicKeys")) {
            experimentalFeatureSettings.require(Xp::VerifiedFetches);
            NixStringContext emptyContext = {};
            attrs.emplace(
                state.symbols[attr.name], printValueAsJSON(state, true, *attr.value, attr.pos, emptyContext).dump());
        } else
            state
                .error<TypeError>(
                    "flake input attribute '%s' is %s while a string, Boolean, or integer is expected",
                    state.symbols[attr.name],
                    showType(*attr.value))
                .debugThrow();
    }
#pragma GCC diagnostic pop
}

static FlakeInput parseFlakeInput(
    EvalState & state,
    Value * value,
    const PosIdx pos,
    const InputAttrPath & lockRootAttrPath,
    const SourcePath & flakeDir)
{
    expectType(state, nAttrs, *value, pos);

    FlakeInput input;

    auto sInputs = state.symbols.create("inputs");
    auto sUrl = state.symbols.create("url");
    auto sFlake = state.symbols.create("flake");
    auto sFollows = state.symbols.create("follows");
    auto sBuildTime = state.symbols.create("buildTime");

    fetchers::Attrs attrs;
    std::optional<std::string> url;

    for (auto & attr : *value->attrs()) {
        try {
            if (attr.name == sUrl) {
                forceTrivialValue(state, *attr.value, pos);
                if (attr.value->type() == nString)
                    url = attr.value->string_view();
                else if (attr.value->type() == nPath) {
                    auto path = attr.value->path();
                    if (path.accessor != flakeDir.accessor)
                        throw Error(
                            "input attribute path '%s' at %s must be in the same source tree as %s",
                            path,
                            state.positions[attr.pos],
                            flakeDir);
                    url = "path:" + flakeDir.path.makeRelative(path.path);
                } else
                    throw Error(
                        "expected a string or a path but got %s at %s",
                        showType(attr.value->type()),
                        state.positions[attr.pos]);
                attrs.emplace("url", *url);
            } else if (attr.name == sFlake) {
                expectType(state, nBool, *attr.value, attr.pos);
                input.isFlake = attr.value->boolean();
            } else if (attr.name == sBuildTime) {
                expectType(state, nBool, *attr.value, attr.pos);
                input.buildTime = attr.value->boolean();
                if (input.buildTime)
                    experimentalFeatureSettings.require(Xp::BuildTimeFetchTree);
            } else if (attr.name == sInputs) {
                input.overrides =
                    parseFlakeInputs(state, attr.value, attr.pos, lockRootAttrPath, flakeDir, false).first;
            } else if (attr.name == sFollows) {
                expectType(state, nString, *attr.value, attr.pos);
                auto follows(parseInputAttrPath(attr.value->string_view()));
                follows.insert(follows.begin(), lockRootAttrPath.begin(), lockRootAttrPath.end());
                input.follows = follows;
            } else
                parseFlakeInputAttr(state, attr, attrs);
        } catch (Error & e) {
            e.addTrace(
                state.positions[attr.pos], HintFmt("while evaluating flake attribute '%s'", state.symbols[attr.name]));
            throw;
        }
    }

    if (attrs.count("type"))
        try {
            input.ref = FlakeRef::fromAttrs(state.fetchSettings, attrs);
        } catch (Error & e) {
            e.addTrace(state.positions[pos], HintFmt("while evaluating flake input"));
            throw;
        }
    else {
        attrs.erase("url");
        if (!attrs.empty())
            throw Error("unexpected flake input attribute '%s', at %s", attrs.begin()->first, state.positions[pos]);
        if (url)
            input.ref = parseFlakeRef(state.fetchSettings, *url, {}, true, input.isFlake, true);
    }

    if (input.ref && input.follows)
        throw Error("flake input has both a flake reference and a follows attribute, at %s", state.positions[pos]);

    return input;
}

static std::pair<std::map<FlakeId, FlakeInput>, fetchers::Attrs> parseFlakeInputs(
    EvalState & state,
    Value * value,
    const PosIdx pos,
    const InputAttrPath & lockRootAttrPath,
    const SourcePath & flakeDir,
    bool allowSelf)
{
    std::map<FlakeId, FlakeInput> inputs;
    fetchers::Attrs selfAttrs;

    expectType(state, nAttrs, *value, pos);

    for (auto & inputAttr : *value->attrs()) {
        auto inputName = state.symbols[inputAttr.name];
        if (inputName == "self") {
            if (!allowSelf)
                throw Error("'self' input attribute not allowed at %s", state.positions[inputAttr.pos]);
            expectType(state, nAttrs, *inputAttr.value, inputAttr.pos);
            for (auto & attr : *inputAttr.value->attrs())
                parseFlakeInputAttr(state, attr, selfAttrs);
        } else {
            inputs.emplace(
                inputName, parseFlakeInput(state, inputAttr.value, inputAttr.pos, lockRootAttrPath, flakeDir));
        }
    }

    return {inputs, selfAttrs};
}

Flake readFlake(
    EvalState & state,
    const FlakeRef & originalRef,
    const FlakeRef & resolvedRef,
    const FlakeRef & lockedRef,
    const SourcePath & rootDir,
    const InputAttrPath & lockRootAttrPath)
{
    auto flakeDir = rootDir / CanonPath(resolvedRef.subdir);
    auto flakePath = flakeDir / "flake.nix";

    // NOTE evalFile forces vInfo to be an attrset because mustBeTrivial is true.
    Value vInfo;
    state.evalFile(flakePath, vInfo, true);

    Flake flake{
        .originalRef = originalRef,
        .resolvedRef = resolvedRef,
        .lockedRef = lockedRef,
        .path = flakePath,
        .provenance = flakePath.getProvenance(),
    };

    if (auto description = vInfo.attrs()->get(state.s.description)) {
        expectType(state, nString, *description->value, description->pos);
        flake.description = description->value->string_view();
    }

    auto sInputs = state.symbols.create("inputs");

    if (auto inputs = vInfo.attrs()->get(sInputs)) {
        auto [flakeInputs, selfAttrs] =
            parseFlakeInputs(state, inputs->value, inputs->pos, lockRootAttrPath, flakeDir, true);
        flake.inputs = std::move(flakeInputs);
        flake.selfAttrs = std::move(selfAttrs);
    }

    auto sOutputs = state.symbols.create("outputs");

    if (auto outputs = vInfo.attrs()->get(sOutputs)) {
        expectType(state, nFunction, *outputs->value, outputs->pos);

        if (outputs->value->isLambda()) {
            if (auto formals = outputs->value->lambda().fun->getFormals()) {
                for (auto & formal : formals->formals) {
                    if (formal.name != state.s.self)
                        flake.inputs.emplace(
                            state.symbols[formal.name],
                            FlakeInput{
                                .ref = parseFlakeRef(state.fetchSettings, std::string(state.symbols[formal.name]))});
                }
            }
        }

    } else
        throw Error("flake '%s' lacks attribute 'outputs'", resolvedRef);

    auto sNixConfig = state.symbols.create("nixConfig");

    if (auto nixConfig = vInfo.attrs()->get(sNixConfig)) {
        expectType(state, nAttrs, *nixConfig->value, nixConfig->pos);

        for (auto & setting : *nixConfig->value->attrs()) {
            forceTrivialValue(state, *setting.value, setting.pos);
            if (setting.value->type() == nString)
                flake.config.settings.emplace(
                    state.symbols[setting.name], std::string(state.forceStringNoCtx(*setting.value, setting.pos, "")));
            else if (setting.value->type() == nPath) {
                auto storePath =
                    fetchToStore(state.fetchSettings, *state.store, setting.value->path(), FetchMode::Copy);
                flake.config.settings.emplace(state.symbols[setting.name], state.store->printStorePath(storePath));
            } else if (setting.value->type() == nInt)
                flake.config.settings.emplace(
                    state.symbols[setting.name], state.forceInt(*setting.value, setting.pos, "").value);
            else if (setting.value->type() == nBool)
                flake.config.settings.emplace(
                    state.symbols[setting.name], Explicit<bool>{state.forceBool(*setting.value, setting.pos, "")});
            else if (setting.value->type() == nList) {
                std::vector<std::string> ss;
                for (auto elem : setting.value->listView()) {
                    if (elem->type() != nString)
                        state
                            .error<TypeError>(
                                "list element in flake configuration setting '%s' is %s while a string is expected",
                                state.symbols[setting.name],
                                showType(*setting.value))
                            .debugThrow();
                    ss.emplace_back(state.forceStringNoCtx(*elem, setting.pos, ""));
                }
                flake.config.settings.emplace(state.symbols[setting.name], ss);
            } else
                state
                    .error<TypeError>(
                        "flake configuration setting '%s' is %s", state.symbols[setting.name], showType(*setting.value))
                    .debugThrow();
        }
    }

    for (auto & attr : *vInfo.attrs()) {
        if (attr.name != state.s.description && attr.name != sInputs && attr.name != sOutputs
            && attr.name != sNixConfig)
            throw Error(
                "flake '%s' has an unsupported attribute '%s', at %s",
                resolvedRef,
                state.symbols[attr.name],
                state.positions[attr.pos]);
    }

    return flake;
}

static FlakeRef applySelfAttrs(const FlakeRef & ref, const Flake & flake)
{
    auto newRef(ref);

    StringSet allowedAttrs{"submodules", "lfs"};

    for (auto & attr : flake.selfAttrs) {
        if (!allowedAttrs.contains(attr.first))
            throw Error("flake 'self' attribute '%s' is not supported", attr.first);
        newRef.input.attrs.insert_or_assign(attr.first, attr.second);
    }

    return newRef;
}

Flake getFlake(
    EvalState & state,
    const FlakeRef & originalRef,
    fetchers::UseRegistries useRegistries,
    const InputAttrPath & lockRootAttrPath,
    bool requireLockable)
{
    // Fetch a lazy tree first.
    auto cachedInput =
        state.inputCache->getAccessor(state.fetchSettings, *state.store, originalRef.input, useRegistries);

    auto subdir = fetchers::maybeGetStrAttr(cachedInput.extraAttrs, "dir").value_or(originalRef.subdir);
    auto resolvedRef = FlakeRef(std::move(cachedInput.resolvedInput), subdir);
    auto lockedRef = FlakeRef(std::move(cachedInput.lockedInput), subdir);

    // Parse/eval flake.nix to get at the input.self attributes.
    auto flake = readFlake(state, originalRef, resolvedRef, lockedRef, {cachedInput.accessor}, lockRootAttrPath);

    // Re-fetch the tree if necessary.
    auto newLockedRef = applySelfAttrs(lockedRef, flake);

    if (lockedRef != newLockedRef) {
        debug("refetching input '%s' due to self attribute", newLockedRef);
        // FIXME: need to remove attrs that are invalidated by the changed input attrs, such as 'narHash'.
        newLockedRef.input.attrs.erase("narHash");
        auto cachedInput2 = state.inputCache->getAccessor(
            state.fetchSettings, *state.store, newLockedRef.input, fetchers::UseRegistries::No);
        cachedInput.accessor = cachedInput2.accessor;
        lockedRef = FlakeRef(std::move(cachedInput2.lockedInput), newLockedRef.subdir);
    }

    // Re-parse flake.nix from the store.
    return readFlake(
        state,
        originalRef,
        resolvedRef,
        lockedRef,
        state.storePath(state.mountInput(lockedRef.input, originalRef.input, cachedInput.accessor, requireLockable)),
        lockRootAttrPath);
}

Flake getFlake(
    EvalState & state, const FlakeRef & originalRef, fetchers::UseRegistries useRegistries, bool requireLockable)
{
    return getFlake(state, originalRef, useRegistries, {}, requireLockable);
}

std::unique_ptr<LockedFlake> lockFlake(
    const Settings & settings, EvalState & state, const FlakeRef & topRef, const LockFlags & lockFlags, Flake flake)
{
    auto useRegistries = lockFlags.useRegistries.value_or(settings.useRegistries);
    auto useRegistriesTop = useRegistries ? fetchers::UseRegistries::All : fetchers::UseRegistries::No;

    if (lockFlags.applyNixConfig) {
        flake.config.apply(settings);
        state.store->setOptions();
    }

    auto flakeRefForTrace = flake.lockedRef.to_string();

    try {
        if (!state.fetchSettings.allowDirty && lockFlags.referenceLockFilePath) {
            throw Error("reference lock file was provided, but the `allow-dirty` setting is set to false");
        }

        auto lockFilePath = lockFlags.referenceLockFilePath.value_or(flake.lockFilePath());

        nlohmann::json oldLockFileJson;

        if (lockFilePath.pathExists()) {
            try {
                oldLockFileJson = nlohmann::json::parse(lockFilePath.readFile());
            } catch (const nlohmann::json::parse_error & e) {
                throw Error("Could not parse '%s': %s", lockFilePath, e.what());
            }
        }

        // FIXME: dispatch on the lock file version here.
        LockedFlakeV7 oldLockedFlake(state.fetchSettings, flake, oldLockFileJson, fmt("%s", lockFilePath));

        debug("old lock file: %s", oldLockedFlake.to_string());

        auto lockedFlake = LockedFlakeV7::lockFlake(settings, state, lockFlags, std::move(flake), oldLockedFlake);

        debug("new lock file: %s", lockedFlake->to_string());

        auto sourcePath = topRef.input.getSourcePath();

        /* Check whether we need to / can write the new lock file. */
        auto lockedFlakeJson = lockedFlake->toJSON();
        if (lockedFlakeJson != oldLockedFlake.toJSON() || lockFlags.outputLockFilePath) {

            auto diff = lockedFlake->diff(oldLockedFlake);

            if (lockFlags.writeLockFile) {
                if (sourcePath || lockFlags.outputLockFilePath) {
                    if (auto unlockedInput = lockedFlake->isUnlocked(state.fetchSettings)) {
                        if (lockFlags.failOnUnlocked)
                            throw Error(
                                "Not writing lock file of flake '%s' because it has an unlocked input ('%s'). "
                                "Use '--allow-dirty-locks' to allow this anyway.",
                                topRef,
                                *unlockedInput);
                        if (state.fetchSettings.warnDirty)
                            warn(
                                "not writing lock file of flake '%s' because it has an unlocked input ('%s')",
                                topRef,
                                *unlockedInput);
                    } else {
                        if (!lockFlags.updateLockFile)
                            throw Error(
                                "flake '%s' requires lock file changes but they're not allowed due to '--no-update-lock-file'",
                                topRef);

                        auto newLockFileS = fmt("%s\n", lockedFlakeJson.dump(2));

                        if (lockFlags.outputLockFilePath) {
                            if (lockFlags.commitLockFile)
                                throw Error("'--commit-lock-file' and '--output-lock-file' are incompatible");
                            writeFile(*lockFlags.outputLockFilePath, newLockFileS);
                        } else {
                            auto relPath = (topRef.subdir == "" ? "" : topRef.subdir + "/") + "flake.lock";
                            auto outputLockFilePath = *sourcePath / relPath;

                            bool lockFileExists = pathExists(outputLockFilePath);

                            auto s = chomp(diff);
                            if (lockFileExists) {
                                if (s.empty())
                                    warn("updating lock file %s", PathFmt(outputLockFilePath));
                                else
                                    warn("updating lock file %s:\n%s", PathFmt(outputLockFilePath), s);
                            } else
                                warn("creating lock file %s: \n%s", PathFmt(outputLockFilePath), s);

                            std::optional<std::string> commitMessage = std::nullopt;

                            if (lockFlags.commitLockFile) {
                                std::string cm;

                                cm = settings.commitLockFileSummary.get();

                                if (cm == "") {
                                    cm = fmt("%s: %s", relPath, lockFileExists ? "Update" : "Add");
                                }

                                cm += "\n\nFlake lock file updates:\n\n";
                                cm += filterANSIEscapes(diff, true);
                                commitMessage = cm;
                            }

                            topRef.input.putFile(
                                CanonPath((topRef.subdir == "" ? "" : topRef.subdir + "/") + "flake.lock"),
                                newLockFileS,
                                commitMessage);
                        }

                        /* Rewriting the lockfile changed the top-level
                           repo, so we should re-read it. FIXME: we could
                           also just clear the 'rev' field... */
                        auto prevLockedRef = lockedFlake->flake.lockedRef;
                        lockedFlake->flake = getFlake(state, topRef, useRegistriesTop, lockFlags.requireLockable);

                        if (lockFlags.commitLockFile && lockedFlake->flake.lockedRef.input.getRev()
                            && prevLockedRef.input.getRev() != lockedFlake->flake.lockedRef.input.getRev())
                            warn("committed new revision '%s'", lockedFlake->flake.lockedRef.input.getRev()->gitRev());
                    }
                } else
                    throw Error(
                        "cannot write modified lock file of flake '%s' (use '--no-write-lock-file' to ignore)", topRef);
            } else {
                warn("not writing modified lock file of flake '%s':\n%s", topRef, chomp(diff));
                lockedFlake->flake.forceDirty = true;
            }
        }

        return lockedFlake;

    } catch (Error & e) {
        e.addTrace({}, "while updating the lock file of flake '%s'", flakeRefForTrace);
        throw;
    }
}

std::unique_ptr<LockedFlake>
lockFlake(const Settings & settings, EvalState & state, const FlakeRef & topRef, const LockFlags & lockFlags)
{
    auto useRegistries = lockFlags.useRegistries.value_or(settings.useRegistries);
    auto useRegistriesTop = useRegistries ? fetchers::UseRegistries::All : fetchers::UseRegistries::No;

    return lockFlake(settings, state, topRef, lockFlags, getFlake(state, topRef, useRegistriesTop, {}, false));
}

std::unique_ptr<LockedFlake>
lockFlake(const Settings & settings, EvalState & state, const SourcePath & flakeDir, const LockFlags & lockFlags)
{
    /* We need a fake flakeref to put in the `Flake` struct, but it's not used for anything. */
    auto fakeRef = parseFlakeRef(state.fetchSettings, "flake:get-flake");
    return lockFlake(settings, state, fakeRef, lockFlags, readFlake(state, fakeRef, fakeRef, fakeRef, flakeDir, {}));
}

static ref<SourceAccessor> makeInternalFS()
{
    auto internalFS = make_ref<MemorySourceAccessor>(MemorySourceAccessor{});
    internalFS->setPathDisplay("«flakes-internal»", "");
    internalFS->addFile(
        CanonPath("call-flake.nix"),
#include "call-flake.nix.gen.hh" // IWYU pragma: keep
    );
    return internalFS;
}

static auto internalFS = makeInternalFS();

static Value * requireInternalFile(EvalState & state, CanonPath path)
{
    SourcePath p{internalFS, path};
    auto v = state.allocValue();
    state.evalFile(p, *v); // has caching
    return v;
}

void callFlake(EvalState & state, const LockedFlake & _lockedFlake, Value & vRes)
{
    auto & lockedFlake = dynamic_cast<const LockedFlakeV7 &>(_lockedFlake);

    auto [lockFileStr, keyMap] = lockedFlake.lockFile.to_string();

    auto overrides = state.buildBindings(lockedFlake.nodePaths.size());

    for (auto & [node, sourcePath] : lockedFlake.nodePaths) {
        auto override = state.buildBindings(2);

        auto & vSourceInfo = override.alloc(state.symbols.create("sourceInfo"));

        auto lockedNode = node.dynamic_pointer_cast<const LockedNode>();

        auto [storePath, subdir] = state.store->toStorePath(sourcePath.path.abs());

        emitTreeAttrs(
            state,
            storePath,
            lockedNode ? lockedNode->lockedRef.input : lockedFlake.flake.lockedRef.input,
            vSourceInfo,
            false,
            !lockedNode && lockedFlake.flake.forceDirty);

        auto key = keyMap.find(node);
        assert(key != keyMap.end());

        override.alloc(state.symbols.create("dir")).mkString(CanonPath(subdir).rel(), state.mem);

        overrides.alloc(state.symbols.create(key->second)).mkAttrs(override);
    }

    auto & vOverrides = state.allocValue()->mkAttrs(overrides);

    Value * vCallFlake = requireInternalFile(state, CanonPath("call-flake.nix"));

    auto vLocks = state.allocValue();
    vLocks->mkString(lockFileStr, state.mem);

    Value * args[] = {vLocks, &vOverrides};
    state.callFunction(*vCallFlake, args, vRes, noPos);
}

LockedFlake::~LockedFlake() {}

std::string LockedFlake::to_string() const
{
    return toJSON().dump(2);
}

std::ostream & operator<<(std::ostream & stream, const LockedFlake & lockedFlake)
{
    return stream << lockedFlake.to_string();
}

std::optional<Fingerprint> LockedFlake::getFingerprint(Store & store, const fetchers::Settings & fetchSettings) const
{
    if (isUnlocked(fetchSettings))
        return std::nullopt;

    auto fingerprint = flake.lockedRef.input.getFingerprint(store);
    if (!fingerprint)
        return std::nullopt;

    *fingerprint += fmt(";%s;%s", flake.lockedRef.subdir, *this);

    if (auto revCount = get(flake.lockedRef.input.attrs, "revCount")) {
        if (std::get_if<fetchers::LazyAttr>(revCount)) {
            /* A lazy revCount is computed by the fetcher, so its
               value is functionally determined by `rev`. We only
               need to record its presence, not force its value.

               This means a lazy and a concrete revCount that would
               resolve to the same value produce different
               fingerprints, sacrificing some cache hits to avoid
               the cost of forcing. */
            *fingerprint += ";hasRevCount";
        } else if (auto n = flake.lockedRef.input.getRevCount()) {
            /* A concrete revCount comes from a lockfile or explicit
               user input. The fetcher passes it through as-is, so
               it can affect evaluation and must be fingerprinted. */
            *fingerprint += fmt(";revCount=%d", *n);
        }
    }
    if (auto lastModified = flake.lockedRef.input.getLastModified())
        *fingerprint += fmt(";lastModified=%d", *lastModified);

    // FIXME: as an optimization, if the flake contains a lock file
    // and we haven't changed it, then it's sufficient to use
    // flake.sourceInfo.storePath for the fingerprint.
    return hashString(HashAlgorithm::SHA256, *fingerprint);
}

Flake::~Flake() {}

} // namespace flake

} // namespace nix
