#include <boost/unordered/unordered_flat_set.hpp>
#include <nlohmann/json.hpp>
#include <assert.h>
#include <boost/unordered/unordered_flat_set_fwd.hpp>
#include <nlohmann/detail/iterators/iter_impl.hpp>
#include <nlohmann/detail/iterators/iteration_proxy.hpp>
#include <nlohmann/json_fwd.hpp>
#include <algorithm>
#include <iomanip>
#include <iterator>
#include <ctime>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "nix/fetchers/fetch-settings.hh"
#include "nix/flake/lockfile-v7.hh"
#include "nix/flake/settings.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/nixexpr.hh"
#include "nix/store/store-api.hh"
#include "nix/fetchers/input-cache.hh"
#include "nix/util/finally.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/strings.hh"
#include "nix/fetchers/attrs.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/flake/flakeref.hh"
#include "nix/store/path.hh"
#include "nix/util/ansicolor.hh"
#include "nix/util/error.hh"
#include "nix/util/fmt.hh"
#include "nix/util/json-utils.hh"
#include "nix/util/logging.hh"
#include "nix/util/ref.hh"
#include "nix/util/types.hh"
#include "nix/util/util.hh"

namespace nix {
class Store;
} // namespace nix

namespace nix::flake {

static FlakeRef
getFlakeRef(const fetchers::Settings & fetchSettings, const nlohmann::json & json, const char * attr, const char * info)
{
    auto i = json.find(attr);
    if (i != json.end()) {
        auto attrs = fetchers::jsonToAttrs(*i);
        // FIXME: remove when we drop support for version 5.
        if (info) {
            auto j = json.find(info);
            if (j != json.end()) {
                for (auto k : fetchers::jsonToAttrs(*j))
                    attrs.insert_or_assign(k.first, k.second);
            }
        }
        return FlakeRef::fromAttrs(fetchSettings, attrs);
    }

    throw Error("attribute '%s' missing in lock file", attr);
}

LockedNode::LockedNode(const fetchers::Settings & fetchSettings, const nlohmann::json & json)
    : lockedRef(getFlakeRef(fetchSettings, json, "locked", "info")) // FIXME: remove "info"
    , originalRef(getFlakeRef(fetchSettings, json, "original", nullptr))
    , isFlake(json.find("flake") != json.end() ? (bool) json["flake"] : true)
    , buildTime(json.find("buildTime") != json.end() ? (bool) json["buildTime"] : false)
    , parentInputAttrPath(
          json.find("parent") != json.end() ? (std::optional<InputAttrPath>) json["parent"] : std::nullopt)
{
    if (!lockedRef.input.isLocked(fetchSettings) && !lockedRef.input.isRelative()) {
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
}

StorePath LockedNode::computeStorePath(Store & store) const
{
    return lockedRef.input.computeStorePath(store);
}

static std::shared_ptr<Node>
doFind(const ref<Node> & root, const InputAttrPath & path, std::vector<InputAttrPath> & visited)
{
    auto pos = root;

    auto found = std::find(visited.cbegin(), visited.cend(), path);

    if (found != visited.end()) {
        std::vector<std::string> cycle;
        std::transform(found, visited.cend(), std::back_inserter(cycle), printInputAttrPath);
        cycle.push_back(printInputAttrPath(path));
        throw Error("follow cycle detected: [%s]", concatStringsSep(" -> ", cycle));
    }
    visited.push_back(path);

    for (auto & elem : path) {
        if (auto i = get(pos->inputs, elem)) {
            if (auto node = std::get_if<0>(&*i))
                pos = *node;
            else if (auto follows = std::get_if<1>(&*i)) {
                if (auto p = doFind(root, *follows, visited))
                    pos = ref(p);
                else
                    return {};
            }
        } else
            return {};
    }

    return pos;
}

std::shared_ptr<Node> LockFileV7::findInput(const InputAttrPath & path) const
{
    std::vector<InputAttrPath> visited;
    return doFind(root, path, visited);
}

LockFileV7::LockFileV7(const fetchers::Settings & fetchSettings, const nlohmann::json & json, std::string_view path)
{
    auto version = json.value("version", 0);
    if (version < 5 || version > 7)
        throw Error("lock file '%s' has unsupported version %d", path, version);

    std::string rootKey = json["root"];
    std::map<std::string, ref<Node>> nodeMap{{rootKey, root}};

    [&](this const auto & getInputs, Node & node, const nlohmann::json & jsonNode) {
        if (jsonNode.find("inputs") == jsonNode.end())
            return;
        for (auto & i : jsonNode["inputs"].items()) {
            if (i.value().is_array()) { // FIXME: remove, obsolete
                InputAttrPath path;
                for (auto & j : i.value())
                    path.push_back(j);
                node.inputs.insert_or_assign(i.key(), path);
            } else {
                std::string inputKey = i.value();
                auto k = nodeMap.find(inputKey);
                if (k == nodeMap.end()) {
                    auto & nodes = json["nodes"];
                    auto jsonNode2 = nodes.find(inputKey);
                    if (jsonNode2 == nodes.end())
                        throw Error("lock file references missing node '%s'", inputKey);
                    auto input = make_ref<LockedNode>(fetchSettings, *jsonNode2);
                    k = nodeMap.insert_or_assign(inputKey, input).first;
                    getInputs(*input, *jsonNode2);
                }
                if (auto child = k->second.dynamic_pointer_cast<LockedNode>())
                    node.inputs.insert_or_assign(i.key(), ref(child));
                else
                    // FIXME: replace by follows node
                    throw Error("lock file contains cycle to root node");
            }
        }
    }(*root, json["nodes"][rootKey]);

    // FIXME: check that there are no cycles in version >= 7. Cycles
    // between inputs are only possible using 'follows' indirections.
    // Once we drop support for version <= 6, we can simplify the code
    // a bit since we don't need to worry about cycles.
}

std::pair<nlohmann::json, LockFileV7::KeyMap> LockFileV7::toJSON() const
{
    nlohmann::json nodes;
    KeyMap nodeKeys;
    boost::unordered_flat_set<std::string> keys;

    auto dumpNode = [&](this auto & dumpNode, std::string key, ref<const Node> node) -> std::string {
        auto k = nodeKeys.find(node);
        if (k != nodeKeys.end())
            return k->second;

        if (!keys.insert(key).second) {
            for (int n = 2;; ++n) {
                auto k = fmt("%s_%d", key, n);
                if (keys.insert(k).second) {
                    key = k;
                    break;
                }
            }
        }

        nodeKeys.insert_or_assign(node, key);

        auto n = nlohmann::json::object();

        if (!node->inputs.empty()) {
            auto inputs = nlohmann::json::object();
            for (auto & i : node->inputs) {
                if (auto child = std::get_if<0>(&i.second)) {
                    inputs[i.first] = dumpNode(i.first, *child);
                } else if (auto follows = std::get_if<1>(&i.second)) {
                    auto arr = nlohmann::json::array();
                    for (auto & x : *follows)
                        arr.push_back(x);
                    inputs[i.first] = std::move(arr);
                }
            }
            n["inputs"] = std::move(inputs);
        }

        if (auto lockedNode = node.dynamic_pointer_cast<const LockedNode>()) {
            n["original"] = fetchers::attrsToJSON(lockedNode->originalRef.toAttrs());
            n["locked"] = fetchers::attrsToJSON(lockedNode->lockedRef.toAttrs());
            assert(lockedNode->lockedRef.input.isFinal() || lockedNode->lockedRef.input.isRelative());
            if (!lockedNode->isFlake)
                n["flake"] = false;
            if (lockedNode->buildTime)
                n["buildTime"] = true;
            if (lockedNode->parentInputAttrPath)
                n["parent"] = *lockedNode->parentInputAttrPath;
        }

        nodes[key] = std::move(n);

        return key;
    };

    nlohmann::json json;
    json["version"] = 7;
    json["root"] = dumpNode("root", root);
    json["nodes"] = std::move(nodes);

    return {json, std::move(nodeKeys)};
}

std::pair<std::string, LockFileV7::KeyMap> LockFileV7::to_string() const
{
    auto [json, nodeKeys] = toJSON();
    return {json.dump(2), std::move(nodeKeys)};
}

std::optional<FlakeRef> LockFileV7::isUnlocked(const fetchers::Settings & fetchSettings) const
{
    std::set<ref<const Node>> nodes;

    [&](this const auto & visit, ref<const Node> node) {
        if (!nodes.insert(node).second)
            return;
        for (auto & i : node->inputs)
            if (auto child = std::get_if<0>(&i.second))
                visit(*child);
    }(root);

    /* Return whether the input is either locked, or, if
       `allow-dirty-locks` is enabled, it has a NAR hash. In the
       latter case, we can verify the input but we may not be able to
       fetch it from anywhere. */
    auto isConsideredLocked = [&](const fetchers::Input & input) {
        return input.isLocked(fetchSettings) || (fetchSettings.allowDirtyLocks && input.getNarHash());
    };

    for (auto & i : nodes) {
        if (i == ref<const Node>(root))
            continue;
        auto node = i.dynamic_pointer_cast<const LockedNode>();
        if (node && (!isConsideredLocked(node->lockedRef.input) || !node->lockedRef.input.isFinal())
            && !node->lockedRef.input.isRelative())
            return node->lockedRef;
    }

    return {};
}

bool LockFileV7::operator==(const LockFileV7 & other) const
{
    // FIXME: slow
    return toJSON().first == other.toJSON().first;
}

std::map<InputAttrPath, Node::Edge> LockFileV7::getAllInputs() const
{
    std::set<ref<Node>> done;
    std::map<InputAttrPath, Node::Edge> res;

    [&](this const auto & recurse, const InputAttrPath & prefix, ref<Node> node) {
        if (!done.insert(node).second)
            return;

        for (auto & [id, input] : node->inputs) {
            auto inputAttrPath(prefix);
            inputAttrPath.push_back(id);
            res.emplace(inputAttrPath, input);
            if (auto child = std::get_if<0>(&input))
                recurse(inputAttrPath, *child);
        }
    }({}, root);

    return res;
}

static std::string describe(const FlakeRef & flakeRef)
{
    auto s = fmt("'%s'", flakeRef.to_string(true));

    if (auto lastModified = flakeRef.input.getLastModified())
        s += fmt(" (%s)", std::put_time(std::gmtime(&*lastModified), "%Y-%m-%d"));

    return s;
}

std::ostream & operator<<(std::ostream & stream, const Node::Edge & edge)
{
    if (auto node = std::get_if<0>(&edge))
        stream << describe((*node)->lockedRef);
    else if (auto follows = std::get_if<1>(&edge))
        stream << fmt("follows '%s'", printInputAttrPath(*follows));
    return stream;
}

static bool equals(const Node::Edge & e1, const Node::Edge & e2)
{
    if (auto n1 = std::get_if<0>(&e1))
        if (auto n2 = std::get_if<0>(&e2))
            return (*n1)->lockedRef == (*n2)->lockedRef;
    if (auto f1 = std::get_if<1>(&e1))
        if (auto f2 = std::get_if<1>(&e2))
            return *f1 == *f2;
    return false;
}

std::string LockedFlakeV7::diff(const LockedFlake & _oldLockFile) const
{
    /* If `oldLockFile` is not a version 7 lock file, diff against an
       empty lock file, i.e. all inputs of this lock file will show up
       as added. */
    auto oldLockFile = dynamic_cast<const LockedFlakeV7 *>(&_oldLockFile);

    auto oldFlat = oldLockFile ? oldLockFile->lockFile.getAllInputs() : std::map<InputAttrPath, Node::Edge>();
    auto newFlat = lockFile.getAllInputs();

    auto i = oldFlat.begin();
    auto j = newFlat.begin();
    std::string res;

    while (i != oldFlat.end() || j != newFlat.end()) {
        if (j != newFlat.end() && (i == oldFlat.end() || i->first > j->first)) {
            res += fmt(
                "• " ANSI_GREEN "Added input '%s':" ANSI_NORMAL "\n    %s\n", printInputAttrPath(j->first), j->second);
            ++j;
        } else if (i != oldFlat.end() && (j == newFlat.end() || i->first < j->first)) {
            res += fmt("• " ANSI_RED "Removed input '%s'" ANSI_NORMAL "\n", printInputAttrPath(i->first));
            ++i;
        } else {
            if (!equals(i->second, j->second)) {
                res +=
                    fmt("• " ANSI_BOLD "Updated input '%s':" ANSI_NORMAL "\n    %s\n  → %s\n",
                        printInputAttrPath(i->first),
                        i->second,
                        j->second);
            }
            ++i;
            ++j;
        }
    }

    return res;
}

void LockFileV7::check()
{
    auto inputs = getAllInputs();

    for (auto & [inputAttrPath, input] : inputs) {
        if (auto follows = std::get_if<1>(&input)) {
            if (!follows->empty() && !findInput(*follows))
                throw Error(
                    "input '%s' follows a non-existent input '%s'",
                    printInputAttrPath(inputAttrPath),
                    printInputAttrPath(*follows));
        }
    }
}

std::vector<FlakeId> LockedFlakeV7::getInputNames(const InputAttrPath & prefix) const
{
    std::vector<FlakeId> res;
    if (auto node = lockFile.findInput(prefix))
        for (auto & [id, input] : node->inputs)
            res.push_back(id);
    return res;
}

std::optional<LockedFlake::InputInfo> LockedFlakeV7::findInput(const InputAttrPath & path) const
{
    if (auto node = std::dynamic_pointer_cast<const LockedNode>(lockFile.findInput(path)))
        return InputInfo{
            .lockedRef = node->lockedRef,
            .isFlake = node->isFlake,
        };
    return std::nullopt;
}

SourcePath LockedFlakeV7::getSourcePath(EvalState & state, const InputAttrPath & inputAttrPath) const
{
    /* The root node. */
    if (inputAttrPath.empty())
        return flake.path.parent();

    auto node = lockFile.findInput(inputAttrPath);
    if (!node)
        throw Error("flake input '%s' does not exist", printInputAttrPath(inputAttrPath));

    auto lockedNode = std::dynamic_pointer_cast<LockedNode>(node);
    assert(lockedNode);

    {
        auto sourcePath(lockedNode->sourcePath.lock());
        if (*sourcePath)
            return **sourcePath;
    }

    /* Note: we fetch without holding the `sourcePath` lock, so
       concurrent calls don't get serialized. Racing fetches of the
       same node are harmless since they produce the same path. */
    auto path = [&]() -> SourcePath {
        if (auto relativePath = lockedNode->lockedRef.input.isRelative()) {
            /* Resolve relative path inputs against the source path of
               their parent flake. */
            auto parentPath = getSourcePath(state, lockedNode->parentInputAttrPath.value());
            return {parentPath.accessor, CanonPath(relativePath->string(), parentPath.path)};
        } else {
            /* Note: `lockedRef` is a copy since `mountInput()` may
               modify the input (e.g. adding a `narHash` attribute). */
            auto lockedRef = lockedNode->lockedRef;
            auto accessor =
                state.inputCache
                    ->getAccessor(state.fetchSettings, *state.store, lockedRef.input, fetchers::UseRegistries::No)
                    .accessor;
            return state.storePath(state.mountInput(lockedRef.input, lockedNode->lockedRef.input, accessor, true, true))
                   / CanonPath(lockedRef.subdir);
        }
    }();

    *lockedNode->sourcePath.lock() = path;

    return path;
}

void LockedFlakeV7::visit(VisitCallback callback) const
{
    if (!callback({}, InputInfo{.lockedRef = flake.lockedRef}))
        return;

    [&](this const auto & recurse, const InputAttrPath & prefix, ref<Node> node) -> void {
        for (auto & [id, input] : node->inputs) {
            auto inputAttrPath(prefix);
            inputAttrPath.push_back(id);
            if (auto child = std::get_if<0>(&input)) {
                if (callback(
                        inputAttrPath,
                        InputInfo{
                            .lockedRef = (*child)->lockedRef,
                            .isFlake = (*child)->isFlake,
                            .buildTime = (*child)->buildTime,
                        }))
                    recurse(inputAttrPath, *child);
            } else if (auto follows = std::get_if<1>(&input)) {
                callback(inputAttrPath, *follows);
            }
        }
    }({}, lockFile.root);
}

std::optional<FlakeRef> LockedFlakeV7::isUnlocked(const fetchers::Settings & fetchSettings) const
{
    return lockFile.isUnlocked(fetchSettings);
}

nlohmann::json LockedFlakeV7::toJSON() const
{
    return lockFile.toJSON().first;
}

LockedFlakeV7::LockedFlakeV7(
    const fetchers::Settings & fetchSettings, Flake flake, const nlohmann::json & json, std::string_view path)
    : LockedFlake(std::move(flake))
    , lockFile(json.is_null() ? LockFileV7() : LockFileV7(fetchSettings, json, path))
{
}

static LockFileV7 readLockFile(const fetchers::Settings & fetchSettings, const SourcePath & lockFilePath)
{
    if (!lockFilePath.pathExists())
        return LockFileV7();

    auto json = [&] {
        try {
            return nlohmann::json::parse(lockFilePath.readFile());
        } catch (const nlohmann::json::parse_error & e) {
            throw Error("Could not parse '%s': %s", lockFilePath, e.what());
        }
    }();

    return LockFileV7(fetchSettings, json, fmt("%s", lockFilePath));
}

std::unique_ptr<LockedFlake> LockedFlakeV7::lockFlake(
    const Settings & settings,
    EvalState & state,
    const LockFlags & lockFlags,
    Flake flake,
    const LockedFlake & _oldLockFile)
{
    auto & oldLockFile = dynamic_cast<const LockedFlakeV7 &>(_oldLockFile).lockFile;

    auto useRegistries = lockFlags.useRegistries.value_or(settings.useRegistries);
    auto useRegistriesInputs = useRegistries ? fetchers::UseRegistries::Limited : fetchers::UseRegistries::No;

    struct OverrideTarget
    {
        FlakeInput input;
        SourcePath sourcePath;
        std::optional<InputAttrPath> parentInputAttrPath; // FIXME: rename to inputAttrPathPrefix?
    };

    std::map<NonEmptyInputAttrPath, OverrideTarget> overrides;
    std::set<NonEmptyInputAttrPath> explicitCliOverrides;
    std::set<NonEmptyInputAttrPath> overridesUsed;
    std::set<InputAttrPath> updatesUsed;

    for (auto & i : lockFlags.inputOverrides) {
        overrides.emplace(
            i.first,
            OverrideTarget{
                .input = FlakeInput{.ref = i.second},
                /* Note: any relative overrides
                   (e.g. `--override-input B/C "path:./foo/bar"`)
                   are interpreted relative to the top-level
                   flake. */
                .sourcePath = flake.path,
            });
        explicitCliOverrides.insert(i.first);
    }

    LockFileV7 newLockFile;

    std::vector<FlakeRef> parents;

    std::function<void(
        const FlakeInputs & flakeInputs,
        ref<Node> node,
        const InputAttrPath & inputAttrPathPrefix,
        std::shared_ptr<const Node> oldNode,
        const InputAttrPath & followsPrefix,
        const SourcePath & sourcePath,
        bool trustLock)>
        computeLocks;

    computeLocks = [&](
                       /* The inputs of this node, either from flake.nix or
                          flake.lock. */
                       const FlakeInputs & flakeInputs,
                       /* The node whose locks are to be updated.*/
                       ref<Node> node,
                       /* The path to this node in the lock file graph. */
                       const InputAttrPath & inputAttrPathPrefix,
                       /* The old node, if any, from which locks can be
                          copied. */
                       std::shared_ptr<const Node> oldNode,
                       /* The prefix relative to which 'follows' should be
                          interpreted. When a node is initially locked, it's
                          relative to the node's flake; when it's already locked,
                          it's relative to the root of the lock file. */
                       const InputAttrPath & followsPrefix,
                       /* The source path of this node's flake. */
                       const SourcePath & sourcePath,
                       bool trustLock) {
        debug("computing lock file node '%s'", printInputAttrPath(inputAttrPathPrefix));

        /* Get the overrides (i.e. attributes of the form
           'inputs.nixops.inputs.nixpkgs.url = ...'). */
        auto addOverrides =
            [&](this const auto & addOverrides, const FlakeInput & input, const InputAttrPath & prefix) -> void {
            for (auto & [idOverride, inputOverride] : input.overrides) {
                auto inputAttrPath = NonEmptyInputAttrPath::append(prefix, idOverride);
                if (inputOverride.ref || inputOverride.follows)
                    overrides.emplace(
                        inputAttrPath,
                        OverrideTarget{
                            .input = inputOverride,
                            .sourcePath = sourcePath,
                            .parentInputAttrPath = inputAttrPathPrefix});
                addOverrides(inputOverride, inputAttrPath);
            }
        };

        for (auto & [id, input] : flakeInputs) {
            auto inputAttrPath(inputAttrPathPrefix);
            inputAttrPath.push_back(id);
            addOverrides(input, inputAttrPath);
        }

        /* Check whether this input has overrides for a
           non-existent input. */
        for (auto [inputAttrPath, inputOverride] : overrides) {
            auto follow = inputAttrPath.inputName();
            auto inputAttrPath2 = inputAttrPath.parent();
            if (inputAttrPath2 == inputAttrPathPrefix && !flakeInputs.count(follow))
                warn(
                    "input '%s' has an override for a non-existent input '%s'",
                    printInputAttrPath(inputAttrPathPrefix),
                    follow);
        }

        /* Go over the flake inputs, resolve/fetch them if
           necessary (i.e. if they're new or the flakeref changed
           from what's in the lock file). */
        for (auto & [id, input2] : flakeInputs) {
            auto nonEmptyInputAttrPath = NonEmptyInputAttrPath::append(inputAttrPathPrefix, id);
            auto inputAttrPath = nonEmptyInputAttrPath.get();
            auto inputAttrPathS = printInputAttrPath(inputAttrPath);
            debug("computing input '%s'", inputAttrPathS);

            try {

                /* Do we have an override for this input from one of the
                   ancestors? */
                auto i = overrides.find(nonEmptyInputAttrPath);
                bool hasOverride = i != overrides.end();
                bool hasCliOverride = explicitCliOverrides.contains(nonEmptyInputAttrPath);
                if (hasOverride)
                    overridesUsed.insert(nonEmptyInputAttrPath);
                auto input = hasOverride ? i->second.input : input2;

                /* Resolve relative 'path:' inputs relative to
                   the source path of the overrider. */
                auto overriddenSourcePath = hasOverride ? i->second.sourcePath : sourcePath;

                /* Respect the "flakeness" of the input even if we
                   override it. */
                if (hasOverride)
                    input.isFlake = input2.isFlake;

                /* Resolve 'follows' later (since it may refer to an input
                   path we haven't processed yet. */
                if (input.follows) {
                    InputAttrPath target;

                    target.insert(target.end(), input.follows->begin(), input.follows->end());

                    debug("input '%s' follows '%s'", inputAttrPathS, printInputAttrPath(target));
                    node->inputs.insert_or_assign(id, target);
                    continue;
                }

                if (!input.ref)
                    input.ref =
                        FlakeRef::fromAttrs(state.fetchSettings, {{"type", "indirect"}, {"id", std::string(id)}});

                auto overriddenParentPath = input.ref->input.isRelative()
                                                ? std::optional<InputAttrPath>(
                                                      hasOverride ? i->second.parentInputAttrPath : inputAttrPathPrefix)
                                                : std::nullopt;

                auto resolveRelativePath = [&]() -> std::optional<SourcePath> {
                    if (auto relativePath = input.ref->input.isRelative()) {
                        return SourcePath{
                            overriddenSourcePath.accessor,
                            CanonPath(relativePath->string(), overriddenSourcePath.path.parent().value())};
                    } else
                        return std::nullopt;
                };

                /* Get the input flake, resolve 'path:./...'
                   flakerefs relative to the parent flake. */
                auto getInputFlake = [&](const FlakeRef & ref, const fetchers::UseRegistries useRegistries) {
                    if (auto resolvedPath = resolveRelativePath()) {
                        return readFlake(state, ref, ref, ref, *resolvedPath, inputAttrPath);
                    } else {
                        return getFlake(state, ref, useRegistriesInputs, inputAttrPath, true);
                    }
                };

                /* Do we have an entry in the existing lock file?
                   And the input is not in updateInputs? */
                std::shared_ptr<LockedNode> oldLock;

                updatesUsed.insert(inputAttrPath);

                if (oldNode && !lockFlags.inputUpdates.count(nonEmptyInputAttrPath))
                    if (auto oldLock2 = get(oldNode->inputs, id))
                        if (auto oldLock3 = std::get_if<0>(&*oldLock2))
                            oldLock = *oldLock3;

                if (oldLock && oldLock->originalRef.canonicalize() == input.ref->canonicalize()
                    && oldLock->parentInputAttrPath == overriddenParentPath && !hasCliOverride) {
                    debug("keeping existing input '%s'", inputAttrPathS);

                    /* Copy the input from the old lock since its flakeref
                       didn't change and there is no override from a
                       higher level flake. */
                    auto childNode = make_ref<LockedNode>(
                        oldLock->lockedRef,
                        oldLock->originalRef,
                        oldLock->isFlake,
                        oldLock->buildTime,
                        oldLock->parentInputAttrPath);

                    node->inputs.insert_or_assign(id, childNode);

                    /* If we have this input in updateInputs, then we
                       must fetch the flake to update it. */
                    auto lb = lockFlags.inputUpdates.lower_bound(nonEmptyInputAttrPath);

                    auto mustRefetch = lb != lockFlags.inputUpdates.end() && lb->get().size() > inputAttrPath.size()
                                       && std::equal(inputAttrPath.begin(), inputAttrPath.end(), lb->get().begin());

                    FlakeInputs fakeInputs;

                    if (!mustRefetch) {
                        /* No need to fetch this flake, we can be
                           lazy. However there may be new overrides on the
                           inputs of this flake, so we need to check
                           those. */
                        for (auto & i : oldLock->inputs) {
                            if (auto lockedNode = std::get_if<0>(&i.second)) {
                                fakeInputs.emplace(
                                    i.first,
                                    FlakeInput{
                                        .ref = (*lockedNode)->originalRef,
                                        .isFlake = (*lockedNode)->isFlake,
                                    });
                            } else if (auto follows = std::get_if<1>(&i.second)) {
                                if (!trustLock) {
                                    // It is possible that the flake has changed,
                                    // so we must confirm all the follows that are in the lock file are also in the
                                    // flake.
                                    auto overridePath = NonEmptyInputAttrPath::append(nonEmptyInputAttrPath, i.first);
                                    auto o = overrides.find(overridePath);
                                    // If the override disappeared, we have to refetch the flake,
                                    // since some of the inputs may not be present in the lock file.
                                    if (o == overrides.end()) {
                                        mustRefetch = true;
                                        // There's no point populating the rest of the fake inputs,
                                        // since we'll refetch the flake anyways.
                                        break;
                                    }
                                }
                                auto absoluteFollows(followsPrefix);
                                absoluteFollows.insert(absoluteFollows.end(), follows->begin(), follows->end());
                                fakeInputs.emplace(
                                    i.first,
                                    FlakeInput{
                                        .follows = absoluteFollows,
                                    });
                            }
                        }
                    }

                    if (mustRefetch) {
                        auto inputFlake = getInputFlake(oldLock->lockedRef, useRegistriesInputs);
                        *childNode->sourcePath.lock() = inputFlake.path.parent();
                        computeLocks(
                            inputFlake.inputs,
                            childNode,
                            inputAttrPath,
                            oldLock,
                            followsPrefix,
                            inputFlake.path,
                            false);
                    } else {
                        computeLocks(fakeInputs, childNode, inputAttrPath, oldLock, followsPrefix, sourcePath, true);
                    }

                } else {
                    /* We need to create a new lock file entry. So fetch
                       this input. */
                    debug("creating new input '%s'", inputAttrPathS);

                    if (!lockFlags.allowUnlocked && !input.ref->input.isLocked(state.fetchSettings)
                        && !input.ref->input.isRelative())
                        throw Error("cannot update unlocked flake input '%s' in pure mode", inputAttrPathS);

                    /* Note: in case of an --override-input, we use
                        the *original* ref (input2.ref) for the
                        "original" field, rather than the
                        override. This ensures that the override isn't
                        nuked the next time we update the lock
                        file. That is, overrides are sticky unless you
                        use --no-write-lock-file. */
                    auto inputIsOverride = explicitCliOverrides.contains(nonEmptyInputAttrPath);
                    auto ref = (input2.ref && inputIsOverride) ? *input2.ref : *input.ref;

                    /* Warn against the use of indirect flakerefs
                       (but only at top-level since we don't want
                       to annoy users about flakes that are not
                       under their control). */
                    auto warnRegistry = [&](const FlakeRef & resolvedRef) {
                        if (inputAttrPath.size() == 1 && !input.ref->input.isDirect()) {
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
                                inputAttrPathS,
                                flake.path,
                                s.str());
                        }
                    };

                    if (input.isFlake) {
                        auto inputFlake = getInputFlake(
                            *input.ref, inputIsOverride ? fetchers::UseRegistries::All : useRegistriesInputs);

                        auto childNode = make_ref<LockedNode>(
                            inputFlake.lockedRef, ref, true, input.buildTime, overriddenParentPath);

                        node->inputs.insert_or_assign(id, childNode);

                        /* Guard against circular flake imports. */
                        for (auto & parent : parents)
                            if (parent == *input.ref)
                                throw Error("found circular import of flake '%s'", parent);
                        parents.push_back(*input.ref);
                        Finally cleanup([&]() { parents.pop_back(); });

                        /* Recursively process the inputs of this
                           flake, using its own lock file. */
                        *childNode->sourcePath.lock() = inputFlake.path.parent();
                        computeLocks(
                            inputFlake.inputs,
                            childNode,
                            inputAttrPath,
                            readLockFile(state.fetchSettings, inputFlake.lockFilePath()).root.get_ptr(),
                            inputAttrPath,
                            inputFlake.path,
                            false);

                        warnRegistry(inputFlake.resolvedRef);
                    }

                    else {
                        auto [path, lockedRef] = [&]() -> std::tuple<SourcePath, FlakeRef> {
                            // Handle non-flake 'path:./...' inputs.
                            if (auto resolvedPath = resolveRelativePath()) {
                                return {*resolvedPath, *input.ref};
                            } else {
                                auto cachedInput = state.inputCache->getAccessor(
                                    state.fetchSettings, *state.store, input.ref->input, useRegistriesInputs);

                                auto resolvedRef = FlakeRef(std::move(cachedInput.resolvedInput), input.ref->subdir);
                                auto lockedRef = FlakeRef(std::move(cachedInput.lockedInput), input.ref->subdir);

                                warnRegistry(resolvedRef);

                                return {
                                    state.storePath(state.mountInput(
                                        lockedRef.input, input.ref->input, cachedInput.accessor, true, true)),
                                    lockedRef};
                            }
                        }();

                        auto childNode =
                            make_ref<LockedNode>(lockedRef, ref, false, input.buildTime, overriddenParentPath);

                        *childNode->sourcePath.lock() = path;

                        node->inputs.insert_or_assign(id, childNode);
                    }
                }

            } catch (Error & e) {
                e.addTrace({}, "while updating the flake input '%s'", inputAttrPathS);
                throw;
            }
        }
    };

    computeLocks(
        flake.inputs,
        newLockFile.root,
        {},
        lockFlags.recreateLockFile ? nullptr : oldLockFile.root.get_ptr(),
        {},
        flake.path,
        false);

    for (auto & i : lockFlags.inputOverrides)
        if (!overridesUsed.count(i.first))
            warn("the flag '--override-input %s %s' does not match any input", printInputAttrPath(i.first), i.second);

    for (auto & i : lockFlags.inputUpdates)
        if (!updatesUsed.count(i))
            warn("'%s' does not match any input of this flake", printInputAttrPath(i));

    /* Check 'follows' inputs. */
    newLockFile.check();

    return std::make_unique<LockedFlakeV7>(std::move(flake), std::move(newLockFile));
}

} // namespace nix::flake
