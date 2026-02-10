#include "nix/store/provenance.hh"
#include "nix/util/json-utils.hh"

namespace nix {

nlohmann::json BuildProvenance::to_json() const
{
    return {
        {"type", "build"},
        {"drv", drvPath.to_string()},
        {"output", output},
        {"buildHost", buildHost},
        {"next", next ? next->to_json() : nlohmann::json(nullptr)},
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
    auto buildProv = make_ref<BuildProvenance>(
        StorePath(getString(valueAt(obj, "drv"))), getString(valueAt(obj, "output")), buildHost, next);
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
