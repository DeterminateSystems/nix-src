#include "nix/store/globals.hh"
#include "nix/cmd/installable-flake.hh"
#include "nix/cmd/installable-derived-path.hh"
#include "nix/store/outputs-spec.hh"
#include "nix/util/util.hh"
#include "nix/cmd/command.hh"
#include "nix/expr/attr-path.hh"
#include "nix/cmd/common-eval-args.hh"
#include "nix/store/derivations.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/get-drvs.hh"
#include "nix/store/store-api.hh"
#include "nix/main/shared.hh"
#include "nix/flake/flake.hh"
#include "nix/expr/eval-cache.hh"
#include "nix/util/url.hh"
#include "nix/fetchers/registry.hh"
#include "nix/store/build-result.hh"
#include "nix/cmd/flake-schemas.hh"

#include <regex>
#include <queue>

#include <nlohmann/json.hpp>

namespace nix {

static std::string showAttrPaths(EvalState & state, const std::vector<eval_cache::AttrPath> & paths)
{
    std::string s;
    for (const auto & [n, i] : enumerate(paths)) {
        if (n > 0)
            s += n + 1 == paths.size() ? " or " : ", ";
        s += '\'';
        s += eval_cache::toAttrPathStr(state, i);
        s += '\'';
    }
    return s;
}

InstallableFlake::InstallableFlake(
    SourceExprCommand * cmd,
    ref<EvalState> state,
    FlakeRef && flakeRef,
    std::string_view fragment,
    ExtendedOutputsSpec extendedOutputsSpec,
    StringSet roles,
    const flake::LockFlags & lockFlags,
    std::optional<FlakeRef> defaultFlakeSchemas)
    : InstallableValue(state)
    , flakeRef(flakeRef)
    , fragment(fragment)
    , roles(roles)
    , extendedOutputsSpec(std::move(extendedOutputsSpec))
    , lockFlags(lockFlags)
    , defaultFlakeSchemas(defaultFlakeSchemas)
{
    if (cmd && cmd->getAutoArgs(*state)->size())
        throw UsageError("'--arg' and '--argstr' are incompatible with flakes");
}

DerivedPathsWithInfo InstallableFlake::toDerivedPaths()
{
    Activity act(*logger, lvlTalkative, actUnknown, fmt("evaluating derivation '%s'", what()));

    auto attr = getCursor(*state);

    auto attrPath = attr->getAttrPathStr();

    if (!attr->isDerivation()) {

        // FIXME: use eval cache?
        auto v = attr->forceValue();

        if (std::optional derivedPathWithInfo = trySinglePathToDerivedPaths(
                v, noPos, fmt("while evaluating the flake output attribute '%s'", attrPath))) {
            return {*derivedPathWithInfo};
        } else {
            throw Error(
                "expected flake output attribute '%s' to be a derivation or path but found %s: %s",
                attrPath,
                showType(v),
                ValuePrinter(*this->state, v, errorPrintOptions));
        }
    }

    auto drvPath = attr->forceDerivation();
    state->waitForPath(drvPath);

    std::optional<NixInt::Inner> priority;

    if (attr->maybeGetAttr(state->sOutputSpecified)) {
    } else if (auto aMeta = attr->maybeGetAttr(state->sMeta)) {
        if (auto aPriority = aMeta->maybeGetAttr("priority"))
            priority = aPriority->getInt().value;
    }

    return {{
        .path =
            DerivedPath::Built{
                .drvPath = makeConstantStorePathRef(std::move(drvPath)),
                .outputs = std::visit(
                    overloaded{
                        [&](const ExtendedOutputsSpec::Default & d) -> OutputsSpec {
                            StringSet outputsToInstall;
                            if (auto aOutputSpecified = attr->maybeGetAttr(state->sOutputSpecified)) {
                                if (aOutputSpecified->getBool()) {
                                    if (auto aOutputName = attr->maybeGetAttr("outputName"))
                                        outputsToInstall = {aOutputName->getString()};
                                }
                            } else if (auto aMeta = attr->maybeGetAttr(state->sMeta)) {
                                if (auto aOutputsToInstall = aMeta->maybeGetAttr("outputsToInstall"))
                                    for (auto & s : aOutputsToInstall->getListOfStrings())
                                        outputsToInstall.insert(s);
                            }

                            if (outputsToInstall.empty())
                                outputsToInstall.insert("out");

                            return OutputsSpec::Names{std::move(outputsToInstall)};
                        },
                        [&](const ExtendedOutputsSpec::Explicit & e) -> OutputsSpec { return e; },
                    },
                    extendedOutputsSpec.raw),
            },
        .info = make_ref<ExtraPathInfoFlake>(
            ExtraPathInfoValue::Value{
                .priority = priority,
                .attrPath = attrPath,
                .extendedOutputsSpec = extendedOutputsSpec,
            },
            ExtraPathInfoFlake::Flake{
                .originalRef = flakeRef,
                .lockedRef = getLockedFlake()->flake.lockedRef,
            }),
    }};
}

std::pair<Value *, PosIdx> InstallableFlake::toValue(EvalState & state)
{
    return {&getCursor(state)->forceValue(), noPos};
}

std::vector<ref<eval_cache::AttrCursor>> InstallableFlake::getCursors(EvalState & state)
{
    auto cache = flake_schemas::call(state, getLockedFlake(), defaultFlakeSchemas);

    auto inventory = cache->getRoot()->getAttr("inventory");
    auto outputs = cache->getRoot()->getAttr("outputs");

    std::vector<ref<eval_cache::AttrCursor>> res;

    Suggestions suggestions;

    std::vector<eval_cache::AttrPath> attrPaths;

    if (fragment.starts_with("."))
        attrPaths.push_back(parseAttrPath(state, fragment.substr(1)));
    else {
        auto schemas = flake_schemas::getSchema(inventory);

        // FIXME: Ugly hack to preserve the historical precedence
        // between outputs. We should add a way for schemas to declare
        // priorities.
        std::vector<std::string> schemasSorted;
        std::set<std::string> schemasSeen;
        auto doSchema = [&](const std::string & schema) {
            if (schemas.contains(schema)) {
                schemasSorted.push_back(schema);
                schemasSeen.insert(schema);
            }
        };
        doSchema("apps");
        doSchema("devShells");
        doSchema("packages");
        doSchema("legacyPackages");
        for (auto & schema : schemas)
            if (!schemasSeen.contains(schema.first))
                schemasSorted.push_back(schema.first);

        auto parsedFragment = parseAttrPath(state, fragment);

        for (auto & role : roles) {
            for (auto & schemaName : schemasSorted) {
                auto & schema = schemas.find(schemaName)->second;
                if (schema.roles.contains(role)) {
                    eval_cache::AttrPath attrPath{state.symbols.create(schemaName)};
                    if (schema.appendSystem)
                        attrPath.push_back(state.symbols.create(settings.thisSystem.get()));

                    if (parsedFragment.empty()) {
                        if (schema.defaultAttrPath) {
                            auto attrPath2{attrPath};
                            for (auto & x : *schema.defaultAttrPath)
                                attrPath2.push_back(x);
                            attrPaths.push_back(attrPath2);
                        }
                    } else {
                        auto attrPath2{attrPath};
                        for (auto & x : parsedFragment)
                            attrPath2.push_back(x);
                        attrPaths.push_back(attrPath2);
                    }
                }
            }
        }

        if (!parsedFragment.empty())
            attrPaths.push_back(parsedFragment);

        // FIXME: compatibility hack to get `nix repl` to return all
        // outputs by default.
        if (parsedFragment.empty() && roles.contains("nix-repl"))
            attrPaths.push_back({});
    }

    if (attrPaths.empty())
        throw Error("flake '%s' does not provide a default output", flakeRef);

    for (auto & attrPath : attrPaths) {
        debug("trying flake output attribute '%s'", eval_cache::toAttrPathStr(state, attrPath));

        auto outputInfo = flake_schemas::getOutput(inventory, attrPath);

        if (outputInfo && outputInfo->leafAttrPath.empty()) {
            if (auto drv = outputInfo->nodeInfo->maybeGetAttr("derivation")) {
                res.push_back(ref(drv));
                continue;
            }
        }

        auto attr = outputs->findAlongAttrPath(attrPath);
        if (attr)
            res.push_back(ref(*attr));
        else
            suggestions += attr.getSuggestions();
    }

    if (res.size() == 0)
        throw Error(suggestions, "flake '%s' does not provide attribute %s", flakeRef, showAttrPaths(state, attrPaths));

    return res;
}

std::shared_ptr<flake::LockedFlake> InstallableFlake::getLockedFlake() const
{
    if (!_lockedFlake) {
        flake::LockFlags lockFlagsApplyConfig = lockFlags;
        // FIXME why this side effect?
        lockFlagsApplyConfig.applyNixConfig = true;
        _lockedFlake =
            std::make_shared<flake::LockedFlake>(lockFlake(flakeSettings, *state, flakeRef, lockFlagsApplyConfig));
    }
    return _lockedFlake;
}

ref<eval_cache::EvalCache> InstallableFlake::openEvalCache() const
{
    if (!_evalCache) {
        _evalCache = flake_schemas::call(*state, getLockedFlake(), defaultFlakeSchemas);
    }
    return ref(_evalCache);
}

FlakeRef InstallableFlake::nixpkgsFlakeRef() const
{
    auto lockedFlake = getLockedFlake();

    if (auto nixpkgsInput = lockedFlake->lockFile.findInput({"nixpkgs"})) {
        if (auto lockedNode = std::dynamic_pointer_cast<const flake::LockedNode>(nixpkgsInput)) {
            debug("using nixpkgs flake '%s'", lockedNode->lockedRef);
            return std::move(lockedNode->lockedRef);
        }
    }

    return defaultNixpkgsFlakeRef();
}

} // namespace nix
