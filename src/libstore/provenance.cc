#include "nix/store/provenance.hh"
#include "nix/util/json-utils.hh"

namespace nix {

nlohmann::json DerivationProvenance::to_json() const
{
    return nlohmann::json{
        {"type", "derivation"},
        {"drv", drvPath.to_string()},
        {"output", output},
        {"next", next ? next->to_json() : nlohmann::json(nullptr)},
    };
}

nlohmann::json CopiedProvenance::to_json() const
{
    nlohmann::json j{
        {"type", "copied"},
        {"from", from},
    };
    if (next)
        j["next"] = next->to_json();
    return j;
}

Provenance::Register registerCopiedProvenance("copied", [](nlohmann::json json) {
    auto & obj = getObject(json);
    std::shared_ptr<const Provenance> next;
    if (auto prov = optionalValueAt(obj, "next"))
        next = Provenance::from_json(*prov);
    return make_ref<CopiedProvenance>(getString(valueAt(obj, "from")), next);
});

} // namespace nix
