#include "nix/flake/provenance.hh"
#include "nix/util/json-utils.hh"

#include <nlohmann/json.hpp>

namespace nix {

nlohmann::json FlakeProvenance::to_json() const
{
    return nlohmann::json{
        {"type", "flake"},
        {"next", next ? next->to_json() : nlohmann::json(nullptr)},
        {"flakeOutput", flakeOutput},
        {"pure", pure}};
}

Provenance::Register registerFlakeProvenance("flake", [](nlohmann::json json) {
    auto & obj = getObject(json);
    std::shared_ptr<const Provenance> next;
    if (auto p = optionalValueAt(obj, "next"); p && !p->is_null())
        next = Provenance::from_json(*p);
    bool pure = true;
    if (auto p = optionalValueAt(obj, "pure"))
        pure = getBoolean(*p);
    return make_ref<FlakeProvenance>(next, getString(valueAt(obj, "flakeOutput")), pure);
});

} // namespace nix
