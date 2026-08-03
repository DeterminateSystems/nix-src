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

    std::map<FlakeId, std::optional<InputAttrPath>> getInputTargets(const InputAttrPath & prefix) const override
    {
        notImplemented("getInputTargets");
    }

    std::optional<InputInfo> findInput(const InputAttrPath & path) const override
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
    const LockedFlake & oldLockFile)
{
    // FIXME: implement the version 8 lock algorithm.
    throw Error("creating version 8 lock files is not implemented yet");
}

} // namespace nix::flake
