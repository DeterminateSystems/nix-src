#include "nix/cmd/command-installable-value.hh"
#include "nix/main/common-args.hh"
#include "nix/main/shared.hh"
#include "nix/store/store-api.hh"
#include "nix/store/local-fs-store.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/value-to-json.hh"

#include <nlohmann/json.hpp>

namespace nix {

struct CmdEval : MixJSON, InstallableValueCommand, MixReadOnlyOption
{
    bool raw = false;
    std::optional<std::string> apply;
    std::optional<std::filesystem::path> writeTo;
    std::optional<std::filesystem::path> drvLink;

    CmdEval()
        : InstallableValueCommand()
    {
        addFlag({
            .longName = "raw",
            .description = "Print strings without quotes or escaping.",
            .handler = {&raw, true},
        });

        addFlag({
            .longName = "apply",
            .description = "Apply the function *expr* to each argument.",
            .labels = {"expr"},
            .handler = {&apply},
        });

        addFlag({
            .longName = "write-to",
            .description = "Write a string or attrset of strings to *path*.",
            .labels = {"path"},
            .handler = {&writeTo},
        });

        addFlag({
            .longName = "drv-link",
            .description =
                "Use *path* as prefix for symlinks to derivations produced by the evaluation at top-level. By default, no symlinks are created.",
            .labels = {"path"},
            .handler = {&drvLink},
            .completer = completePath,
        });
    }

    std::string description() override
    {
        return "evaluate a Nix expression";
    }

    std::string doc() override
    {
        return
#include "eval.md"
            ;
    }

    Category category() override
    {
        return catSecondary;
    }

    void run(ref<Store> store, ref<InstallableValue> installable) override
    {
        if (raw && json)
            throw UsageError("--raw and --json are mutually exclusive");

        if (drvLink && !raw && !json)
            throw UsageError("--drv-link requires --raw or --json");

        auto state = getEvalState();

        auto [v, pos] = installable->toValue(*state);
        NixStringContext context;

        if (apply) {
            auto vApply = state->allocValue();
            state->eval(state->parseExprFromString(*apply, state->rootPath(".")), *vApply);
            auto vRes = state->allocValue();
            state->callFunction(*vApply, *v, *vRes, noPos);
            v = vRes;
        }

        if (writeTo) {
            logger->stop();

            if (pathExists(*writeTo))
                throw Error("path '%s' already exists", writeTo->string());

            [&](this const auto & recurse, Value & v, const PosIdx pos, const std::filesystem::path & path) -> void {
                state->forceValue(v, pos);
                if (v.type() == nString)
                    // FIXME: disallow strings with contexts?
                    writeFile(path, v.string_view());
                else if (v.type() == nAttrs) {
                    [[maybe_unused]] bool directoryCreated = std::filesystem::create_directory(path);
                    // Directory should not already exist
                    assert(directoryCreated);
                    for (auto & attr : *v.attrs()) {
                        std::string_view name = state->symbols[attr.name];
                        try {
                            if (name == "." || name == "..")
                                throw Error("invalid file name '%s'", name);
                            recurse(*attr.value, attr.pos, path / name);
                        } catch (Error & e) {
                            e.addTrace(
                                state->positions[attr.pos], HintFmt("while evaluating the attribute '%s'", name));
                            throw;
                        }
                    }
                } else
                    state->error<TypeError>("value at '%s' is not a string or an attribute set", state->positions[pos])
                        .debugThrow();
            }(*v, pos, *writeTo);
        }

        else if (raw) {
            logger->stop();
            writeFull(
                getStandardOutput(),
                state->devirtualize(
                    *state->coerceToString(noPos, *v, context, "while generating the eval command output"), context));
        }

        else if (json) {
            // FIXME: use printJSON
            auto j = printValueAsJSON(*state, true, *v, pos, context, false);
            logger->cout("%s", state->devirtualize(outputPretty ? j.dump(2) : j.dump(), context));
        }

        else {
            logger->cout("%s", ValuePrinter(*state, *v, PrintOptions{.force = true, .derivationPaths = true}));
        }

        if (drvLink) {
            // Create out links for the store paths in `context`. Similar to createOutLinks(), except that for
            // derivations, we create symlinks to the .drv files rather than the outputs (since we didn't build
            // anything).
            if (auto store2 = store.dynamic_pointer_cast<LocalFSStore>()) {
                std::set<StorePath> roots;
                for (auto & c : context)
                    std::visit(
                        overloaded{
                            [&](const NixStringContextElem::Opaque & o) { roots.insert(state->devirtualize(o.path)); },
                            [&](const NixStringContextElem::DrvDeep & d) { roots.insert(d.drvPath); },
                            [&](const NixStringContextElem::Built & b) { roots.insert(b.drvPath->getBaseStorePath()); },
                            [&](const NixStringContextElem::Path &) {},
                        },
                        c.raw);
                for (const auto & [i, path] : enumerate(roots)) {
                    auto symlink = *drvLink;
                    if (i)
                        symlink += fmt("-%d", i);
                    state->waitForPath(path);
                    store2->addPermRoot(path, absPath(symlink).string());
                }
            }
        }
    }
};

static auto rCmdEval = registerCommand<CmdEval>("eval");

} // namespace nix
