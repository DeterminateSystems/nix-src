#include <nlohmann/json.hpp>
#include <functional>
#include <iomanip>
#include <ctime>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nix/fetchers/fetch-settings.hh"
#include "nix/flake/flake.hh"
#include "flake-impl.hh"
#include "nix/flake/settings.hh"
#include "nix/util/sync.hh"
#include "nix/expr/eval.hh"
#include "nix/store/store-api.hh"
#include "nix/fetchers/attrs.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/fetchers/input-cache.hh"
#include "nix/flake/flakeref.hh"
#include "nix/util/ansicolor.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/error.hh"
#include "nix/util/finally.hh"
#include "nix/util/fmt.hh"
#include "nix/util/logging.hh"
#include "nix/util/types.hh"
#include "nix/util/util.hh"

namespace nix::flake {

static FlakeRef getFlakeRef(const fetchers::Settings & fetchSettings, const nlohmann::json & json, const char * attr)
{
    auto i = json.find(attr);
    if (i == json.end())
        throw Error("attribute '%s' missing in lock file", attr);
    return FlakeRef::fromAttrs(fetchSettings, fetchers::jsonToAttrs(*i));
}

/**
 * The sparse lock file format (version 8). Unlike the graph-based
 * versions 5-7, it only stores the immediate inputs of the flake,
 * plus any overrides of transitive inputs; the rest of the dependency
 * graph is resolved at evaluation time from the inputs' own lock
 * files.
 */
struct LockFileV8
{
    struct Lock
    {
        FlakeRef originalRef, lockedRef;

        /**
         * The locks for the transitive inputs of this input. Only
         * present if the input does not have a lock file of its
         * own. The keys are relative to this input.
         */
        std::unique_ptr<LockFileV8> locks;

        /**
         * The source path of this input, if it has been fetched.
         */
        mutable Sync<std::optional<SourcePath>> sourcePath;

        Lock(FlakeRef originalRef, FlakeRef lockedRef)
            : originalRef(std::move(originalRef))
            , lockedRef(std::move(lockedRef))
        {
        }

        Lock(const fetchers::Settings & fetchSettings, const nlohmann::json & json)
            : originalRef(getFlakeRef(fetchSettings, json, "original"))
            , lockedRef(getFlakeRef(fetchSettings, json, "locked"))
        {
            if (!lockedRef.input.isLocked(fetchSettings)) {
                if (lockedRef.input.getNarHash())
                    warn(
                        "Lock file entry '%s' is unlocked (e.g. lacks a Git revision) but is checked by NAR hash. "
                        "This is not reproducible and will break after garbage collection or when shared.",
                        lockedRef.to_string());
                else
                    throw Error(
                        "Lock file contains unlocked input '%s'. Use '--allow-dirty-locks' to accept this lock file.",
                        fetchers::attrsToJSON(lockedRef.input.toAttrs()));
            }

            // For backward compatibility, lock file entries are implicitly final.
            assert(!lockedRef.input.attrs.contains("__final"));
            lockedRef.input.attrs.insert_or_assign("__final", Explicit<bool>(true));

            if (auto locks = json.find("locks"); locks != json.end())
                this->locks = std::make_unique<LockFileV8>(fetchSettings, *locks);
        }

        /**
         * Return a deep copy of this lock, without the `sourcePath`
         * cache. (An explicit method since `Sync` is not copyable.)
         */
        Lock clone() const
        {
            Lock lock(originalRef, lockedRef);
            if (locks)
                lock.locks = std::make_unique<LockFileV8>(locks->clone());
            return lock;
        }

        nlohmann::json toJSON() const
        {
            nlohmann::json n;
            n["original"] = fetchers::attrsToJSON(originalRef.toAttrs());
            n["locked"] = fetchers::attrsToJSON(lockedRef.toAttrs());
            assert(lockedRef.input.isFinal());
            if (locks)
                n["locks"] = locks->toLocksJSON();
            return n;
        }
    };

    /**
     * The locks, keyed by slash-separated input attribute paths. A
     * single-element key denotes an immediate input of the flake;
     * longer keys denote overrides of transitive inputs.
     */
    std::map<NonEmptyInputAttrPath, Lock> locks;

    LockFileV8() = default;

    /**
     * Construct from a `locks` attribute of a lock file.
     */
    LockFileV8(const fetchers::Settings & fetchSettings, const nlohmann::json & locksJson)
    {
        for (auto & i : locksJson.items()) {
            auto path = NonEmptyInputAttrPath::parse(i.key());
            if (!path)
                throw Error("lock file contains an empty input attribute path");
            locks.emplace(std::move(*path), Lock(fetchSettings, i.value()));
        }
    }

    /**
     * Construct from the JSON contents of a lock file.
     */
    LockFileV8(const fetchers::Settings & fetchSettings, const nlohmann::json & json, std::string_view path)
    {
        auto version = json.value("version", 0);
        if (version != 8)
            throw Error("lock file '%s' has unsupported version %d", path, version);

        if (auto locks = json.find("locks"); locks != json.end())
            *this = LockFileV8(fetchSettings, *locks);
    }

    LockFileV8 clone() const
    {
        LockFileV8 res;
        for (auto & [path, lock] : locks)
            res.locks.emplace(path, lock.clone());
        return res;
    }

    nlohmann::json toLocksJSON() const
    {
        auto res = nlohmann::json::object();
        for (auto & [path, lock] : locks)
            res[printInputAttrPath(path)] = lock.toJSON();
        return res;
    }

    nlohmann::json toJSON() const
    {
        nlohmann::json json;
        json["version"] = 8;
        json["locks"] = toLocksJSON();
        return json;
    }

    /**
     * Check whether this lock file has any unlocked or non-final
     * inputs. If so, return one.
     */
    std::optional<FlakeRef> isUnlocked(const fetchers::Settings & fetchSettings) const
    {
        /* Return whether the input is either locked, or, if
           `allow-dirty-locks` is enabled, it has a NAR hash. In the
           latter case, we can verify the input but we may not be able to
           fetch it from anywhere. */
        auto isConsideredLocked = [&](const fetchers::Input & input) {
            return input.isLocked(fetchSettings) || (fetchSettings.allowDirtyLocks && input.getNarHash());
        };

        for (auto & [path, lock] : locks) {
            if (!isConsideredLocked(lock.lockedRef.input) || !lock.lockedRef.input.isFinal())
                return lock.lockedRef;
            if (lock.locks)
                if (auto ref = lock.locks->isUnlocked(fetchSettings))
                    return ref;
        }

        return std::nullopt;
    }

    /**
     * Flatten this lock file into a map from absolute input attribute
     * paths to lock entries. Inline locks appear under the path of
     * their containing entry. A colliding top-level (override) entry
     * shadows an inline entry, matching the precedence of overrides
     * at evaluation time.
     */
    void
    getAllLockEntries(std::map<InputAttrPath, LockedFlake::LockEntry> & res, const InputAttrPath & prefix = {}) const
    {
        for (auto & [path, lock] : locks) {
            InputAttrPath absPath(prefix);
            absPath.insert(absPath.end(), path.get().begin(), path.get().end());
            if (lock.locks)
                lock.locks->getAllLockEntries(res, absPath);
            /* Note: this shadows any colliding inline entry, since
               the entry for the containing input sorts before the
               override path and thus has been recursed into
               already. */
            res.insert_or_assign(std::move(absPath), lock.lockedRef);
        }
    }
};

struct LockedFlakeV8 : LockedFlake
{
    /**
     * The lock file in the sparse format (version 8).
     */
    LockFileV8 lockFile;

    LockedFlakeV8(Flake && flake, LockFileV8 && lockFile)
        : LockedFlake(std::move(flake))
        , lockFile(std::move(lockFile))
    {
    }

    /**
     * Construct from the JSON contents of a lock file (which must be
     * null if the lock file doesn't exist).
     */
    LockedFlakeV8(
        const fetchers::Settings & fetchSettings, Flake flake, const nlohmann::json & json, std::string_view path)
        : LockedFlake(std::move(flake))
        , lockFile(json.is_null() ? LockFileV8() : LockFileV8(fetchSettings, json, path))
    {
    }

    [[noreturn]] static void notImplemented(std::string_view what)
    {
        throw Error("'%s' is not implemented yet for lock file version 8", what);
    }

    std::map<FlakeId, std::optional<InputAttrPath>>
    getInputTargets(EvalState & state, const InputAttrPath & prefix) const override
    {
        notImplemented("getInputTargets");
    }

    std::optional<InputInfo> findInput(EvalState & state, const InputAttrPath & path) const override
    {
        notImplemented("findInput");
    }

    SourcePath getSourcePath(EvalState & state, const InputAttrPath & inputAttrPath) const override
    {
        notImplemented("getSourcePath");
    }

    std::optional<FlakeRef> isUnlocked(const fetchers::Settings & fetchSettings) const override
    {
        return lockFile.isUnlocked(fetchSettings);
    }

    unsigned int version() const override
    {
        return 8;
    }

    std::map<InputAttrPath, LockEntry> getAllLockEntries(bool fetchTransitive) const override
    {
        if (fetchTransitive)
            throw Error("fetching transitive lock files is not implemented yet for lock file version 8");

        std::map<InputAttrPath, LockEntry> res;
        lockFile.getAllLockEntries(res);
        return res;
    }

    nlohmann::json toJSON() const override
    {
        return lockFile.toJSON();
    }
};

std::unique_ptr<LockedFlake> parseLockFileV8(
    const fetchers::Settings & fetchSettings, Flake flake, const nlohmann::json & json, std::string_view path)
{
    return std::make_unique<LockedFlakeV8>(fetchSettings, std::move(flake), json, path);
}

LockFlakeResult lockFlakeV8(
    const Settings & settings,
    EvalState & state,
    const LockFlags & lockFlags,
    Flake flake,
    const LockedFlake & _oldLockFile)
{
    /* The old lock file to reuse entries from. Null if the old lock
       file is not a version 8 lock file (e.g. when migrating from
       version 7), or if we're relocking from scratch. Note that
       updating all inputs (`inputUpdates` = nullopt) ignores the old
       lock file, but lock file entries can then still be *copied*
       from dependencies' own lock files. */
    const LockFileV8 * oldLockFile = nullptr;
    if (auto old = dynamic_cast<const LockedFlakeV8 *>(&_oldLockFile);
        old && !lockFlags.recreateLockFile && lockFlags.inputUpdates)
        oldLockFile = &old->lockFile;

    auto useRegistries = lockFlags.useRegistries.value_or(settings.useRegistries);
    auto useRegistriesInputs = useRegistries ? fetchers::UseRegistries::Limited : fetchers::UseRegistries::No;

    std::set<NonEmptyInputAttrPath> overridesUsed;
    std::set<InputAttrPath> updatesUsed;
    std::set<NonEmptyInputAttrPath> explicitCliOverrides;

    /* Apply command line overrides as if they were overrides declared
       by the top-level flake (`inputs.foo.inputs.bar.url = ...`),
       creating intermediate override entries as needed. They
       overwrite any conflicting override in `flake.nix` ("outermost
       override wins"). Note: overrides of inputs that don't exist at
       the top level are left unapplied so the caller can warn about
       them. */
    for (auto & [path, ref] : lockFlags.inputOverrides) {
        auto input = get(flake.inputs, path.get().front());
        if (!input)
            continue;
        for (auto & elem : std::views::drop(path.get(), 1))
            input = &input->overrides[elem];
        input->ref = ref;
        input->follows = std::nullopt;
        overridesUsed.insert(path);
        explicitCliOverrides.insert(path);
    }

    LockFileV8 newLockFile;

    std::vector<FlakeRef> parents;

    std::function<void(
        const FlakeInputs & flakeInputs,
        LockFileV8 & output,
        const InputAttrPath & absPrefix,
        const LockFileV8 * oldLocks,
        const SourcePath & sourcePath)>
        computeLocks;

    computeLocks = [&](
                       /* The declared inputs of the flake being
                          locked (from its flake.nix). */
                       const FlakeInputs & flakeInputs,
                       /* The lock file being computed for this
                          flake. */
                       LockFileV8 & output,
                       /* The absolute input attribute path of this
                          flake (empty for the top-level flake). */
                       const InputAttrPath & absPrefix,
                       /* The old locks, if any, from which locks can
                          be copied. */
                       const LockFileV8 * oldLocks,
                       /* The path of this flake's `flake.nix`. */
                       const SourcePath & sourcePath) {
        debug("computing lock file entries for '%s'", printInputAttrPath(absPrefix));

        /* Compute the lock for a single input or override declared by
           this flake. `relPath` is relative to this flake. Returns
           std::nullopt for inputs that are not stored in the lock
           file ('follows' and relative path inputs). */
        auto createLock = [&](const NonEmptyInputAttrPath & relPath,
                              const FlakeInput & input) -> std::optional<LockFileV8::Lock> {
            InputAttrPath absPath(absPrefix);
            absPath.insert(absPath.end(), relPath.get().begin(), relPath.get().end());
            auto nonEmptyAbsPath = *NonEmptyInputAttrPath::make(absPath);
            auto absPathS = printInputAttrPath(absPath);
            debug("computing input '%s'", absPathS);

            try {
                updatesUsed.insert(absPath);

                if (input.follows) {
                    /* 'follows' inputs are not stored in the lock
                       file; they are resolved at evaluation time from
                       the flake.nix files. */
                    if (!input.overrides.empty())
                        throw Error(
                            "input '%s' has both 'follows' and overrides for its inputs, which is not supported by lock file version 8",
                            absPathS);
                    return std::nullopt;
                }

                auto ref = input.ref.value_or(
                    FlakeRef::fromAttrs(
                        state.fetchSettings, {{"type", "indirect"}, {"id", std::string(relPath.inputName())}}));

                if (auto relativePath = ref.input.isRelative()) {
                    /* Relative path inputs (e.g. 'path:./foo') are
                       not stored in the lock file, since they change
                       along with the flake that declares them. If
                       they're flakes, they must have a lock file of
                       their own, which is used at evaluation time. */
                    SourcePath resolved{
                        sourcePath.accessor, CanonPath(relativePath->string(), sourcePath.path.parent().value())};
                    if (input.isFlake && !(resolved / "flake.lock").pathExists())
                        throw Error(
                            "relative path input '%s' does not have a lock file; run 'nix flake lock %s' to create it",
                            absPathS,
                            resolved);
                    return std::nullopt;
                }

                auto explicitUpdate = lockFlags.inputUpdates && lockFlags.inputUpdates->count(nonEmptyAbsPath);

                auto oldLock = oldLocks ? get(oldLocks->locks, relPath) : nullptr;

                if (oldLock && !explicitUpdate && oldLock->originalRef.canonicalize() == ref.canonicalize()) {
                    /* Copy the input from the old lock file since its
                       flakeref didn't change. */

                    /* Check whether an explicit update of an input
                     *below* this one is requested. */
                    bool mustRefetch = false;
                    if (lockFlags.inputUpdates) {
                        auto lb = lockFlags.inputUpdates->lower_bound(nonEmptyAbsPath);
                        mustRefetch = lb != lockFlags.inputUpdates->end() && lb->get().size() > absPath.size()
                                      && std::equal(absPath.begin(), absPath.end(), lb->get().begin());
                    }

                    /* If so, and this input's transitive inputs are
                       locked here (because it has no lock file of its
                       own), refetch it and recompute its inline
                       locks. Otherwise the update path doesn't match
                       anything we can update, and the caller will
                       warn about it. */
                    if (!mustRefetch || !oldLock->locks) {
                        debug("keeping existing input '%s'", absPathS);
                        return oldLock->clone();
                    }

                    auto inputFlake = getFlake(state, oldLock->lockedRef, useRegistriesInputs, absPath, true);

                    LockFileV8::Lock lock(oldLock->originalRef, oldLock->lockedRef);
                    *lock.sourcePath.lock() = inputFlake.path.parent();
                    lock.locks = std::make_unique<LockFileV8>();
                    computeLocks(inputFlake.inputs, *lock.locks, absPath, oldLock->locks.get(), inputFlake.path);
                    return lock;
                }

                /* We need to create a new lock file entry. So fetch
                   this input. */
                debug("creating new input '%s'", absPathS);

                if (!lockFlags.allowUnlocked && !ref.input.isLocked(state.fetchSettings))
                    throw Error("cannot update unlocked flake input '%s' in pure mode", absPathS);

                auto useRegistriesInput =
                    explicitCliOverrides.contains(nonEmptyAbsPath) ? fetchers::UseRegistries::All : useRegistriesInputs;

                if (input.isFlake) {
                    auto inputFlake = getFlake(state, ref, useRegistriesInput, absPath, true);

                    warnRegistry(absPath, ref, inputFlake.resolvedRef, flake.path);

                    LockFileV8::Lock lock(ref, inputFlake.lockedRef);
                    *lock.sourcePath.lock() = inputFlake.path.parent();

                    /* If the input doesn't have a lock file of its
                       own, lock its transitive inputs here, in the
                       `locks` attribute of this entry. */
                    if (!inputFlake.lockFilePath().pathExists()) {
                        /* Guard against circular flake imports. */
                        for (auto & parent : parents)
                            if (parent == ref)
                                throw Error("found circular import of flake '%s'", parent);
                        parents.push_back(ref);
                        Finally cleanup([&]() { parents.pop_back(); });

                        lock.locks = std::make_unique<LockFileV8>();
                        computeLocks(
                            inputFlake.inputs,
                            *lock.locks,
                            absPath,
                            oldLock && oldLock->locks ? oldLock->locks.get() : nullptr,
                            inputFlake.path);
                    }

                    return lock;
                } else {
                    auto cachedInput =
                        state.inputCache->getAccessor(state.fetchSettings, *state.store, ref.input, useRegistriesInput);

                    auto resolvedRef = FlakeRef(std::move(cachedInput.resolvedInput), ref.subdir);
                    auto lockedRef = FlakeRef(std::move(cachedInput.lockedInput), ref.subdir);

                    warnRegistry(absPath, ref, resolvedRef, flake.path);

                    /* Note: `mountInput()` adds a NAR hash to
                       `lockedRef.input` if it doesn't have one. */
                    auto storePath =
                        state.storePath(state.mountInput(lockedRef.input, ref.input, cachedInput.accessor, true, true));

                    LockFileV8::Lock lock(ref, lockedRef);
                    *lock.sourcePath.lock() = storePath;
                    return lock;
                }

            } catch (Error & e) {
                e.addTrace({}, "while updating the flake input '%s'", absPathS);
                throw;
            }
        };

        for (auto & [id, input] : flakeInputs) {
            auto relPath = NonEmptyInputAttrPath::append({}, id);

            if (auto lock = createLock(relPath, input))
                output.locks.emplace(relPath, std::move(*lock));

            /* Store the overrides declared by this flake for the
               transitive inputs of this input
               (e.g. `inputs.foo.inputs.bar.url = ...`), keyed by
               their path relative to this flake. */
            [&](this const auto & recurse, const NonEmptyInputAttrPath & prefix, const FlakeInput & input) -> void {
                for (auto & [id2, override] : input.overrides) {
                    auto relPath2 = NonEmptyInputAttrPath::append(prefix, id2);
                    if (override.follows) {
                        /* 'follows' overrides are not stored; they
                           are resolved at evaluation time from the
                           flake.nix files. */
                        if (!override.overrides.empty())
                            throw Error(
                                "input '%s' has both 'follows' and overrides for its inputs, which is not supported by lock file version 8",
                                printInputAttrPath(relPath2));
                        continue;
                    }
                    if (override.ref)
                        if (auto lock = createLock(relPath2, override))
                            output.locks.emplace(relPath2, std::move(*lock));
                    recurse(relPath2, override);
                }
            }(relPath, input);
        }
    };

    computeLocks(flake.inputs, newLockFile, {}, oldLockFile, flake.path);

    return {
        .lockedFlake = std::make_unique<LockedFlakeV8>(std::move(flake), std::move(newLockFile)),
        .overridesUsed = std::move(overridesUsed),
        .updatesUsed = std::move(updatesUsed),
    };
}

} // namespace nix::flake
