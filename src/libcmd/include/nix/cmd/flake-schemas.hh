#pragma once

#include "nix/expr/eval-cache.hh"
#include "nix/flake/flake.hh"
#include "nix/cmd/command.hh"

namespace nix::flake_schemas {

using namespace eval_cache;

ref<eval_cache::EvalCache>
call(EvalState & state, std::shared_ptr<flake::LockedFlake> lockedFlake, std::optional<FlakeRef> defaultSchemasFlake);

void forEachOutput(
    ref<AttrCursor> inventory,
    std::function<void(Symbol outputName, std::shared_ptr<AttrCursor> output, const std::string & doc, bool isLast)> f);

struct Node
{
    const ref<AttrCursor> node;

    Node(const ref<AttrCursor> & node)
        : node(node)
    {
    }

    /**
     * Return the `forSystems` attribute. This can be null, which
     * means "all systems".
     */
    std::optional<std::vector<std::string>> forSystems() const;
};

struct Leaf : Node
{
    using Node::Node;

    std::optional<std::string> what() const;

    std::optional<std::string> shortDescription() const;

    std::shared_ptr<AttrCursor> derivation() const;
};

typedef std::function<void(Symbol attrName, ref<AttrCursor> attr, bool isLast)> ForEachChild;

void visit(
    std::optional<std::string> system,
    ref<AttrCursor> node,
    std::function<void(const Leaf & leaf)> visitLeaf,
    std::function<void(std::function<void(ForEachChild)>)> visitNonLeaf,
    std::function<void(ref<AttrCursor> node, const std::vector<std::string> & systems)> visitFiltered);

struct OutputInfo
{
    ref<AttrCursor> schemaInfo;
    ref<AttrCursor> nodeInfo;
    eval_cache::AttrPath leafAttrPath;
};

std::optional<OutputInfo> getOutput(ref<AttrCursor> inventory, eval_cache::AttrPath attrPath);

struct SchemaInfo
{
    std::string doc;
    StringSet roles;
    bool appendSystem = false;
    std::optional<eval_cache::AttrPath> defaultAttrPath;
};

using Schemas = std::map<std::string, SchemaInfo>;

Schemas getSchema(ref<AttrCursor> root);

} // namespace nix::flake_schemas
