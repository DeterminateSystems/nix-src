#include "nix/util/provenance.hh"
#include "nix/util/json-utils.hh"

#include <nlohmann/json.hpp>

namespace nix {

struct UnknownProvenance : Provenance
{
    nlohmann::json payload;

    UnknownProvenance(nlohmann::json payload)
        : payload(std::move(payload))
    {
    }

    nlohmann::json to_json() const override
    {
        return payload;
    }
};

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
        return make_ref<UnknownProvenance>(obj);

    return it->second(obj);
}

std::string Provenance::to_json_str() const
{
    return to_json().dump();
}

nlohmann::json SubpathProvenance::to_json() const
{
    nlohmann::json j{
        {"type", "subpath"},
        {"subpath", subpath.abs()},
    };
    if (next)
        j["next"] = next->to_json();
    return j;
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
