#pragma once
///@file

#include "nix/flake/flake.hh"

#include <nlohmann/json_fwd.hpp>

namespace nix {
class Store;
class StorePath;
} // namespace nix

namespace nix::flake {

struct LockedNode;

/**
 * A node in the lock file. It has outgoing edges to other nodes (its
 * inputs). Only the root node has this type; all other nodes have
 * type LockedNode.
 */
struct Node : std::enable_shared_from_this<Node>
{
    typedef std::variant<ref<LockedNode>, InputAttrPath> Edge;

    std::map<FlakeId, Edge> inputs;

    virtual ~Node() {}
};

/**
 * A non-root node in the lock file.
 */
struct LockedNode : Node
{
    FlakeRef lockedRef, originalRef;
    bool isFlake = true;
    bool buildTime = false;

    /* The node relative to which relative source paths
       (e.g. 'path:../foo') are interpreted. */
    std::optional<InputAttrPath> parentInputAttrPath;

    LockedNode(
        const FlakeRef & lockedRef,
        const FlakeRef & originalRef,
        bool isFlake = true,
        bool buildTime = false,
        std::optional<InputAttrPath> parentInputAttrPath = {})
        : lockedRef(std::move(lockedRef))
        , originalRef(std::move(originalRef))
        , isFlake(isFlake)
        , buildTime(buildTime)
        , parentInputAttrPath(std::move(parentInputAttrPath))
    {
    }

    LockedNode(const fetchers::Settings & fetchSettings, const nlohmann::json & json);

    StorePath computeStorePath(Store & store) const;
};

/**
 * The old graph-based lock file format (versions 5-7).
 */
struct LockFileV7
{
    ref<Node> root = make_ref<Node>();

    LockFileV7() {};
    LockFileV7(const fetchers::Settings & fetchSettings, const nlohmann::json & json, std::string_view path);

    typedef std::map<ref<const Node>, std::string> KeyMap;

    std::pair<nlohmann::json, KeyMap> toJSON() const;

    std::pair<std::string, KeyMap> to_string() const;

    /**
     * Check whether this lock file has any unlocked or non-final
     * inputs. If so, return one.
     */
    std::optional<FlakeRef> isUnlocked(const fetchers::Settings & fetchSettings) const;

    bool operator==(const LockFileV7 & other) const;

    std::shared_ptr<Node> findInput(const InputAttrPath & path) const;

    std::map<InputAttrPath, Node::Edge> getAllInputs() const;

    /**
     * Check that every 'follows' input target exists.
     */
    void check();
};

struct LockedFlakeV7 : LockedFlake
{
    LockFileV7 lockFile;

    /**
     * Source tree accessors for nodes that have been fetched in
     * lockFlake(); in particular, the root node and the overridden
     * inputs.
     * FIXME: move into lockFile?
     */
    std::map<ref<Node>, SourcePath> nodePaths;

    LockedFlakeV7(Flake && flake, LockFileV7 && lockFile, std::map<ref<Node>, SourcePath> && nodePaths)
        : LockedFlake(std::move(flake))
        , lockFile(std::move(lockFile))
        , nodePaths(std::move(nodePaths))
    {
    }

    /**
     * Construct from the JSON contents of a lock file (which must be
     * null if the lock file doesn't exist).
     */
    LockedFlakeV7(
        const fetchers::Settings & fetchSettings, Flake flake, const nlohmann::json & json, std::string_view path);

    std::vector<FlakeId> getInputNames(const InputAttrPath & prefix) const override;

    std::optional<InputInfo> findInput(const InputAttrPath & path) const override;

    void visit(VisitCallback callback) const override;

    std::optional<FlakeRef> isUnlocked(const fetchers::Settings & fetchSettings) const override;

    std::string diff(const LockedFlake & oldLockFile) const override;

    nlohmann::json toJSON() const override;

    /**
     * Compute a lock file for `flake`, reusing entries from
     * `oldLockFile` (which must be a `LockedFlakeV7`) where
     * possible. Note: this does not write the new lock file.
     */
    static std::unique_ptr<LockedFlake> lockFlake(
        const Settings & settings,
        EvalState & state,
        const LockFlags & lockFlags,
        Flake flake,
        const LockedFlake & oldLockFile);
};

} // namespace nix::flake
