#include "nix/main/common-args.hh"
#include "nix/main/shared.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/get-drvs.hh"
#include "nix/util/signals.hh"
#include "nix/store/store-open.hh"
#include "nix/store/derivations.hh"
#include "nix/store/outputs-spec.hh"
#include "nix/expr/attr-path.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/fetchers/registry.hh"
#include "nix/expr/eval-cache.hh"
#include "nix/cmd/markdown.hh"
#include "nix/util/users.hh"
#include "nix/fetchers/fetch-to-store.hh"
#include "nix/store/local-fs-store.hh"
#include "nix/store/globals.hh"
#include "nix/expr/parallel-eval.hh"
#include "nix/cmd/flake-schemas.hh"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <iomanip>

#include "nix/util/strings-inline.hh"

// FIXME is this supposed to be private or not?
#include "flake-command.hh"

namespace nix::fs {
using namespace std::filesystem;
}

using namespace nix;
using namespace nix::flake;
using json = nlohmann::json;

struct CmdFlakeUpdate;

FlakeCommand::FlakeCommand()
{
    expectArgs(
        {.label = "flake-url",
         .optional = true,
         .handler = {&flakeUrl},
         .completer = {[&](AddCompletions & completions, size_t, std::string_view prefix) {
             completeFlakeRef(completions, getStore(), prefix);
         }}});
}

FlakeRef FlakeCommand::getFlakeRef()
{
    return parseFlakeRef(fetchSettings, flakeUrl, std::filesystem::current_path().string()); // FIXME
}

LockedFlake FlakeCommand::lockFlake()
{
    return flake::lockFlake(flakeSettings, *getEvalState(), getFlakeRef(), lockFlags);
}

std::vector<FlakeRef> FlakeCommand::getFlakeRefsForCompletion()
{
    return {// Like getFlakeRef but with expandTilde called first
            parseFlakeRef(fetchSettings, expandTilde(flakeUrl), std::filesystem::current_path().string())};
}

struct CmdFlakeUpdate : FlakeCommand
{
public:

    std::string description() override
    {
        return "update flake lock file";
    }

    CmdFlakeUpdate()
    {
        expectedArgs.clear();
        addFlag({
            .longName = "flake",
            .description = "The flake to operate on. Default is the current directory.",
            .labels = {"flake-url"},
            .handler = {&flakeUrl},
            .completer = {[&](AddCompletions & completions, size_t, std::string_view prefix) {
                completeFlakeRef(completions, getStore(), prefix);
            }},
        });
        expectArgs({
            .label = "inputs",
            .optional = true,
            .handler = {[&](std::vector<std::string> inputsToUpdate) {
                for (const auto & inputToUpdate : inputsToUpdate) {
                    InputAttrPath inputAttrPath;
                    try {
                        inputAttrPath = flake::parseInputAttrPath(inputToUpdate);
                    } catch (Error & e) {
                        warn(
                            "Invalid flake input '%s'. To update a specific flake, use 'nix flake update --flake %s' instead.",
                            inputToUpdate,
                            inputToUpdate);
                        throw e;
                    }
                    if (lockFlags.inputUpdates.contains(inputAttrPath))
                        warn(
                            "Input '%s' was specified multiple times. You may have done this by accident.",
                            printInputAttrPath(inputAttrPath));
                    lockFlags.inputUpdates.insert(inputAttrPath);
                }
            }},
            .completer = {[&](AddCompletions & completions, size_t, std::string_view prefix) {
                completeFlakeInputAttrPath(completions, getEvalState(), getFlakeRefsForCompletion(), prefix);
            }},
        });

        /* Remove flags that don't make sense. */
        removeFlag("no-update-lock-file");
        removeFlag("no-write-lock-file");
    }

    std::string doc() override
    {
        return
#include "flake-update.md"
            ;
    }

    void run(nix::ref<nix::Store> store) override
    {
        settings.tarballTtl = 0;
        auto updateAll = lockFlags.inputUpdates.empty();

        lockFlags.recreateLockFile = updateAll;
        lockFlags.writeLockFile = true;
        lockFlags.applyNixConfig = true;
        lockFlags.requireLockable = false;

        lockFlake();
    }
};

struct CmdFlakeLock : FlakeCommand
{
    std::string description() override
    {
        return "create missing lock file entries";
    }

    CmdFlakeLock()
    {
        /* Remove flags that don't make sense. */
        removeFlag("no-write-lock-file");
    }

    std::string doc() override
    {
        return
#include "flake-lock.md"
            ;
    }

    void run(nix::ref<nix::Store> store) override
    {
        settings.tarballTtl = 0;

        lockFlags.writeLockFile = true;
        lockFlags.failOnUnlocked = true;
        lockFlags.applyNixConfig = true;
        lockFlags.requireLockable = false;

        lockFlake();
    }
};

struct CmdFlakeMetadata : FlakeCommand, MixJSON
{
    std::string description() override
    {
        return "show flake metadata";
    }

    std::string doc() override
    {
        return
#include "flake-metadata.md"
            ;
    }

    void run(nix::ref<nix::Store> store) override
    {
        lockFlags.requireLockable = false;
        auto lockedFlake = lockFlake();
        auto & flake = lockedFlake.flake;

        /* Hack to show the store path if available. */
        std::optional<StorePath> storePath;
        if (store->isInStore(flake.path.path.abs())) {
            auto path = store->toStorePath(flake.path.path.abs()).first;
            if (store->isValidPath(path))
                storePath = path;
        }

        if (json) {
            nlohmann::json j;
            if (flake.description)
                j["description"] = *flake.description;
            j["originalUrl"] = flake.originalRef.to_string();
            j["original"] = fetchers::attrsToJSON(flake.originalRef.toAttrs());
            j["resolvedUrl"] = flake.resolvedRef.to_string();
            j["resolved"] = fetchers::attrsToJSON(flake.resolvedRef.toAttrs());
            j["url"] = flake.lockedRef.to_string(); // FIXME: rename to lockedUrl
            // "locked" is a misnomer - this is the result of the
            // attempt to lock.
            j["locked"] = fetchers::attrsToJSON(flake.lockedRef.toAttrs());
            if (auto rev = flake.lockedRef.input.getRev())
                j["revision"] = rev->to_string(HashFormat::Base16, false);
            if (auto dirtyRev = fetchers::maybeGetStrAttr(flake.lockedRef.toAttrs(), "dirtyRev"))
                j["dirtyRevision"] = *dirtyRev;
            if (auto revCount = flake.lockedRef.input.getRevCount())
                j["revCount"] = *revCount;
            if (auto lastModified = flake.lockedRef.input.getLastModified())
                j["lastModified"] = *lastModified;
            if (storePath)
                j["path"] = store->printStorePath(*storePath);
            j["locks"] = lockedFlake.lockFile.toJSON().first;
            if (auto fingerprint = lockedFlake.getFingerprint(store, fetchSettings))
                j["fingerprint"] = fingerprint->to_string(HashFormat::Base16, false);
            printJSON(j);
        } else {
            logger->cout(ANSI_BOLD "Resolved URL:" ANSI_NORMAL "  %s", flake.resolvedRef.to_string());
            if (flake.lockedRef.input.isLocked())
                logger->cout(ANSI_BOLD "Locked URL:" ANSI_NORMAL "    %s", flake.lockedRef.to_string());
            if (flake.description)
                logger->cout(ANSI_BOLD "Description:" ANSI_NORMAL "   %s", *flake.description);
            if (storePath)
                logger->cout(ANSI_BOLD "Path:" ANSI_NORMAL "          %s", store->printStorePath(*storePath));
            if (auto rev = flake.lockedRef.input.getRev())
                logger->cout(ANSI_BOLD "Revision:" ANSI_NORMAL "      %s", rev->to_string(HashFormat::Base16, false));
            if (auto dirtyRev = fetchers::maybeGetStrAttr(flake.lockedRef.toAttrs(), "dirtyRev"))
                logger->cout(ANSI_BOLD "Revision:" ANSI_NORMAL "      %s", *dirtyRev);
            if (auto revCount = flake.lockedRef.input.getRevCount())
                logger->cout(ANSI_BOLD "Revisions:" ANSI_NORMAL "     %s", *revCount);
            if (auto lastModified = flake.lockedRef.input.getLastModified())
                logger->cout(
                    ANSI_BOLD "Last modified:" ANSI_NORMAL " %s",
                    std::put_time(std::localtime(&*lastModified), "%F %T"));
            if (auto fingerprint = lockedFlake.getFingerprint(store, fetchSettings))
                logger->cout(
                    ANSI_BOLD "Fingerprint:" ANSI_NORMAL "   %s", fingerprint->to_string(HashFormat::Base16, false));

            if (!lockedFlake.lockFile.root->inputs.empty())
                logger->cout(ANSI_BOLD "Inputs:" ANSI_NORMAL);

            std::set<ref<Node>> visited;

            std::function<void(const Node & node, const std::string & prefix)> recurse;

            recurse = [&](const Node & node, const std::string & prefix) {
                for (const auto & [i, input] : enumerate(node.inputs)) {
                    bool last = i + 1 == node.inputs.size();

                    if (auto lockedNode = std::get_if<0>(&input.second)) {
                        std::string lastModifiedStr = "";
                        if (auto lastModified = (*lockedNode)->lockedRef.input.getLastModified())
                            lastModifiedStr = fmt(" (%s)", std::put_time(std::gmtime(&*lastModified), "%F %T"));
                        logger->cout(
                            "%s" ANSI_BOLD "%s" ANSI_NORMAL ": %s%s",
                            prefix + (last ? treeLast : treeConn),
                            input.first,
                            (*lockedNode)->lockedRef,
                            lastModifiedStr);

                        bool firstVisit = visited.insert(*lockedNode).second;

                        if (firstVisit)
                            recurse(**lockedNode, prefix + (last ? treeNull : treeLine));
                    } else if (auto follows = std::get_if<1>(&input.second)) {
                        logger->cout(
                            "%s" ANSI_BOLD "%s" ANSI_NORMAL " follows input '%s'",
                            prefix + (last ? treeLast : treeConn),
                            input.first,
                            printInputAttrPath(*follows));
                    }
                }
            };

            visited.insert(lockedFlake.lockFile.root);
            recurse(*lockedFlake.lockFile.root, "");
        }
    }
};

struct CmdFlakeInfo : CmdFlakeMetadata
{
    void run(nix::ref<nix::Store> store) override
    {
        warn("'nix flake info' is a deprecated alias for 'nix flake metadata'");
        CmdFlakeMetadata::run(store);
    }
};

struct CmdFlakeCheck : FlakeCommand, MixFlakeSchemas
{
    bool build = true;
    bool checkAllSystems = false;

    CmdFlakeCheck()
    {
        addFlag({
            .longName = "no-build",
            .description = "Do not build checks.",
            .handler = {&build, false},
        });
        addFlag({
            .longName = "all-systems",
            .description = "Check the outputs for all systems.",
            .handler = {&checkAllSystems, true},
        });
    }

    std::string description() override
    {
        return "check whether the flake evaluates and run its tests";
    }

    std::string doc() override
    {
        return
#include "flake-check.md"
            ;
    }

    void run(nix::ref<nix::Store> store) override
    {
        if (!build) {
            settings.readOnlyMode = true;
            evalSettings.enableImportFromDerivation.setDefault(false);
        }

        auto state = getEvalState();

        lockFlags.applyNixConfig = true;
        auto flake = std::make_shared<LockedFlake>(lockFlake());
        auto localSystem = std::string(settings.thisSystem.get());

        auto cache = flake_schemas::call(*state, flake, getDefaultFlakeSchemas());

        auto inventory = cache->getRoot()->getAttr("inventory");

        std::vector<DerivedPath> drvPaths;

        Sync<std::set<std::string>> uncheckedOutputs;
        Sync<std::set<std::string>> omittedSystems;

        std::function<void(ref<eval_cache::AttrCursor> node)> visit;

        std::atomic_bool hasErrors = false;

        auto reportError = [&](const Error & e) {
            try {
                throw e;
            } catch (Interrupted & e) {
                throw;
            } catch (Error & e) {
                if (settings.keepGoing) {
                    logError({.msg = e.info().msg});
                    hasErrors = true;
                } else
                    throw;
            }
        };

        visit = [&](ref<eval_cache::AttrCursor> node) {
            flake_schemas::visit(
                checkAllSystems ? std::optional<std::string>() : localSystem,
                node,

                [&](ref<eval_cache::AttrCursor> leaf) {
                    if (auto evalChecks = leaf->maybeGetAttr("evalChecks")) {
                        auto checkNames = evalChecks->getAttrs();
                        for (auto & checkName : checkNames) {
                            // FIXME: update activity
                            auto cursor = evalChecks->getAttr(checkName);
                            auto b = cursor->getBool();
                            if (!b)
                                reportError(Error("Evaluation check '%s' failed.", cursor->getAttrPathStr()));
                        }
                    }

                    if (auto drv = flake_schemas::derivation(leaf)) {
                        if (auto isFlakeCheck = leaf->maybeGetAttr("isFlakeCheck")) {
                            if (isFlakeCheck->getBool()) {
                                auto drvPath = drv->forceDerivation();
                                drvPaths.push_back(
                                    DerivedPath::Built{
                                        .drvPath = makeConstantStorePathRef(drvPath),
                                        .outputs = OutputsSpec::All{},
                                    });
                            }
                        }
                    }
                },

                [&](std::function<void(flake_schemas::ForEachChild)> forEachChild) {
                    forEachChild([&](Symbol attrName, ref<eval_cache::AttrCursor> node, bool isLast) { visit(node); });
                },

                [&](ref<eval_cache::AttrCursor> node, const std::vector<std::string> & systems) {
                    for (auto & s : systems)
                        omittedSystems.lock()->insert(s);
                });
        };

        flake_schemas::forEachOutput(
            inventory,
            [&](Symbol outputName,
                std::shared_ptr<eval_cache::AttrCursor> output,
                const std::string & doc,
                bool isLast) {
                if (output) {
                    visit(ref(output));
                } else
                    uncheckedOutputs.lock()->insert(std::string(state->symbols[outputName]));
            });

        if (!uncheckedOutputs.lock()->empty())
            warn(
                "The following flake outputs are unchecked: %s.",
                concatStringsSep(", ", *uncheckedOutputs.lock())); // FIXME: quote

        if (build && !drvPaths.empty()) {
            // FIXME: should start building while evaluating.
            Activity act(*logger, lvlInfo, actUnknown, fmt("running %d flake checks", drvPaths.size()));

            state->waitForAllPaths();

            auto missing = store->queryMissing(drvPaths);

            /* This command doesn't need to actually substitute
               derivation outputs if they're missing but
               substitutable. So filter out derivations that are
               substitutable or already built. */
            std::vector<DerivedPath> toBuild;
            for (auto & path : drvPaths) {
                std::visit(
                    overloaded{
                        [&](const DerivedPath::Built & bfd) {
                            auto drvPathP = std::get_if<DerivedPath::Opaque>(&*bfd.drvPath);
                            if (!drvPathP || missing.willBuild.contains(drvPathP->path)
                                || missing.unknown.contains(drvPathP->path))
                                toBuild.push_back(path);
                        },
                        [&](const DerivedPath::Opaque & bo) {
                            if (!missing.willSubstitute.contains(bo.path))
                                toBuild.push_back(path);
                        },
                    },
                    path.raw());
            }

            store->buildPaths(toBuild);
        }

        if (hasErrors)
            throw Error("some errors were encountered during the evaluation");

        if (!omittedSystems.lock()->empty()) {
            // TODO: empty system is not visible; render all as nix strings?
            warn(
                "The check omitted these incompatible systems: %s\n"
                "Use '--all-systems' to check all.",
                concatStringsSep(", ", *omittedSystems.lock()));
        }
    };
};

struct CmdFlakeInitCommon : virtual Args, EvalCommand, MixFlakeSchemas
{
    std::string templateUrl = "https://flakehub.com/f/DeterminateSystems/flake-templates/0.1";
    Path destDir;

    const LockFlags lockFlags{.writeLockFile = false};

    CmdFlakeInitCommon()
    {
        addFlag({
            .longName = "template",
            .shortName = 't',
            .description = "The template to use.",
            .labels = {"template"},
            .handler = {&templateUrl},
            .completer = {[&](AddCompletions & completions, size_t, std::string_view prefix) {
                completeFlakeRefWithFragment(completions, getEvalState(), lockFlags, {"nix-template"}, prefix);
            }},
        });
    }

    void run(nix::ref<nix::Store> store) override
    {
        auto flakeDir = absPath(destDir);

        auto evalState = getEvalState();

        auto [templateFlakeRef, templateName] =
            parseFlakeRefWithFragment(fetchSettings, templateUrl, std::filesystem::current_path().string());

        auto installable = InstallableFlake(
            nullptr,
            evalState,
            std::move(templateFlakeRef),
            templateName,
            ExtendedOutputsSpec::Default(),
            {"nix-template"},
            lockFlags,
            {});

        auto cursor = installable.getCursor(*evalState);

        auto templateDirAttr = cursor->getAttr("path")->forceValue();
        NixStringContext context;
        auto templateDir = evalState->coerceToPath(noPos, templateDirAttr, context, "");

        std::vector<std::filesystem::path> changedFiles;
        std::vector<std::filesystem::path> conflictedFiles;

        std::function<void(const SourcePath & from, const std::filesystem::path & to)> copyDir;
        copyDir = [&](const SourcePath & from, const std::filesystem::path & to) {
            createDirs(to);

            for (auto & [name, entry] : from.readDirectory()) {
                checkInterrupt();
                auto from2 = from / name;
                auto to2 = to / name;
                auto st = from2.lstat();
                auto to_st = std::filesystem::symlink_status(to2);
                if (st.type == SourceAccessor::tDirectory)
                    copyDir(from2, to2);
                else if (st.type == SourceAccessor::tRegular) {
                    auto contents = from2.readFile();
                    if (std::filesystem::exists(to_st)) {
                        auto contents2 = readFile(to2.string());
                        if (contents != contents2) {
                            printError(
                                "refusing to overwrite existing file '%s'\n please merge it manually with '%s'",
                                to2.string(),
                                from2);
                            conflictedFiles.push_back(to2);
                        } else {
                            notice("skipping identical file: %s", from2);
                        }
                        continue;
                    } else
                        writeFile(to2, contents);
                } else if (st.type == SourceAccessor::tSymlink) {
                    auto target = from2.readLink();
                    if (std::filesystem::exists(to_st)) {
                        if (std::filesystem::read_symlink(to2) != target) {
                            printError(
                                "refusing to overwrite existing file '%s'\n please merge it manually with '%s'",
                                to2.string(),
                                from2);
                            conflictedFiles.push_back(to2);
                        } else {
                            notice("skipping identical file: %s", from2);
                        }
                        continue;
                    } else
                        createSymlink(target, os_string_to_string(PathViewNG{to2}));
                } else
                    throw Error(
                        "path '%s' needs to be a symlink, file, or directory but instead is a %s",
                        from2,
                        st.typeString());
                changedFiles.push_back(to2);
                notice("wrote: %s", to2);
            }
        };

        copyDir(templateDir, flakeDir);

        if (!changedFiles.empty() && std::filesystem::exists(std::filesystem::path{flakeDir} / ".git")) {
            Strings args = {"-C", flakeDir, "add", "--intent-to-add", "--force", "--"};
            for (auto & s : changedFiles)
                args.emplace_back(s.string());
            runProgram("git", true, args);
        }

        if (auto welcomeText = cursor->maybeGetAttr("welcomeText")) {
            notice("\n");
            notice(renderMarkdownToTerminal(welcomeText->getString()));
        }

        if (!conflictedFiles.empty())
            throw Error("encountered %d conflicts - see above", conflictedFiles.size());
    }
};

struct CmdFlakeInit : CmdFlakeInitCommon
{
    std::string description() override
    {
        return "create a flake in the current directory from a template";
    }

    std::string doc() override
    {
        return
#include "flake-init.md"
            ;
    }

    CmdFlakeInit()
    {
        destDir = ".";
    }
};

struct CmdFlakeNew : CmdFlakeInitCommon
{
    std::string description() override
    {
        return "create a flake in the specified directory from a template";
    }

    std::string doc() override
    {
        return
#include "flake-new.md"
            ;
    }

    CmdFlakeNew()
    {
        expectArgs({.label = "dest-dir", .handler = {&destDir}, .completer = completePath});
    }
};

struct CmdFlakeClone : FlakeCommand
{
    Path destDir;

    std::string description() override
    {
        return "clone flake repository";
    }

    std::string doc() override
    {
        return
#include "flake-clone.md"
            ;
    }

    CmdFlakeClone()
    {
        addFlag({
            .longName = "dest",
            .shortName = 'f',
            .description = "Clone the flake to path *dest*.",
            .labels = {"path"},
            .handler = {&destDir},
        });
    }

    void run(nix::ref<nix::Store> store) override
    {
        if (destDir.empty())
            throw Error("missing flag '--dest'");

        getFlakeRef().resolve(store).input.clone(destDir);
    }
};

struct CmdFlakeArchive : FlakeCommand, MixJSON, MixDryRun
{
    std::string dstUri;

    CheckSigsFlag checkSigs = CheckSigs;

    SubstituteFlag substitute = NoSubstitute;

    CmdFlakeArchive()
    {
        addFlag({
            .longName = "to",
            .description = "URI of the destination Nix store",
            .labels = {"store-uri"},
            .handler = {&dstUri},
        });
        addFlag({
            .longName = "no-check-sigs",
            .description = "Do not require that paths are signed by trusted keys.",
            .handler = {&checkSigs, NoCheckSigs},
        });
    }

    std::string description() override
    {
        return "copy a flake and all its inputs to a store";
    }

    std::string doc() override
    {
        return
#include "flake-archive.md"
            ;
    }

    void run(nix::ref<nix::Store> store) override
    {
        auto flake = lockFlake();

        StorePathSet sources;

        auto storePath = dryRun ? flake.flake.lockedRef.input.computeStorePath(*store)
                                : std::get<StorePath>(flake.flake.lockedRef.input.fetchToStore(store));

        sources.insert(storePath);

        // FIXME: use graph output, handle cycles.
        std::function<nlohmann::json(const Node & node)> traverse;
        traverse = [&](const Node & node) {
            nlohmann::json jsonObj2 = json ? json::object() : nlohmann::json(nullptr);
            for (auto & [inputName, input] : node.inputs) {
                if (auto inputNode = std::get_if<0>(&input)) {
                    std::optional<StorePath> storePath;
                    if (!(*inputNode)->lockedRef.input.isRelative()) {
                        storePath = dryRun ? (*inputNode)->lockedRef.input.computeStorePath(*store)
                                           : std::get<StorePath>((*inputNode)->lockedRef.input.fetchToStore(store));
                        sources.insert(*storePath);
                    }
                    if (json) {
                        auto & jsonObj3 = jsonObj2[inputName];
                        if (storePath)
                            jsonObj3["path"] = store->printStorePath(*storePath);
                        jsonObj3["inputs"] = traverse(**inputNode);
                    } else
                        traverse(**inputNode);
                }
            }
            return jsonObj2;
        };

        if (json) {
            nlohmann::json jsonRoot = {
                {"path", store->printStorePath(storePath)},
                {"inputs", traverse(*flake.lockFile.root)},
            };
            printJSON(jsonRoot);
        } else {
            traverse(*flake.lockFile.root);
        }

        if (!dryRun && !dstUri.empty()) {
            ref<Store> dstStore = dstUri.empty() ? openStore() : openStore(dstUri);

            copyPaths(*store, *dstStore, sources, NoRepair, checkSigs, substitute);
        }
    }
};

struct CmdFlakeShow : FlakeCommand, MixJSON, MixFlakeSchemas
{
    bool showLegacy = false;
    bool showAllSystems = false;

    CmdFlakeShow()
    {
        addFlag({
            .longName = "legacy",
            .description = "Show the contents of the `legacyPackages` output.",
            .handler = {&showLegacy, true},
        });
        addFlag({
            .longName = "all-systems",
            .description = "Show the contents of outputs for all systems.",
            .handler = {&showAllSystems, true},
        });
    }

    std::string description() override
    {
        return "show the outputs provided by a flake";
    }

    std::string doc() override
    {
        return
#include "flake-show.md"
            ;
    }

    void run(nix::ref<nix::Store> store) override
    {
        auto state = getEvalState();
        auto flake = std::make_shared<LockedFlake>(lockFlake());
        auto localSystem = std::string(settings.thisSystem.get());

        auto cache = flake_schemas::call(*state, flake, getDefaultFlakeSchemas());

        auto inventory = cache->getRoot()->getAttr("inventory");

        FutureVector futures(*state->executor);

        std::function<void(ref<eval_cache::AttrCursor> node, nlohmann::json & obj)> visit;

        visit = [&](ref<eval_cache::AttrCursor> node, nlohmann::json & obj) {
            flake_schemas::visit(
                showAllSystems ? std::optional<std::string>() : localSystem,
                node,

                [&](ref<eval_cache::AttrCursor> leaf) {
                    obj.emplace("leaf", true);

                    if (auto what = flake_schemas::what(leaf))
                        obj.emplace("what", *what);

                    if (auto shortDescription = flake_schemas::shortDescription(leaf))
                        obj.emplace("shortDescription", *shortDescription);

                    if (auto drv = flake_schemas::derivation(leaf))
                        obj.emplace("derivationName", drv->getAttr(state->sName)->getString());

                    // FIXME: add more stuff
                },

                [&](std::function<void(flake_schemas::ForEachChild)> forEachChild) {
                    auto children = nlohmann::json::object();
                    forEachChild([&](Symbol attrName, ref<eval_cache::AttrCursor> node, bool isLast) {
                        auto j = nlohmann::json::object();
                        try {
                            visit(node, j);
                        } catch (EvalError & e) {
                            // FIXME: make it a flake schema attribute whether to ignore evaluation errors.
                            if (node->root->state.symbols[node->getAttrPath()[0]] == "legacyPackages")
                                j.emplace("failed", true);
                            else
                                throw;
                        }
                        children.emplace(state->symbols[attrName], std::move(j));
                    });
                    obj.emplace("children", std::move(children));
                },

                [&](ref<eval_cache::AttrCursor> node, const std::vector<std::string> & systems) {
                    obj.emplace("filtered", true);
                });
        };

        auto res = nlohmann::json::object();

        flake_schemas::forEachOutput(
            inventory,
            [&](Symbol outputName,
                std::shared_ptr<eval_cache::AttrCursor> output,
                const std::string & doc,
                bool isLast) {
                auto j = nlohmann::json::object();

                if (!showLegacy && state->symbols[outputName] == "legacyPackages") {
                    j.emplace("skipped", true);
                } else if (output) {
                    j.emplace("doc", doc);
                    auto j2 = nlohmann::json::object();
                    visit(ref(output), j2);
                    j.emplace("output", std::move(j2));
                } else
                    j.emplace("unknown", true);

                res.emplace(state->symbols[outputName], j);
            });

        if (json)
            printJSON(res);
        else {

            // Render the JSON into a tree representation.
            std::function<void(nlohmann::json j, const std::string & headerPrefix, const std::string & nextPrefix)>
                render;

            render = [&](nlohmann::json j, const std::string & headerPrefix, const std::string & nextPrefix) {
                auto what = j.find("what");
                auto filtered = j.find("filtered");
                auto derivationName = j.find("derivationName");

                auto s = headerPrefix;

                if (what != j.end())
                    s += fmt(": %s", (std::string) *what);

                if (derivationName != j.end())
                    s += fmt(ANSI_ITALIC " [%s]" ANSI_NORMAL, (std::string) *derivationName);

                if (filtered != j.end() && (bool) *filtered)
                    s += " " ANSI_WARNING "omitted" ANSI_NORMAL " (use '--all-systems' to show)";

                logger->cout(s);

                auto children = j.find("children");

                if (children != j.end()) {
                    for (const auto & [i, child] : enumerate(children->items())) {
                        bool last = i + 1 == children->size();
                        render(
                            child.value(),
                            fmt(ANSI_GREEN "%s%s" ANSI_NORMAL ANSI_BOLD "%s" ANSI_NORMAL,
                                nextPrefix,
                                last ? treeLast : treeConn,
                                child.key()),
                            nextPrefix + (last ? treeNull : treeLine));
                    }
                }
            };

            logger->cout("%s", fmt(ANSI_BOLD "%s" ANSI_NORMAL, flake->flake.lockedRef));

            for (const auto & [i, child] : enumerate(res.items())) {
                bool last = i + 1 == res.size();
                auto nextPrefix = last ? treeNull : treeLine;
                render(
                    child.value()["output"],
                    fmt(ANSI_GREEN "%s%s" ANSI_NORMAL ANSI_BOLD "%s" ANSI_NORMAL,
                        "",
                        last ? treeLast : treeConn,
                        child.key()),
                    nextPrefix);
                if (child.value().contains("unknown"))
                    logger->cout(
                        ANSI_GREEN "%s%s" ANSI_NORMAL ANSI_ITALIC "(unknown flake output)" ANSI_NORMAL,
                        nextPrefix,
                        treeLast);
            }
        }
    }
};

struct CmdFlakePrefetch : FlakeCommand, MixJSON
{
    std::optional<std::filesystem::path> outLink;

    CmdFlakePrefetch()
    {
        addFlag({
            .longName = "out-link",
            .shortName = 'o',
            .description = "Create symlink named *path* to the resulting store path.",
            .labels = {"path"},
            .handler = {&outLink},
            .completer = completePath,
        });
    }

    std::string description() override
    {
        return "download the source tree denoted by a flake reference into the Nix store";
    }

    std::string doc() override
    {
        return
#include "flake-prefetch.md"
            ;
    }

    void run(ref<Store> store) override
    {
        auto originalRef = getFlakeRef();
        auto resolvedRef = originalRef.resolve(store);
        auto [accessor, lockedRef] = resolvedRef.lazyFetch(store);
        auto storePath =
            fetchToStore(getEvalState()->fetchSettings, *store, accessor, FetchMode::Copy, lockedRef.input.getName());
        auto hash = store->queryPathInfo(storePath)->narHash;

        if (json) {
            auto res = nlohmann::json::object();
            res["storePath"] = store->printStorePath(storePath);
            res["hash"] = hash.to_string(HashFormat::SRI, true);
            res["original"] = fetchers::attrsToJSON(resolvedRef.toAttrs());
            res["locked"] = fetchers::attrsToJSON(lockedRef.toAttrs());
            res["locked"].erase("__final"); // internal for now
            printJSON(res);
        } else {
            notice(
                "Downloaded '%s' to '%s' (hash '%s').",
                lockedRef.to_string(),
                store->printStorePath(storePath),
                hash.to_string(HashFormat::SRI, true));
        }

        if (outLink) {
            if (auto store2 = store.dynamic_pointer_cast<LocalFSStore>())
                createOutLinks(*outLink, {BuiltPath::Opaque{storePath}}, *store2);
            else
                throw Error("'--out-link' is not supported for this Nix store");
        }
    }
};

struct CmdFlake : NixMultiCommand
{
    CmdFlake()
        : NixMultiCommand("flake", RegisterCommand::getCommandsFor({"flake"}))
    {
    }

    std::string description() override
    {
        return "manage Nix flakes";
    }

    std::string doc() override
    {
        return
#include "flake.md"
            ;
    }
};

static auto rCmdFlake = registerCommand<CmdFlake>("flake");
static auto rCmdFlakeArchive = registerCommand2<CmdFlakeArchive>({"flake", "archive"});
static auto rCmdFlakeCheck = registerCommand2<CmdFlakeCheck>({"flake", "check"});
static auto rCmdFlakeClone = registerCommand2<CmdFlakeClone>({"flake", "clone"});
static auto rCmdFlakeInfo = registerCommand2<CmdFlakeInfo>({"flake", "info"});
static auto rCmdFlakeInit = registerCommand2<CmdFlakeInit>({"flake", "init"});
static auto rCmdFlakeLock = registerCommand2<CmdFlakeLock>({"flake", "lock"});
static auto rCmdFlakeMetadata = registerCommand2<CmdFlakeMetadata>({"flake", "metadata"});
static auto rCmdFlakeNew = registerCommand2<CmdFlakeNew>({"flake", "new"});
static auto rCmdFlakePrefetch = registerCommand2<CmdFlakePrefetch>({"flake", "prefetch"});
static auto rCmdFlakeShow = registerCommand2<CmdFlakeShow>({"flake", "show"});
static auto rCmdFlakeUpdate = registerCommand2<CmdFlakeUpdate>({"flake", "update"});
