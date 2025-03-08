#include "flake-schemas.hh"
#include "eval-settings.hh"
#include "fetch-to-store.hh"
#include "memory-source-accessor.hh"
#include "strings-inline.hh"
#include "command.hh"
#include "value-to-json.hh"
#include "installable-flake.hh"
#include "flake-options.hh"

#include <sstream>

namespace nix::flake_schemas {

using namespace eval_cache;
using namespace flake;

static LockedFlake getBuiltinDefaultSchemasFlake(EvalState & state)
{
    auto accessor = make_ref<MemorySourceAccessor>();

    accessor->setPathDisplay("«builtin-flake-schemas»");

    accessor->addFile(
        CanonPath("flake.nix"),
#include "builtin-flake-schemas.nix.gen.hh"
    );

    // FIXME: remove this when we have lazy trees.
    auto storePath = fetchToStore(*state.store, {accessor}, FetchMode::Copy);
    state.allowPath(storePath);

    // Construct a dummy flakeref.
    auto flakeRef = parseFlakeRef(
        fetchSettings,
        fmt("tarball+https://builtin-flake-schemas?narHash=%s",
            state.store->queryPathInfo(storePath)->narHash.to_string(HashFormat::SRI, true)));

    auto flake = readFlake(state, flakeRef, flakeRef, flakeRef, state.rootPath(state.store->toRealPath(storePath)), {});

    return lockFlake(flakeSettings, state, flakeRef, {}, flake);
}

static std::pair<Value *, Value *> optionsToValue(ref<EvalState> state, const Options & options)
{
    auto vFingerprint = state->allocValue();
    auto vOptions = state->allocValue();

    auto attrsFingerprint = state->buildBindings(options.size());
    auto attrsOptions = state->buildBindings(options.size());

    for (auto & [name, value] : options) {
        std::visit(
            overloaded{
                [&](const std::string & s) {
                    attrsFingerprint.alloc(name).mkString(s);
                    attrsOptions.alloc(name).mkString(s);
                },
                [&](const Explicit<bool> & b) {
                    attrsFingerprint.alloc(name).mkBool(b.t);
                    attrsOptions.alloc(name).mkBool(b.t);
                },
                [&](const PackageOption & pkg) {
                    // FIXME: memoize InstallableFlake.
                    auto installable = make_ref<InstallableFlake>(
                        nullptr,
                        state,
                        FlakeRef(pkg.flakeRef),
                        pkg.fragment,
                        ExtendedOutputsSpec::Default(),
                        StringSet{"nix-build"}, // FIXME: get role from option schema
                        LockFlags{},
                        std::nullopt);

                    auto cursor = installable->getCursor(*state);

                    auto lockedFlake = installable->getLockedFlake();

                    auto s = lockedFlake->flake.lockedRef.to_string() \
                        + "#"
                        + toAttrPathStr(*state, cursor->getAttrPath());

                    attrsFingerprint.alloc(name).mkString(s);

                    // FIXME: reuse cursor
                    auto [value, pos] = installable->toValue(*state);
                    attrsOptions.insert(state->symbols.create(name), value, pos);
                },
                [&](const std::vector<PackageOption> & pkgs) {
                    auto listFingerprint = state->buildList(pkgs.size());
                    auto listOptions = state->buildList(pkgs.size());

                    for (const auto & [n, pkg] : enumerate(pkgs)) {
                        // FIXME: memoize InstallableFlake.
                        auto installable = make_ref<InstallableFlake>(
                            nullptr,
                            state,
                            FlakeRef(pkg.flakeRef),
                            pkg.fragment,
                            ExtendedOutputsSpec::Default(),
                            StringSet{"nix-build"}, // FIXME: get role from option schema
                            LockFlags{},
                            std::nullopt);

                        auto cursor = installable->getCursor(*state);

                        auto lockedFlake = installable->getLockedFlake();

                        auto s = lockedFlake->flake.lockedRef.to_string() \
                            + "#"
                            + toAttrPathStr(*state, cursor->getAttrPath());

                        listFingerprint[n] = state->allocValue();
                        listFingerprint[n]->mkString(s);

                        // FIXME: reuse cursor
                        auto [value, pos] = installable->toValue(*state);
                        listOptions[n] = value;
                    }

                    attrsFingerprint.alloc(name).mkList(listFingerprint);
                    attrsOptions.alloc(name).mkList(listOptions);
                },
                [&](const NixInt & n) {
                    attrsFingerprint.alloc(name).mkInt(n);
                    attrsOptions.alloc(name).mkInt(n);
                }},
            value);
    }

    vFingerprint->mkAttrs(attrsFingerprint);
    vOptions->mkAttrs(attrsOptions);

    return {vFingerprint, vOptions};
}

static Hash hashValue(EvalState & state, Value & v)
{
    std::ostringstream str;
    NixStringContext context;
    printValueAsJSON(state, true, v, noPos, str, context, false);
    return hashString(HashAlgorithm::SHA256, str.str());
}

ref<EvalCache> call(
    ref<EvalState> state,
    std::shared_ptr<flake::LockedFlake> lockedFlake,
    std::optional<FlakeRef> defaultSchemasFlake,
    const Options & options)
{
    auto [vFingerprint, vOptions] = optionsToValue(state, options);

    auto fingerprint = lockedFlake->getFingerprint(state->store, state->fetchSettings);

    std::string callFlakeSchemasNix =
#include "call-flake-schemas.nix.gen.hh"
        ;

    auto lockedDefaultSchemasFlake = defaultSchemasFlake
                                         ? flake::lockFlake(flakeSettings, *state, *defaultSchemasFlake, {})
                                         : getBuiltinDefaultSchemasFlake(*state);
    auto lockedDefaultSchemasFlakeFingerprint =
        lockedDefaultSchemasFlake.getFingerprint(state->store, state->fetchSettings);

    std::optional<Fingerprint> fingerprint2;
    if (fingerprint && lockedDefaultSchemasFlakeFingerprint)
        fingerprint2 = hashString(
            HashAlgorithm::SHA256,
            fmt("app:%s:%s:%s:%s",
                hashString(HashAlgorithm::SHA256, callFlakeSchemasNix).to_string(HashFormat::Base16, false),
                fingerprint->to_string(HashFormat::Base16, false),
                lockedDefaultSchemasFlakeFingerprint->to_string(HashFormat::Base16, false),
                hashValue(*state, *vFingerprint).to_string(HashFormat::Base16, false)));

    // FIXME: memoize eval cache on fingerprint to avoid opening the
    // same database twice.
    auto cache = make_ref<EvalCache>(
        evalSettings.useEvalCache && evalSettings.pureEval ? fingerprint2 : std::nullopt,
        *state,
        [state, lockedFlake, callFlakeSchemasNix, lockedDefaultSchemasFlake, vOptions]() {
            auto vCallFlakeSchemas = state->allocValue();
            state->eval(
                state->parseExprFromString(callFlakeSchemasNix, state->rootPath(CanonPath::root)), *vCallFlakeSchemas);

            auto vFlake = state->allocValue();
            flake::callFlake(*state, *lockedFlake, *vFlake);

            auto vDefaultSchemasFlake = state->allocValue();
            if (vFlake->type() == nAttrs && vFlake->attrs()->get(state->symbols.create("schemas")))
                vDefaultSchemasFlake->mkNull();
            else
                flake::callFlake(*state, lockedDefaultSchemasFlake, *vDefaultSchemasFlake);

            auto vRes = state->allocValue();
            Value * args[] = {vDefaultSchemasFlake, vFlake, vOptions};
            state->callFunction(*vCallFlakeSchemas, args, *vRes, noPos);

            return vRes;
        });

    /* Derive the flake output attribute path from the cursor used to
       traverse the inventory. We do this so we don't have to maintain
       a separate attrpath for that. */
    cache->cleanupAttrPath = [state](eval_cache::AttrPath && attrPath) {
        eval_cache::AttrPath res;
        auto i = attrPath.begin();
        if (i == attrPath.end())
            return attrPath;

        if (state->symbols[*i] == "inventory") {
            ++i;
            if (i != attrPath.end()) {
                res.push_back(*i++); // copy output name
                if (i != attrPath.end())
                    ++i; // skip "outputs"
                while (i != attrPath.end()) {
                    ++i; // skip "children"
                    if (i != attrPath.end())
                        res.push_back(*i++);
                }
            }
        }

        else if (state->symbols[*i] == "outputs") {
            res.insert(res.begin(), ++i, attrPath.end());
        }

        else
            abort();

        return res;
    };

    return cache;
}

void forEachOutput(
    ref<AttrCursor> inventory,
    std::function<void(Symbol outputName, std::shared_ptr<AttrCursor> output, const std::string & doc, bool isLast)> f)
{
    // FIXME: handle non-IFD outputs first.
    // evalSettings.enableImportFromDerivation.setDefault(false);

    auto outputNames = inventory->getAttrs();
    for (const auto & [i, outputName] : enumerate(outputNames)) {
        auto output = inventory->getAttr(outputName);
        try {
            auto isUnknown = (bool) output->maybeGetAttr("unknown");
            Activity act(*logger, lvlInfo, actUnknown, fmt("evaluating '%s'", output->getAttrPathStr()));
            f(outputName,
              isUnknown ? std::shared_ptr<AttrCursor>() : output->getAttr("node"),
              isUnknown ? "" : output->getAttr("doc")->getString(),
              i + 1 == outputNames.size());
        } catch (Error & e) {
            e.addTrace(nullptr, "while evaluating the flake output '%s':", output->getAttrPathStr());
            throw;
        }
    }
}

void visit(
    std::optional<std::string> system,
    ref<AttrCursor> node,
    std::function<void(ref<AttrCursor> leaf)> visitLeaf,
    std::function<void(std::function<void(ForEachChild)>)> visitNonLeaf,
    std::function<void(ref<AttrCursor> node, const std::vector<std::string> & systems)> visitFiltered)
{
    Activity act(*logger, lvlInfo, actUnknown, fmt("evaluating '%s'", node->getAttrPathStr()));

    /* Apply the system type filter. */
    if (system) {
        if (auto forSystems = node->maybeGetAttr("forSystems")) {
            auto systems = forSystems->getListOfStrings();
            if (std::find(systems.begin(), systems.end(), system) == systems.end()) {
                visitFiltered(node, systems);
                return;
            }
        }
    }

    if (auto children = node->maybeGetAttr("children")) {
        visitNonLeaf([&](ForEachChild f) {
            auto attrNames = children->getAttrs();
            for (const auto & [i, attrName] : enumerate(attrNames)) {
                try {
                    f(attrName, children->getAttr(attrName), i + 1 == attrNames.size());
                } catch (Error & e) {
                    // FIXME: make it a flake schema attribute whether to ignore evaluation errors.
                    if (node->root->state.symbols[node->getAttrPath()[0]] != "legacyPackages") {
                        e.addTrace(
                            nullptr, "while evaluating the flake output attribute '%s':", node->getAttrPathStr());
                        throw;
                    }
                }
            }
        });
    }

    else
        visitLeaf(ref(node));
}

std::optional<std::string> what(ref<AttrCursor> leaf)
{
    if (auto what = leaf->maybeGetAttr("what"))
        return what->getString();
    else
        return std::nullopt;
}

std::optional<std::string> shortDescription(ref<AttrCursor> leaf)
{
    if (auto what = leaf->maybeGetAttr("shortDescription")) {
        auto s = trim(what->getString());
        if (s != "")
            return s;
    }
    return std::nullopt;
}

std::shared_ptr<AttrCursor> derivation(ref<AttrCursor> leaf)
{
    return leaf->maybeGetAttr("derivation");
}

OrSuggestions<OutputInfo> getOutput(ref<AttrCursor> inventory, eval_cache::AttrPath attrPath)
{
    assert(!attrPath.empty());

    auto outputName = attrPath.front();

    auto schemaInfo = inventory->maybeGetAttr(outputName);
    if (!schemaInfo)
        return OrSuggestions<OutputInfo>::failed(inventory->getSuggestionsForAttr(outputName));

    auto node = schemaInfo->getAttr("node");

    auto pathLeft = std::span(attrPath).subspan(1);

    while (!pathLeft.empty()) {
        auto children = node->maybeGetAttr("children");
        if (!children)
            break;
        auto attr = pathLeft.front();
        auto childNode = children->maybeGetAttr(attr);
        if (!childNode)
            return OrSuggestions<OutputInfo>::failed(children->getSuggestionsForAttr(attr));
        node = ref(childNode);
        pathLeft = pathLeft.subspan(1);
    }

    return OutputInfo{
        .schemaInfo = ref(schemaInfo),
        .nodeInfo = node,
        .rawValue = node->getAttr("raw"),
        .leafAttrPath = std::vector(pathLeft.begin(), pathLeft.end()),
    };
}

Schemas getSchema(ref<AttrCursor> inventory)
{
    auto & state(inventory->root->state);

    Schemas schemas;

    for (auto & schemaName : inventory->getAttrs()) {
        auto schema = inventory->getAttr(schemaName);

        SchemaInfo schemaInfo;

        if (auto roles = schema->maybeGetAttr("roles")) {
            for (auto & roleName : roles->getAttrs()) {
                schemaInfo.roles.insert(std::string(state.symbols[roleName]));
            }
        }

        if (auto appendSystem = schema->maybeGetAttr("appendSystem"))
            schemaInfo.appendSystem = appendSystem->getBool();

        if (auto defaultAttrPath = schema->maybeGetAttr("defaultAttrPath")) {
            eval_cache::AttrPath attrPath;
            for (auto & s : defaultAttrPath->getListOfStrings())
                attrPath.push_back(state.symbols.create(s));
            schemaInfo.defaultAttrPath = std::move(attrPath);
        }

        schemas.insert_or_assign(std::string(state.symbols[schemaName]), std::move(schemaInfo));
    }

    return schemas;
}

}

namespace nix {

MixFlakeConfigOptions::MixFlakeConfigOptions()
{
    addFlag(
        {.longName = "string",
         .description = "Set a flake option to a string value.",
         .labels = {"name", "value"},
         .handler = {[&, this](std::string name, std::string value) { options.insert_or_assign(name, value); }}});

    addFlag(
        {.longName = "enable",
         .description = "Set a flake option to the Boolean value `true`.",
         .labels = {"name"},
         .handler = {[&, this](std::string name) { options.insert_or_assign(name, Explicit<bool>(true)); }}});

    addFlag(
        {.longName = "disable",
         .description = "Set a flake option to the Boolean value `false`.",
         .labels = {"name"},
         .handler = {[&, this](std::string name) { options.insert_or_assign(name, Explicit<bool>(false)); }}});

    auto packageCompleter = [&](AddCompletions & completions, size_t index, std::string_view prefix) {
        if (index == 1)
            completeFlakeRefWithFragment(
                completions,
                getEvalState(),
                flake::LockFlags{},
                StringSet{"nix-build"}, // FIXME: get role from option schema
                prefix);
    };

    addFlag(
        {.longName = "with",
         .description = "Set a flake option to a package specified as a flake output.",
         .labels = {"name", "flakeref"},
         .handler = {[&, this](std::string name, std::string value) {
             auto [flakeRef, fragment] = parseFlakeRefWithFragment(fetchSettings, value, absPath(getCommandBaseDir()));
             options.insert_or_assign(name, flake_schemas::PackageOption{.flakeRef = flakeRef, .fragment = fragment});
         }},
         .completer = {packageCompleter}});

    addFlag(
        {.longName = "plugin",
         .description = "Append a package to a flake option.",
         .labels = {"name", "flakeref"},
         .handler = {[&, this](std::string name, std::string value) {
             auto [flakeRef, fragment] = parseFlakeRefWithFragment(fetchSettings, value, absPath(getCommandBaseDir()));
             std::get<std::vector<flake_schemas::PackageOption>>(
                 options.emplace(name, std::vector<flake_schemas::PackageOption>()).first->second)
                 .push_back(flake_schemas::PackageOption{.flakeRef = flakeRef, .fragment = fragment});
         }},
         .completer = {packageCompleter}});

    addFlag(
        {.longName = "int",
         .description = "Set a flake option to an integer value.",
         .labels = {"name", "value"},
         .handler = {[&, this](std::string name, std::string value) {
             if (auto n = string2Int<NixInt::Inner>(value))
                 options.insert_or_assign(name, NixInt(*n));
             else
                 throw UsageError("not an integer: '%s'", value);
         }}});
}

MixFlakeSchemas::MixFlakeSchemas()
{
    addFlag(
        {.longName = "default-flake-schemas",
         .description = "The URL of the flake providing default flake schema definitions.",
         .labels = {"flake-ref"},
         .handler = {&defaultFlakeSchemas},
         .completer = {[&](AddCompletions & completions, size_t, std::string_view prefix) {
             completeFlakeRef(completions, getStore(), prefix);
         }}});
}

std::optional<FlakeRef> MixFlakeSchemas::getDefaultFlakeSchemas()
{
    if (!defaultFlakeSchemas)
        return std::nullopt;
    else
        return parseFlakeRef(fetchSettings, *defaultFlakeSchemas, absPath(getCommandBaseDir()));
}

}
