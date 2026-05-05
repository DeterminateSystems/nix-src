#include "nix/expr/primops.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/store/derivations.hh"
#include "nix/store/store-api.hh"
#include "nix/util/hash.hh"

namespace nix {

static void prim_fakeDerivation(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceAttrs(*args[0], pos, "while evaluating the argument passed to builtins.fakeDerivation");

    std::optional<std::string> name;

    struct FakeDerivationOutput
    {
        StorePath path;
        std::optional<Hash> narHash;
    };

    std::map<std::string, FakeDerivationOutput> outputs;

    for (auto & attr : *args[0]->attrs()) {
        std::string_view attrName = state.symbols[attr.name];
        auto attrHint = fmt("while evaluating the attribute '%s' passed to builtins.fakeDerivation", attrName);

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
                    fmt("while evaluating the output '%s' passed to builtins.fakeDerivation", outName));

                std::optional<StorePath> path;
                std::optional<Hash> narHash;

                for (auto & outField : *outAttr.value->attrs()) {
                    std::string_view fieldName = state.symbols[outField.name];
                    auto fieldHint =
                        fmt("while evaluating the attribute '%s' of output '%s' passed to builtins.fakeDerivation",
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
                                "attribute '%s' isn't supported in an output passed to 'builtins.fakeDerivation'",
                                fieldName)
                            .atPos(outField.pos)
                            .debugThrow();
                }

                if (!path)
                    state
                        .error<EvalError>(
                            "attribute 'path' is missing in output '%s' passed to 'builtins.fakeDerivation'", outName)
                        .atPos(outAttr.pos)
                        .debugThrow();

                outputs.emplace(std::string(outName), FakeDerivationOutput{std::move(*path), std::move(narHash)});
            }
        }

        else
            state.error<EvalError>("attribute '%s' isn't supported in call to 'builtins.fakeDerivation'", attrName)
                .atPos(attr.pos)
                .debugThrow();
    }

    if (!name)
        state.error<EvalError>("attribute 'name' is missing in call to 'builtins.fakeDerivation'")
            .atPos(pos)
            .debugThrow();

    if (outputs.empty())
        state.error<EvalError>("attribute 'outputs' is missing or empty in call to 'builtins.fakeDerivation'")
            .atPos(pos)
            .debugThrow();

    Derivation drv;
    drv.name = *name;
    drv.platform = "builtin";
    drv.builder = "builtin:fake-derivation";

    for (auto & [outName, out] : outputs) {
#if 0
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
#endif
        drv.outputs.insert_or_assign(outName, DerivationOutput::InputAddressed{.path = out.path});
    }

    auto drvPath = state.store->writeDerivation(*state.asyncPathWriter, drv, state.repair);

    // FIXME
    state.waitForPath(drvPath);

    state.mkStorePathString(drvPath, v);
}

static RegisterPrimOp primop_fakeDerivation({
    .name = "__fakeDerivation",
    .args = {"attrs"},
    .doc = R"(
        Placeholder.
    )",
    .impl = prim_fakeDerivation,
});

} // namespace nix
