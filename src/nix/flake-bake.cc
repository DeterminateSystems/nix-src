#include "flake-command.hh"
#include "nix/cmd/flake-schemas.hh"
#include "nix/expr/parallel-eval.hh"
#include "nix/store/derivations.hh"

using namespace nix;
using namespace nix::flake;

struct CmdFlakeBake : FlakeCommand, MixFlakeSchemas
{
    std::string description() override
    {
        return "bake a flake";
    }

    std::string doc() override
    {
        return
#include "flake-bake.md"
            ;
    }

    void run(nix::ref<nix::Store> store) override
    {
        auto state = getEvalState();
        auto evalStore = getEvalStore();
        auto flake = make_ref<LockedFlake>(lockFlake());

        auto cache = flake_schemas::call(*state, flake, getDefaultFlakeSchemas());

        auto inventory = cache->getRoot()->getAttr("inventory");
        auto outputs = cache->getRoot()->getAttr("outputs");

        FutureVector futures(*state->executor);

        // FIXME: generate an AST.
        Sync<std::string> result;

        *result.lock() = R""(
{
  outputs =
    { self }:
    {
)"";

        auto visit = [&](this auto & visit, ref<eval_cache::AttrCursor> node) -> void {
            flake_schemas::visit(
                {"x86_64-linux"}, // FIXME
                true,
                node,
                flake->flake.provenance,

                [&](const flake_schemas::Leaf & leaf) {
                    printError("AT %s", leaf.node->getAttrPathStr());

                    auto drv = leaf.derivation(outputs);
                    if (!drv)
                        // FIXME: emit attrs that throw a useful error?
                        return;

                    // FIXME: do we need to force the derivation?
                    auto drvPath = drv->forceDerivation();
                    auto storeDrv = evalStore->derivationFromPath(drvPath);
                    std::map<std::string, StorePath> outputs;
                    for (auto & i : storeDrv.outputsAndOptPaths(*store)) {
                        if (auto outPath = i.second.second)
                            outputs.emplace(i.first, *outPath);
                        else
                            // FIXME: we could build the derivation to get the outputs.
                            return;
                    }

                    auto name = drv->getAttr(state->s.name)->getString();

                    auto r(result.lock());
                    *r += "      " + leaf.node->getAttrPathStr() + " = {\n";
                    *r += "        drvPath = builtins.fakeDerivation {\n";
                    *r += "          name = \"" + name + "\";\n";
                    for (auto & i : outputs)
                        *r += "          outputs." + i.first + ".path = \"" + store->printStorePath(i.second) + "\";\n";
                    *r += "        };\n";
                    *r += "        type = \"derivation\";\n";
                    *r += "        name = \"" + name + "\";\n";
                    *r += "        system = \"x86_64-linux\";\n"; // FIXME
                    // meta.description = "Docker image with Nix for x86_64-linux";
                    *r += "      };\n\n";
                },

                [&](std::function<void(flake_schemas::ForEachChild)> forEachChild) {
                    forEachChild([&](Symbol attrName, ref<eval_cache::AttrCursor> node, bool isLast) {
                        state->spawn(futures, 1, [&visit, node]() {
                            try {
                                visit(node);
                            } catch (EvalError & e) {
                                // FIXME: make it a flake schema attribute whether to ignore evaluation errors.
                                if (node->root->state.symbols[node->getAttrPath()[0]] != "legacyPackages")
                                    throw;
                            }
                        });
                    });
                },

                [&](ref<eval_cache::AttrCursor> node, const std::vector<std::string> & systems) {},

                [&](ref<eval_cache::AttrCursor> node) {});
        };

        flake_schemas::forEachOutput(
            inventory,
            [&](Symbol outputName,
                std::shared_ptr<eval_cache::AttrCursor> output,
                const std::string & doc,
                bool isLast) {
                if (output)
                    state->spawn(futures, 1, [&visit, output]() { visit(ref(output)); });
            });

        futures.finishAll();

        *result.lock() += R""(
    };
}
)"";

        logger->cout(*result.lock());
    }
};

static auto rCmdFlakeBake = registerCommand2<CmdFlakeBake>({"flake", "bake"});
