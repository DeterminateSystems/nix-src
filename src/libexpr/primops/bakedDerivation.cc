#include "nix/expr/primops.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/store/derivations.hh"
#include "nix/store/derived-path.hh"
#include "nix/store/store-api.hh"
#include "nix/util/hash.hh"

#include <boost/unordered/concurrent_flat_map.hpp>

namespace nix {

static void prim_bakedDerivation(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceAttrs(*args[0], pos, "while evaluating the argument passed to builtins.bakedDerivation");

    std::optional<std::string> name;

    struct BakedDerivationOutput
    {
        StorePath path;
        std::optional<Hash> narHash;
    };

    std::map<std::string, BakedDerivationOutput> outputs;

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
                std::optional<Hash> narHash;

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

                    else if (fieldName == "narHash") {
                        auto s = state.forceStringNoCtx(*outField.value, outField.pos, fieldHint);
                        narHash = Hash::parseAny(s, std::nullopt);
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

                outputs.emplace(std::string(outName), BakedDerivationOutput{std::move(*path), std::move(narHash)});
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

    for (auto & [outName, out] : outputs) {
        if (out.narHash)
            drv.outputs.insert_or_assign(
                outName,
                DerivationOutput::CAFixed{
                    .ca =
                        ContentAddress{
                            .method = ContentAddressMethod::Raw::NixArchive,
                            .hash = *out.narHash,
                        },
                });
        else
            drv.outputs.insert_or_assign(outName, DerivationOutput::InputAddressed{.path = out.path});
    }

    auto drvPath = state.store->writeDerivation(*state.asyncPathWriter, drv, state.repair);

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
    for (auto & [outName, out] : outputs)
        state.mkOutputString(
            result.alloc(outName),
            SingleDerivedPath::Built{
                .drvPath = makeConstantStorePathRef(drvPath),
                .output = outName,
            },
            out.path);

    v.mkAttrs(result);
}

static RegisterPrimOp primop_bakedDerivation({
    .name = "__bakedDerivation",
    .args = {"attrs"},
    .doc = R"(
        Placeholder.
    )",
    .impl = prim_bakedDerivation,
});

} // namespace nix
