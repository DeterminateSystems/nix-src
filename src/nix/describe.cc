#include "command.hh"
#include "installable-flake.hh"
#include "flake-schemas.hh"
#include "markdown.hh"

using namespace nix;

struct CmdDescribe : InstallablesCommand
{
    std::string description() override
    {
        return "give information about an installable";
    }

    void run(ref<Store> store, Installables && installables) override
    {
        auto state = getEvalState();

        for (auto & _installable : installables) {
            auto installable = _installable.dynamic_pointer_cast<InstallableFlake>();
            if (!installable)
                throw Error("'nix describe' currently does not support installables '%s'", _installable->what());

            auto cache = installable->openEvalCache();

            auto inventory = cache->getRoot()->getAttr("inventory");

            auto cursor = installable->getCursor(*state);

            logger->cout(ANSI_BOLD "   Installable: " ANSI_NORMAL "%s", installable->what());
            logger->cout(ANSI_BOLD "Attribute path: " ANSI_NORMAL "%s", cursor->getAttrPathStr());

            auto outputInfo = flake_schemas::getOutput(inventory, cursor->getAttrPath());
            if (outputInfo) {
                if (auto what = flake_schemas::what(outputInfo->nodeInfo))
                    logger->cout(ANSI_BOLD "          What: " ANSI_NORMAL "%s", *what);

                if (auto description = flake_schemas::shortDescription(outputInfo->nodeInfo))
                    logger->cout(ANSI_BOLD "   Description: " ANSI_NORMAL "%s", *description);

                if (auto options = outputInfo->nodeInfo->maybeGetAttr("options")) {
                    logger->cout(ANSI_BOLD "     Arguments:" ANSI_NORMAL);
                    for (auto & optionName : options->getAttrs()) {
                        auto option = options->getAttr(optionName);
                        logger->cout(
                            "  - %s (%s): %s",
                            state->symbols[optionName],
                            option->getAttr("type")->getAttr("description")->getString(),
                            option->getAttr("description")->getString());
                    }
                }

#if 0
                logger->cout(
                    ANSI_BOLD "Output schema docs:" ANSI_NORMAL "\n%s",
                    renderMarkdownToTerminal(outputInfo->schemaInfo->getAttr("doc")->getString()));
#endif
            }
        }
    }
};

static auto rCmdDescribe = registerCommand<CmdDescribe>("describe");
