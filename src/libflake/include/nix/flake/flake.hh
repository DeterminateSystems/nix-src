#pragma once
///@file

#include "nix/util/types.hh"
#include "nix/flake/flakeref.hh"
#include "nix/flake/input-attr-path.hh"
#include "nix/expr/value.hh"
#include "nix/expr/eval-cache.hh"

#include <functional>

namespace nix {

class EvalState;
struct Provenance;

namespace flake {

struct Settings;

struct FlakeInput;

typedef std::map<FlakeId, FlakeInput> FlakeInputs;

/**
 * FlakeInput is the 'Flake'-level parsed form of the "input" entries
 * in the flake file.
 *
 * A FlakeInput is normally constructed by the 'parseFlakeInput'
 * function which parses the input specification in the '.flake' file
 * to create a 'FlakeRef' (a fetcher, the fetcher-specific
 * representation of the input specification, and possibly the fetched
 * local store path result) and then creating this FlakeInput to hold
 * that FlakeRef, along with anything that might override that
 * FlakeRef (like command-line overrides or "follows" specifications).
 *
 * A FlakeInput is also sometimes constructed directly from a FlakeRef
 * instead of starting at the flake-file input specification
 * (e.g. overrides, follows, and implicit inputs).
 *
 * A FlakeInput will usually have one of either "ref" or "follows"
 * set.  If not otherwise specified, a "ref" will be generated to a
 * 'type="indirect"' flake, which is treated as simply the name of a
 * flake to be resolved in the registry.
 */

struct FlakeInput
{
    std::optional<FlakeRef> ref;

    /**
     * Whether to call the `flake.nix` file in this input to get its outputs.
     */
    bool isFlake = true;

    /**
     * Whether to fetch this input at evaluation time or at build
     * time.
     */
    bool buildTime = false;

    std::optional<InputAttrPath> follows;
    FlakeInputs overrides;
};

struct ConfigFile
{
    using ConfigValue = std::variant<std::string, int64_t, Explicit<bool>, std::vector<std::string>>;

    std::map<std::string, ConfigValue> settings;

    void apply(const Settings & settings);
};

/**
 * A flake in context
 */
struct Flake
{
    /**
     * The original flake specification (by the user)
     */
    FlakeRef originalRef;

    /**
     * registry references and caching resolved to the specific underlying flake
     */
    FlakeRef resolvedRef;

    /**
     * The flakeref returned by the fetcher. Note that this is a misnomer and it might not actually be locked (e.g. a
     * dirty Git repo).
     */
    FlakeRef lockedRef;

    /**
     * The path of `flake.nix`.
     */
    SourcePath path;

    /**
     * Cached provenance of `flake.nix` (equivalent to `path.getProvenance()`).
     */
    std::shared_ptr<const Provenance> provenance;

    /**
     * Pretend that `lockedRef` is dirty.
     */
    bool forceDirty = false;

    std::optional<std::string> description;

    FlakeInputs inputs;

    /**
     * Attributes to be retroactively applied to the `self` input
     * (such as `submodules = true`).
     */
    fetchers::Attrs selfAttrs;

    /**
     * 'nixConfig' attribute
     */
    ConfigFile config;

    ~Flake();

    SourcePath lockFilePath()
    {
        return path.parent() / "flake.lock";
    }
};

Flake getFlake(
    EvalState & state, const FlakeRef & flakeRef, fetchers::UseRegistries useRegistries, bool requireLockable = true);

Flake getFlake(
    EvalState & state,
    const FlakeRef & originalRef,
    fetchers::UseRegistries useRegistries,
    const InputAttrPath & lockRootAttrPath,
    bool requireLockable);

/**
 * Fingerprint of a locked flake; used as a cache key.
 */
typedef Hash Fingerprint;

struct LockedFlake
{
    Flake flake;

    LockedFlake(Flake && flake)
        : flake(std::move(flake))
    {
    }

    virtual ~LockedFlake();

    /**
     * For the input denoted by `prefix` (or the top-level flake if
     * `prefix` is empty), return a map from the names of its inputs
     * to the target of that input: for a regular input, std::nullopt;
     * for a "follows" input, the input attribute path (relative to
     * the top-level flake) of the immediate target of the
     * "follows". Note that the target may itself denote a "follows"
     * input. Throws an error if `prefix` does not denote an existing
     * input.
     */
    virtual std::map<FlakeId, std::optional<InputAttrPath>> getInputTargets(const InputAttrPath & prefix) const = 0;

    /**
     * Return the names of the inputs of the input denoted by
     * `prefix`, or of the top-level flake if `prefix` is empty.
     */
    std::vector<FlakeId> getInputNames(const InputAttrPath & prefix) const;

    /**
     * Information about a locked input.
     */
    struct InputInfo
    {
        FlakeRef lockedRef;
        bool isFlake = true;
        bool buildTime = false;

        /**
         * For relative path inputs, the input attribute path of the
         * flake relative to which the path is interpreted.
         */
        std::optional<InputAttrPath> parentInputAttrPath;
    };

    /**
     * Return information about the input denoted by `path`, resolving
     * 'follows' indirections. Returns std::nullopt if the input does
     * not exist.
     */
    virtual std::optional<InputInfo> findInput(const InputAttrPath & path) const = 0;

    /**
     * Return the source path of the input denoted by `inputAttrPath`
     * (or of the top-level flake if `inputAttrPath` is empty),
     * fetching it if necessary. Note: the returned path is backed by
     * `EvalState::rootFS` (i.e. it's a store path, possibly a virtual
     * one that has the input's accessor mounted on it if lazy trees
     * are enabled), not by the input's original accessor.
     */
    virtual SourcePath getSourcePath(EvalState & state, const InputAttrPath & inputAttrPath) const = 0;

    /**
     * Callback for `visit()`. The second argument is either an
     * `InputInfo` for locked inputs, or, for "follows" inputs, the
     * input attribute path of the target of the "follows" (relative
     * to the top-level flake). The return value denotes whether
     * `visit()` should recurse into the inputs of this input.
     */
    using VisitCallback =
        std::function<bool(const InputAttrPath & inputAttrPath, const std::variant<InputInfo, InputAttrPath> & input)>;

    /**
     * Call `callback` for every transitive input of this flake,
     * including the root (which has the empty input attribute
     * path). Inputs are visited in depth-first order, parents before
     * children. If the callback returns false, we do not recurse into
     * the inputs of that input. We never recurse into "follows"
     * inputs; their targets are visited under their own paths.
     */
    virtual void visit(VisitCallback callback) const = 0;

    std::optional<Fingerprint> getFingerprint(Store & store, const fetchers::Settings & fetchSettings) const;

    /**
     * Check whether the lock file has any unlocked or non-final
     * inputs. If so, return one.
     */
    virtual std::optional<FlakeRef> isUnlocked(const fetchers::Settings & fetchSettings) const = 0;

    /**
     * Return a human-readable description of the differences between
     * the (older) `oldLockFile` and the lock file of this flake.
     */
    virtual std::string diff(const LockedFlake & oldLockFile) const = 0;

    virtual nlohmann::json toJSON() const = 0;

    std::string to_string() const;
};

std::ostream & operator<<(std::ostream & stream, const LockedFlake & lockedFlake);

struct LockFlags
{
    /**
     * Whether to ignore the existing lock file, creating a new one
     * from scratch.
     */
    bool recreateLockFile = false;

    /**
     * Whether to update the lock file at all. If set to false, if any
     * change to the lock file is needed (e.g. when an input has been
     * added to flake.nix), you get a fatal error.
     */
    bool updateLockFile = true;

    /**
     * Whether to write the lock file to disk. If set to true, if the
     * any changes to the lock file are needed and the flake is not
     * writable (i.e. is not a local Git working tree or similar), you
     * get a fatal error. If set to false, Nix will use the modified
     * lock file in memory only, without writing it to disk.
     */
    bool writeLockFile = true;

    /**
     * Throw an exception when the flake has an unlocked input.
     */
    bool failOnUnlocked = false;

    /**
     * Whether to use the registries to lookup indirect flake
     * references like 'nixpkgs'.
     */
    std::optional<bool> useRegistries = std::nullopt;

    /**
     * Whether to apply flake's nixConfig attribute to the configuration
     */

    bool applyNixConfig = false;

    /**
     * Whether unlocked flake references (i.e. those without a Git
     * revision or similar) without a corresponding lock are
     * allowed. Unlocked flake references with a lock are always
     * allowed.
     */
    bool allowUnlocked = true;

    /**
     * Whether to commit changes to flake.lock.
     */
    bool commitLockFile = false;

    /**
     * The path to a lock file to read instead of the `flake.lock` file in the top-level flake
     */
    std::optional<SourcePath> referenceLockFilePath;

    /**
     * The path to a lock file to write to instead of the `flake.lock` file in the top-level flake
     */
    std::optional<std::filesystem::path> outputLockFilePath;

    /**
     * Flake inputs to be overridden.
     */
    std::map<NonEmptyInputAttrPath, FlakeRef> inputOverrides;

    /**
     * Flake inputs to be updated. This means that any existing lock
     * for those inputs will be ignored.
     */
    std::set<NonEmptyInputAttrPath> inputUpdates;

    /**
     * Whether to require a locked input.
     */
    bool requireLockable = true;
};

/**
 * Return a `Flake` object representing the flake read from the
 * `flake.nix` file in `rootDir`.
 */
Flake readFlake(
    EvalState & state,
    const FlakeRef & originalRef,
    const FlakeRef & resolvedRef,
    const FlakeRef & lockedRef,
    const SourcePath & rootDir,
    const InputAttrPath & lockRootPath);

/*
 * Compute an in-memory lock file for the specified top-level flake, and optionally write it to file, if the flake is
 * writable.
 */
std::unique_ptr<LockedFlake>
lockFlake(const Settings & settings, EvalState & state, const FlakeRef & flakeRef, const LockFlags & lockFlags);

std::unique_ptr<LockedFlake> lockFlake(
    const Settings & settings, EvalState & state, const FlakeRef & topRef, const LockFlags & lockFlags, Flake flake);

std::unique_ptr<LockedFlake>
lockFlake(const Settings & settings, EvalState & state, const SourcePath & flakeDir, const LockFlags & lockFlags);

/**
 * Parse a lock file in the old graph-based format (versions 5-7).
 * `json` must be null if the lock file doesn't exist.
 */
std::unique_ptr<LockedFlake> parseLockFileV7(
    const fetchers::Settings & fetchSettings, Flake flake, const nlohmann::json & json, std::string_view path);

/**
 * Compute a version 7 lock file for `flake`, reusing entries from
 * `oldLockFile` (which must have been produced by `parseLockFileV7()`)
 * where possible. Note: this does not write the new lock file.
 */
std::unique_ptr<LockedFlake> lockFlakeV7(
    const Settings & settings,
    EvalState & state,
    const LockFlags & lockFlags,
    Flake flake,
    const LockedFlake & oldLockFile);

void callFlake(EvalState & state, std::shared_ptr<const LockedFlake> lockedFlake, Value & v);

} // namespace flake

} // namespace nix
