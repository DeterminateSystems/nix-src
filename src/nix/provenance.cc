#include "nix/cmd/command.hh"
#include "nix/store/store-api.hh"
#include "nix/expr/provenance.hh"
#include "nix/store/provenance.hh"
#include "nix/flake/provenance.hh"
#include "nix/fetchers/provenance.hh"
#include "nix/util/provenance.hh"
#include "nix/util/json-utils.hh"

#include <memory>
#include <nlohmann/json.hpp>

#define TAB "    "

using namespace nix;

struct CmdProvenance : NixMultiCommand
{
    CmdProvenance()
        : NixMultiCommand("provenance", RegisterCommand::getCommandsFor({"provenance"}))
    {
    }

    std::string description() override
    {
        return "query and check the provenance of store paths";
    }

    std::optional<ExperimentalFeature> experimentalFeature() override
    {
        return Xp::Provenance;
    }

    Category category() override
    {
        return catUtility;
    }
};

static auto rCmdProvenance = registerCommand<CmdProvenance>("provenance");

struct CmdProvenanceShow : StorePathsCommand
{
    std::string description() override
    {
        return "show the provenance chain of store paths";
    }

    std::string doc() override
    {
        return
#include "provenance-show.md"
            ;
    }

    void displayProvenance(Store & store, const StorePath & path, std::shared_ptr<const Provenance> provenance)
    {
        while (provenance) {
            if (auto copied = std::dynamic_pointer_cast<const CopiedProvenance>(provenance)) {
                logger->cout("← copied from " ANSI_BOLD "%s" ANSI_NORMAL, copied->from);
                provenance = copied->next;
            } else if (auto build = std::dynamic_pointer_cast<const BuildProvenance>(provenance)) {
                logger->cout(
                    "← built from derivation " ANSI_BOLD "%s" ANSI_NORMAL " (output " ANSI_BOLD "%s" ANSI_NORMAL
                    ") on " ANSI_BOLD "%s" ANSI_NORMAL " for " ANSI_BOLD "%s" ANSI_NORMAL,
                    store.printStorePath(build->drvPath),
                    build->output,
                    build->buildHost.value_or("unknown host").c_str(),
                    build->system);
                provenance = build->next;
            } else if (auto flake = std::dynamic_pointer_cast<const FlakeProvenance>(provenance)) {
                // Collapse subpath/tree provenance into the flake provenance for legibility.
                auto next = flake->next;
                CanonPath flakePath("/flake.nix");
                if (auto subpath = std::dynamic_pointer_cast<const SubpathProvenance>(next)) {
                    next = subpath->next;
                    flakePath = subpath->subpath;
                }
                if (auto tree = std::dynamic_pointer_cast<const TreeProvenance>(next)) {
                    FlakeRef flakeRef(
                        fetchers::Input::fromAttrs(fetchSettings, fetchers::jsonToAttrs(*tree->attrs)),
                        Path(flakePath.parent().value_or(CanonPath::root).rel()));
                    logger->cout(
                        "← %sinstantiated from %sflake output " ANSI_BOLD "%s#%s" ANSI_NORMAL,
                        flake->pure ? "" : ANSI_RED "impurely" ANSI_NORMAL " ",
                        flakeRef.input.isLocked(fetchSettings) ? "" : ANSI_RED "unlocked" ANSI_NORMAL " ",
                        flakeRef.to_string(),
                        flake->flakeOutput);
                    break;
                } else {
                    logger->cout("← instantiated from flake output " ANSI_BOLD "%s" ANSI_NORMAL, flake->flakeOutput);
                    provenance = flake->next;
                }
            } else if (auto tree = std::dynamic_pointer_cast<const TreeProvenance>(provenance)) {
                auto input = fetchers::Input::fromAttrs(fetchSettings, fetchers::jsonToAttrs(*tree->attrs));
                logger->cout(
                    "← from %stree " ANSI_BOLD "%s" ANSI_NORMAL,
                    input.isLocked(fetchSettings) ? "" : ANSI_RED "unlocked" ANSI_NORMAL " ",
                    input.to_string());
                break;
            } else if (auto subpath = std::dynamic_pointer_cast<const SubpathProvenance>(provenance)) {
                logger->cout("← from file " ANSI_BOLD "%s" ANSI_NORMAL, subpath->subpath.abs());
                provenance = subpath->next;
            } else if (auto drv = std::dynamic_pointer_cast<const DerivationProvenance>(provenance)) {
                logger->cout("← with derivation metadata");
                auto json = getObject(*(drv->meta));
                if (auto identifiers = optionalValueAt(json, "identifiers")) {
                    auto ident = getObject(*identifiers);
                    if (auto cpeParts = optionalValueAt(ident, "cpeParts")) {
                        auto parts = getObject(*cpeParts);

                        auto vendor = parts["vendor"];
                        auto product = parts["product"];
                        auto version = parts["version"];
                        auto update = parts["update"];

                        logger->cout(
                            TAB "" ANSI_BOLD "CPE:" ANSI_NORMAL " cpe:2.3:a:%s:%s:%s:%s:*:*:*:*:*:*",
                            vendor.is_null() ? "*" : vendor.get<std::string>(),
                            product.is_null() ? "*" : product.get<std::string>(),
                            version.is_null() ? "*" : version.get<std::string>(),
                            update.is_null() ? "*" : update.get<std::string>());
                    }
                }
                if (auto license = optionalValueAt(json, "license")) {
                    if (license->is_array()) {
                        logger->cout(TAB "" ANSI_BOLD "Licenses:" ANSI_NORMAL);
                        auto licenses = getArray(*license);
                        for (auto it = licenses.begin(); it != licenses.end(); it++) {
                            auto license = getObject(*it);
                            auto shortName = license["shortName"];
                            logger->cout(TAB "" TAB "- %s", shortName.get<std::string>());
                        }
                    } else {
                        auto obj = getObject(*license);
                        auto shortName = obj["shortName"];
                        logger->cout(TAB "" ANSI_BOLD "License:" ANSI_NORMAL " %s", shortName.get<std::string>());
                    }
                }
                if (auto licenses = optionalValueAt(json, "licenses")) {
                    if (licenses->is_array()) {
                        logger->cout(TAB "" ANSI_BOLD "Licenses:" ANSI_NORMAL);
                        auto licensesArray = getArray(*licenses);
                        for (auto it = licensesArray.begin(); it != licensesArray.end(); it++) {
                            auto license = getObject(*it);
                            auto shortName = license["shortName"];
                            logger->cout(TAB "" TAB "- %s", shortName.get<std::string>());
                        }
                    } else {
                        auto license = getObject(*licenses);
                        auto shortName = license["shortName"];
                        logger->cout(TAB "" ANSI_BOLD "License:" ANSI_NORMAL " %s", shortName.get<std::string>());
                    }
                }
                provenance = drv->next;
            } else {
                // Unknown or unhandled provenance type
                auto json = provenance->to_json();
                auto typeIt = json.find("type");
                if (typeIt != json.end() && typeIt->is_string())
                    logger->cout("← " ANSI_RED "unknown provenance type '%s'" ANSI_NORMAL, typeIt->get<std::string>());
                else
                    logger->cout("← " ANSI_RED "unknown provenance type" ANSI_NORMAL);
                break;
            }
        }
    }

    void run(ref<Store> store, StorePaths && storePaths) override
    {
        bool first = true;

        for (auto & storePath : storePaths) {
            auto info = store->queryPathInfo(storePath);
            if (!first)
                logger->cout("");
            first = false;
            logger->cout(ANSI_BOLD "%s" ANSI_NORMAL, store->printStorePath(info->path));

            if (info->provenance)
                displayProvenance(*store, storePath, info->provenance);
            else
                logger->cout(ANSI_RED "  (no provenance information available)" ANSI_NORMAL);
        }
    }
};

static auto rCmdProvenanceShow = registerCommand2<CmdProvenanceShow>({"provenance", "show"});
