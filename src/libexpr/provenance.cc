#include "nix/expr/provenance.hh"
#include "nix/util/json-utils.hh"

#include <nlohmann/json.hpp>

namespace nix {

nlohmann::json DerivationProvenance::to_json() const
{
    return nlohmann::json{
        {"type", "derivation"},
        {"meta", *meta},
        {"next", next ? next->to_json() : nlohmann::json(nullptr)},
    };
}

Provenance::Register registerDerivationProvenance("derivation", [](nlohmann::json json) {
    auto & obj = getObject(json);
    std::shared_ptr<const Provenance> next;
    if (auto p = optionalValueAt(obj, "next"); p && !p->is_null())
        next = Provenance::from_json(*p);
    return make_ref<DerivationProvenance>(next, make_ref<nlohmann::json>(valueAt(obj, "meta")));
});

} // namespace nix
