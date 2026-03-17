#include "nix/store/provenance.hh"
#include "nix/util/json-utils.hh"

#include <regex>

namespace nix {

static void checkProvenanceTagName(std::string_view name)
{
    static const std::regex tagNameRegex("^[A-Za-z_][A-Za-z0-9_+\\-]*$");
    if (!std::regex_match(name.begin(), name.end(), tagNameRegex))
        throw Error("tag name '%s' is invalid", name);
}

BuildProvenance::BuildProvenance(
    const StorePath & drvPath,
    const OutputName & output,
    std::optional<std::string> buildHost,
    std::map<std::string, std::string> tags,
    std::string system,
    std::shared_ptr<const Provenance> next)
    : drvPath(drvPath)
    , output(output)
    , buildHost(std::move(buildHost))
    , tags(std::move(tags))
    , system(std::move(system))
    , next(std::move(next))
{
    for (const auto & [name, value] : this->tags)
        checkProvenanceTagName(name);
}

nlohmann::json BuildProvenance::to_json() const
{
    return {
        {"type", "build"},
        {"drv", drvPath.to_string()},
        {"output", output},
        {"buildHost", buildHost},
        {"system", system},
        {"next", next ? next->to_json() : nlohmann::json(nullptr)},
        {"tags", tags},
    };
}

Provenance::Register registerBuildProvenance("build", [](nlohmann::json json) {
    auto & obj = getObject(json);
    std::shared_ptr<const Provenance> next;
    if (auto p = optionalValueAt(obj, "next"); p && !p->is_null())
        next = Provenance::from_json(*p);
    std::optional<std::string> buildHost;
    if (auto p = optionalValueAt(obj, "buildHost"))
        buildHost = p->get<std::optional<std::string>>();
    std::map<std::string, std::string> tags;
    if (auto p = optionalValueAt(obj, "tags"); p && !p->is_null())
        tags = p->get<std::map<std::string, std::string>>();
    auto buildProv = make_ref<BuildProvenance>(
        StorePath(getString(valueAt(obj, "drv"))),
        getString(valueAt(obj, "output")),
        std::move(buildHost),
        std::move(tags),
        getString(valueAt(obj, "system")),
        next);
    return buildProv;
});

nlohmann::json CopiedProvenance::to_json() const
{
    return {
        {"type", "copied"},
        {"from", from},
        {"next", next ? next->to_json() : nlohmann::json(nullptr)},
    };
}

Provenance::Register registerCopiedProvenance("copied", [](nlohmann::json json) {
    auto & obj = getObject(json);
    std::shared_ptr<const Provenance> next;
    if (auto p = optionalValueAt(obj, "next"); p && !p->is_null())
        next = Provenance::from_json(*p);
    return make_ref<CopiedProvenance>(getString(valueAt(obj, "from")), next);
});

} // namespace nix
