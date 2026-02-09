#include "nix/store/provenance.hh"
#include "nix/util/json-utils.hh"

namespace nix {

nlohmann::json BuildProvenance::to_json() const
{
    return {
        {"type", "build"},
        {"drv", drvPath.to_string()},
        {"output", output},
        {"next", next ? next->to_json() : nlohmann::json(nullptr)},
    };
}

Provenance::Register registerBuildProvenance("build", [](nlohmann::json json) {
    auto & obj = getObject(json);
    std::shared_ptr<const Provenance> next;
    if (auto p = optionalValueAt(obj, "next"); p && !p->is_null())
        next = Provenance::from_json(*p);
    return make_ref<BuildProvenance>(
        StorePath(getString(valueAt(obj, "drv"))), getString(valueAt(obj, "output")), next);
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
