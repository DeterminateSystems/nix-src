#include "nix/util/provenance.hh"
#include "nix/util/json-utils.hh"

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

std::shared_ptr<const Provenance> Provenance::from_json_str_optional(std::string_view s)
{
    if (s.empty())
        return nullptr;
    return Provenance::from_json_str(s);
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
    return {
        {"type", "subpath"},
        {"subpath", subpath.abs()},
        {"next", next ? next->to_json() : nlohmann::json(nullptr)},
    };
}

} // namespace nix
