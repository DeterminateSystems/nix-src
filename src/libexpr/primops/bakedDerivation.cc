#include "nix/expr/primops.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/store/derivations.hh"
#include "nix/store/derived-path.hh"
#include "nix/store/store-api.hh"
#include "nix/store/globals.hh"

#include <boost/unordered/concurrent_flat_map.hpp>

namespace nix {

static void prim_bakedDerivation(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceAttrs(*args[0], pos, "while evaluating the argument passed to builtins.bakedDerivation");

    std::optional<std::string> name;

    /* TODO: add a `narHash` attribute to assert the known contents of a pre-built output. */
    std::map<std::string, StorePath> outputs;

    for (auto & attr : *args[0]->attrs()) {
        std::string_view attrName = state.symbols[attr.name];
        auto attrHint = fmt("while evaluating the attribute '%s' passed to builtins.bakedDerivation", attrName);

        if (attrName == "name") {
            name = state.forceStringNoCtx(*attr.value, attr.pos, attrHint);
        }

        else if (attrName == "outputs") {
            state.forceAttrs(*attr.value, attr.pos, attrHint);
            for (auto & outAttr : *attr.value->attrs()) {
                std::string_view outName = state.symbols[outAttr.name];

                state.forceAttrs(
                    *outAttr.value,
                    outAttr.pos,
                    fmt("while evaluating the output '%s' passed to builtins.bakedDerivation", outName));

                std::optional<StorePath> path;

                for (auto & outField : *outAttr.value->attrs()) {
                    std::string_view fieldName = state.symbols[outField.name];
                    auto fieldHint =
                        fmt("while evaluating the attribute '%s' of output '%s' passed to builtins.bakedDerivation",
                            fieldName,
                            outName);

                    if (fieldName == "path") {
                        auto s = state.forceStringNoCtx(*outField.value, outField.pos, fieldHint);
                        path = state.store->parseStorePath(s);
                    }

                    else
                        state
                            .error<EvalError>(
                                "attribute '%s' isn't supported in an output passed to 'builtins.bakedDerivation'",
                                fieldName)
                            .atPos(outField.pos)
                            .debugThrow();
                }

                if (!path)
                    state
                        .error<EvalError>(
                            "attribute 'path' is missing in output '%s' passed to 'builtins.bakedDerivation'", outName)
                        .atPos(outAttr.pos)
                        .debugThrow();

                outputs.emplace(std::string(outName), std::move(*path));
            }
        }

        else
            state.error<EvalError>("attribute '%s' isn't supported in call to 'builtins.bakedDerivation'", attrName)
                .atPos(attr.pos)
                .debugThrow();
    }

    if (!name)
        state.error<EvalError>("attribute 'name' is missing in call to 'builtins.bakedDerivation'")
            .atPos(pos)
            .debugThrow();

    if (outputs.empty())
        state.error<EvalError>("attribute 'outputs' is missing or empty in call to 'builtins.bakedDerivation'")
            .atPos(pos)
            .debugThrow();

    Derivation drv;
    drv.name = *name;
    drv.platform = "builtin";
    drv.builder = "builtin:substitute";

    for (auto & [outName, outPath] : outputs)
        drv.outputs.insert_or_assign(outName, DerivationOutput::InputAddressed{.path = outPath});

    /* As in `derivationStrict`, don't write the derivation in read-only mode. */
    auto drvPath = settings.readOnlyMode ? computeStorePath(*state.store, drv)
                                         : state.store->writeDerivation(*state.asyncPathWriter, drv, state.repair);

    /* As in `derivationStrict`, cache the derivation hash so that derivations depending on this one don't need to
       read it back from the store. */
    drvHashes.insert_or_assign(drvPath, hashDerivationModulo(*state.store, drv, false));

    /* Return an attribute set of the same shape as `derivationStrict`: the derivation path and one string per
       output. The output strings carry string context, so that they can be used as inputs of other derivations. */
    auto result = state.buildBindings(1 + drv.outputs.size());
    result.alloc(state.s.drvPath)
        .mkString(
            state.store->printStorePath(drvPath),
            {
                NixStringContextElem::DrvDeep{.drvPath = drvPath},
            },
            state.mem);
    for (auto & [outName, outPath] : outputs)
        state.mkOutputString(
            result.alloc(outName),
            SingleDerivedPath::Built{
                .drvPath = makeConstantStorePathRef(drvPath),
                .output = outName,
            },
            outPath);

    v.mkAttrs(result);
}

static RegisterPrimOp primop_bakedDerivation({
    .name = "__bakedDerivation",
    .args = {"attrs"},
    .doc = R"(
      Create a *baked derivation*: a derivation that is never built, but
      whose outputs are obtained by substitution from a binary cache.
      This is what [`nix flake bake`](@docroot@/command-ref/new-cli/nix3-flake-bake.md)
      uses to represent the pre-evaluated outputs of a flake.

      The argument is an attribute set with the following attributes:

      - `name`: The name of the derivation.

      - `outputs`: An attribute set mapping output names to attribute
        sets with a single attribute `path`, the store path of that
        output.

      The result has the same shape as the result of
      `builtins.derivationStrict`: an attribute set containing `drvPath`
      (the store path of the derivation) and one attribute per output
      containing that output's store path. These strings carry
      [string context](@docroot@/language/string-context.md), so the
      outputs of a baked derivation can be used as inputs of other
      derivations.

      The resulting derivation uses the builder `builtin:substitute`
      and has no inputs. Realising it does not run anything; instead,
      Nix substitutes the given output paths. If they cannot be
      substituted, the build fails.

      Example:

      ```nix
      builtins.bakedDerivation {
        name = "hello-2.12.1";
        outputs = {
          out.path = "/nix/store/1q8w6grhdj6pn0cvlw18cw07nvjb7pj5-hello-2.12.1";
        };
      }
      ```

      evaluates to

      ```nix
      {
        drvPath = "/nix/store/1jczli5n8zgxl2vgfsc5vy3z0f5lfn3q-hello-2.12.1.drv";
        out = "/nix/store/1q8w6grhdj6pn0cvlw18cw07nvjb7pj5-hello-2.12.1";
      }
      ```
    )",
    .impl = prim_bakedDerivation,
    .experimentalFeature = Xp::BakedDerivations,
});

} // namespace nix
