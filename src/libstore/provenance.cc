#include "nix/store/provenance.hh"
#include "nix/util/json-utils.hh"

#include <nlohmann/json.hpp>

namespace nix {

Provenance::RegisteredTypes & Provenance::registeredTypes()
{
    static Provenance::RegisteredTypes types;
    return types;
}

ref<const Provenance> Provenance::from_json_str(std::string_view s)
{
    return from_json(nlohmann::json::parse(s));
}

ref<const Provenance> Provenance::from_json(const nlohmann::json & json)
{
    auto & obj = getObject(json);

    auto type = getString(valueAt(obj, "type"));

    auto it = registeredTypes().find(type);
    if (it == registeredTypes().end())
        return make_ref<UnknownProvenance>(type, make_ref<nlohmann::json>(obj));

    return it->second(obj);
}

std::string Provenance::to_json_str() const
{
    return to_json().dump();
}

nlohmann::json UnknownProvenance::to_json() const
{
    return *payload;
}

nlohmann::json CopiedProvenance::to_json() const
{
    nlohmann::json j{
        {"type", "copied"},
        {"from", from},
    };
    if (provenance)
        j["provenance"] = provenance->to_json();
    return j;
}

Provenance::Register registerCopiedProvenance("copied", [](nlohmann::json json) {
    auto & obj = getObject(json);
    std::shared_ptr<const Provenance> provenance;
    if (auto prov = optionalValueAt(obj, "provenance"))
        provenance = Provenance::from_json(*prov);
    return make_ref<CopiedProvenance>(getString(valueAt(obj, "from")), provenance);
});

} // namespace nix
