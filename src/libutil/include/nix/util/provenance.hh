#pragma once

#include "nix/util/ref.hh"
#include "nix/util/canon-path.hh"

#include <functional>

#include <nlohmann/json_fwd.hpp>

namespace nix {

struct Provenance
{
    static ref<const Provenance> from_json_str(std::string_view);

    static std::shared_ptr<const Provenance> from_json_str_optional(std::string_view);

    static ref<const Provenance> from_json(const nlohmann::json & json);

    std::string to_json_str() const;

    virtual nlohmann::json to_json() const = 0;

protected:

    using ProvenanceFactory = std::function<ref<Provenance>(nlohmann::json)>;

    using RegisteredTypes = std::map<std::string, ProvenanceFactory>;

    static RegisteredTypes & registeredTypes();

public:

    struct Register
    {
        Register(const std::string & type, ProvenanceFactory && factory)
        {
            registeredTypes().insert_or_assign(type, std::move(factory));
        }
    };
};

struct SubpathProvenance : public Provenance
{
    std::shared_ptr<const Provenance> next;
    CanonPath subpath;

    SubpathProvenance(std::shared_ptr<const Provenance> next, const CanonPath & subpath)
        : next(std::move(next))
        , subpath(subpath)
    {
    }

    nlohmann::json to_json() const override;
};

} // namespace nix
