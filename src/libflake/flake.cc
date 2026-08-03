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
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "nix/util/terminal.hh"
#include "nix/util/ref.hh"
#include "nix/util/environment-variables.hh"
#include "nix/flake/flake.hh"
#include "flake-impl.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-cache.hh"
#include "nix/expr/eval-settings.hh"
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
#include "nix/expr/json-to-value.hh"
#include "nix/expr/primops.hh"
#include "nix/expr/print.hh"
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
#include "nix/util/strings.hh"
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

std::unique_ptr<LockedFlake> parseLockFile(
    const fetchers::Settings & fetchSettings,
    Flake flake,
    const nlohmann::json & json,
    std::string_view path,
    unsigned int versionIfMissing)
{
    auto version = json.is_null() ? versionIfMissing : (unsigned int) json.value("version", 0);

    if (version >= 5 && version <= 7)
        return parseLockFileV7(fetchSettings, std::move(flake), json, path);
    else
        throw Error("lock file '%s' has unsupported version %d", path, version);
}

void warnRegistry(
    const InputAttrPath & inputAttrPath,
    const FlakeRef & ref,
    const FlakeRef & resolvedRef,
    const SourcePath & topFlakePath)
{
    if (inputAttrPath.size() == 1 && !ref.input.isDirect()) {
        std::ostringstream s;
        printLiteralString(s, resolvedRef.to_string());
        warn(
            "Flake input '%1%' uses the flake registry. "
            "Using the registry in flake inputs is deprecated in Determinate Nix. "
            "To make your flake future-proof, add the following to '%2%':\n"
            "\n"
            "  inputs.%1%.url = %3%;\n"
            "\n"
            "For more information, see: https://github.com/DeterminateSystems/nix-src/issues/37",
            printInputAttrPath(inputAttrPath),
            topFlakePath,
            s.str());
    }
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
        auto oldLockedFlake = parseLockFile(state.fetchSettings, flake, oldLockFileJson, fmt("%s", lockFilePath));

        debug("old lock file: %s", oldLockedFlake->to_string());

        auto [lockedFlake, overridesUsed, updatesUsed] =
            lockFlakeV7(settings, state, lockFlags, std::move(flake), *oldLockedFlake);

        for (auto & i : lockFlags.inputOverrides)
            if (!overridesUsed.count(i.first))
                warn(
                    "the flag '--override-input %s %s' does not match any input",
                    printInputAttrPath(i.first),
                    i.second);

        if (lockFlags.inputUpdates)
            for (auto & i : *lockFlags.inputUpdates)
                if (!updatesUsed.count(i))
                    warn("'%s' does not match any input of this flake", printInputAttrPath(i));

        debug("new lock file: %s", lockedFlake->to_string());

        auto sourcePath = topRef.input.getSourcePath();

        /* Check whether we need to / can write the new lock file. */
        auto lockedFlakeJson = lockedFlake->toJSON();
        if (lockedFlakeJson != oldLockedFlake->toJSON() || lockFlags.outputLockFilePath) {

            auto diff = diffLockedFlakes(*oldLockedFlake, *lockedFlake, false);

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

        return std::move(lockedFlake);

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

/**
 * An external value wrapping a `LockedFlake`, passed as an argument
 * to `call-flake.nix` and consumed by the `listFlakeInputs` and
 * `fetchFlakeInput` primops.
 */
class LockedFlakeValue : public ExternalValueBase, public gc_cleanup
{
public:
    const std::shared_ptr<const LockedFlake> lockedFlake;

    LockedFlakeValue(std::shared_ptr<const LockedFlake> lockedFlake)
        : lockedFlake(std::move(lockedFlake))
    {
    }

    std::string showType() const override
    {
        return "a locked flake";
    }

    std::string typeOf() const override
    {
        return "lockedFlake";
    }

protected:
    std::ostream & print(std::ostream & str) const override
    {
        return str << "«locked flake»";
    }
};

static const LockedFlake & requireLockedFlake(EvalState & state, Value & v, const PosIdx pos)
{
    state.forceValue(v, pos);
    if (v.type() == nExternal)
        if (auto * ext = dynamic_cast<LockedFlakeValue *>(v.external()))
            return *ext->lockedFlake;
    state.error<TypeError>("expected a locked flake but found %1%", showType(v)).atPos(pos).debugThrow();
}

static InputAttrPath getInputAttrPathArg(EvalState & state, Value & v, const PosIdx pos)
{
    state.forceList(v, pos, "while evaluating an input attribute path");
    InputAttrPath path;
    for (auto elem : v.listView())
        path.push_back(
            std::string(state.forceStringNoCtx(*elem, pos, "while evaluating an input attribute path element")));
    return path;
}

static void prim_listFlakeInputs(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto & lockedFlake = requireLockedFlake(state, *args[0], pos);
    auto prefix = getInputAttrPathArg(state, *args[1], pos);

    auto targets = lockedFlake.getInputTargets(state, prefix);

    auto attrs = state.buildBindings(targets.size());

    for (auto & [id, target] : targets) {
        auto & vTarget = attrs.alloc(state.symbols.create(id));
        if (!target)
            vTarget.mkNull();
        else {
            auto list = state.buildList(target->size());
            for (const auto & [n, elem] : enumerate(*target))
                (list[n] = state.allocValue())->mkString(elem, state.mem);
            vTarget.mkList(list);
        }
    }

    v.mkAttrs(attrs);
}

static RegisterPrimOp primop_listFlakeInputs({
    .name = "__listFlakeInputs",
    .args = {"lockedFlake", "inputAttrPath"},
    .doc = R"(
      For the flake input of *lockedFlake* denoted by *inputAttrPath*
      (a list of strings, where the empty list denotes the top-level
      flake), return an attribute set mapping the names of its inputs
      to either null (for a regular input) or the input attribute path
      of the target of a "follows" input.
    )",
    .impl = prim_listFlakeInputs,
    .internal = true,
});

static void prim_fetchFlakeInput(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto & lockedFlake = requireLockedFlake(state, *args[0], pos);
    auto path = getInputAttrPathArg(state, *args[1], pos);

    std::optional<LockedFlake::InputInfo> info;
    if (!path.empty()) {
        info = lockedFlake.findInput(state, path);
        if (!info)
            state.error<EvalError>("flake input '%s' does not exist", printInputAttrPath(path)).atPos(pos).debugThrow();
    }

    auto attrs = state.buildBindings(4);

    attrs.alloc("flake").mkBool(info ? info->isFlake : true);
    attrs.alloc("buildTime").mkBool(info && info->buildTime);

    if (info && info->buildTime) {
        /* Build-time inputs are not fetched at evaluation time;
           return the locked input attributes so that call-flake.nix
           can construct a `builtin:fetch-tree` derivation. */
        parseJSON(state, fetchers::attrsToJSON(info->lockedRef.toAttrs()).dump(), attrs.alloc("locked"));
    } else {
        if (info && !info->lockedRef.input.isRelative())
            state.checkURI(info->lockedRef.input.toURLString());

        auto sourcePath = lockedFlake.getSourcePath(state, path);
        auto [storePath, subdir] = state.store->toStorePath(sourcePath.path.abs());

        /* Relative path inputs have the same source tree as their
           parent flake, so their `sourceInfo` metadata comes from the
           nearest non-relative ancestor. */
        auto info2 = info;
        while (info2 && info2->lockedRef.input.isRelative()) {
            assert(info2->parentInputAttrPath);
            if (info2->parentInputAttrPath->empty())
                /* The parent is the top-level flake. */
                info2.reset();
            else
                info2 = lockedFlake.findInput(state, *info2->parentInputAttrPath);
        }

        emitTreeAttrs(
            state,
            storePath,
            info2 ? info2->lockedRef.input : lockedFlake.flake.lockedRef.input,
            attrs.alloc("sourceInfo"),
            false,
            !info2 && lockedFlake.flake.forceDirty);

        attrs.alloc("dir").mkString(CanonPath(subdir).rel(), state.mem);
    }

    v.mkAttrs(attrs);
}

static RegisterPrimOp primop_fetchFlakeInput({
    .name = "__fetchFlakeInput",
    .args = {"lockedFlake", "inputAttrPath"},
    .doc = R"(
      Fetch the flake input of *lockedFlake* denoted by *inputAttrPath*
      (a list of strings, where the empty list denotes the top-level
      flake) and return an attribute set describing it: `flake`
      (whether it's a flake), `buildTime` (whether it's fetched at
      build time), and either `locked` (the locked input attributes,
      for build-time inputs, which are not fetched) or `sourceInfo`
      (the fetched tree's metadata) and `dir` (the subdirectory of the
      flake within `sourceInfo`).
    )",
    .impl = prim_fetchFlakeInput,
    .internal = true,
});

void callFlake(EvalState & state, std::shared_ptr<const LockedFlake> lockedFlake, Value & vRes)
{
    auto vLockedFlake = state.allocValue();
    vLockedFlake->mkExternal(new LockedFlakeValue(std::move(lockedFlake)));

    Value * vCallFlake = requireInternalFile(state, CanonPath("call-flake.nix"));

    Value * args[] = {
        vLockedFlake,
        **get(state.internalPrimOps, "listFlakeInputs"),
        **get(state.internalPrimOps, "fetchFlakeInput"),
    };
    state.callFunction(*vCallFlake, args, vRes, noPos);
}

LockedFlake::~LockedFlake() {}

std::vector<FlakeId> LockedFlake::getInputNames(EvalState & state, const InputAttrPath & prefix) const
{
    std::vector<FlakeId> res;
    for (auto & [name, target] : getInputTargets(state, prefix))
        res.push_back(name);
    return res;
}

InputAttrPath LockedFlake::resolveFollows(EvalState & state, const InputAttrPath & path) const
{
    std::vector<InputAttrPath> visited;

    InputAttrPath res;

    /* The path elements still to be resolved, in reverse order. */
    InputAttrPath todo(path.rbegin(), path.rend());

    while (!todo.empty()) {
        auto name = todo.back();
        todo.pop_back();

        auto targets = getInputTargets(state, res);
        auto i = targets.find(name);

        if (i == targets.end()) {
            /* The input doesn't exist, so there is nothing to
               resolve; return the remaining path unchanged and leave
               it to the caller to deal with it. */
            res.push_back(std::move(name));
            res.insert(res.end(), todo.rbegin(), todo.rend());
            return res;
        }

        if (i->second) {
            /* A "follows" input: restart resolution from its target
               (which is relative to the top-level flake). */
            auto followsPath(res);
            followsPath.push_back(std::move(name));
            if (std::find(visited.begin(), visited.end(), followsPath) != visited.end()) {
                std::vector<std::string> cycle;
                std::transform(visited.begin(), visited.end(), std::back_inserter(cycle), printInputAttrPath);
                cycle.push_back(printInputAttrPath(followsPath));
                throw Error("follow cycle detected: [%s]", concatStringsSep(" -> ", cycle));
            }
            visited.push_back(std::move(followsPath));
            todo.insert(todo.end(), i->second->rbegin(), i->second->rend());
            res.clear();
        } else
            res.push_back(std::move(name));
    }

    return res;
}

void LockedFlake::visit(EvalState & state, VisitCallback callback) const
{
    if (!callback({}, InputInfo{.lockedRef = flake.lockedRef}))
        return;

    /* Note: this is not a recursive lambda using an explicit object
       parameter because that triggers an internal compiler error in
       GCC. */
    std::function<void(const InputAttrPath &)> recurse;

    recurse = [&](const InputAttrPath & prefix) {
        for (auto & [id, target] : getInputTargets(state, prefix)) {
            auto inputAttrPath(prefix);
            inputAttrPath.push_back(id);
            if (target)
                callback(inputAttrPath, *target);
            else if (auto info = findInput(state, inputAttrPath)) {
                if (callback(inputAttrPath, *info) && info->isFlake)
                    recurse(inputAttrPath);
            }
        }
    };

    recurse({});
}

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
