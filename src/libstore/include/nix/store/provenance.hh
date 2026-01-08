#pragma once

#include "nix/util/ref.hh"
#include "nix/store/outputs-spec.hh"
#include "nix/store/path.hh"

#include <functional>

#include <nlohmann/json_fwd.hpp>

namespace nix {

struct Provenance
{
    static ref<const Provenance> from_json_str(std::string_view);

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

struct UnknownProvenance : Provenance
{
    std::string type;
    ref<nlohmann::json> payload;

    UnknownProvenance(std::string_view type, ref<nlohmann::json> payload)
        : type(type)
        , payload(std::move(payload))
    {
    }

    virtual nlohmann::json to_json() const override;
};

struct DerivationProvenance : Provenance
{
    /**
     * The derivation that built this path.
     */
    StorePath drvPath;

    /**
     * The output of the derivation that corresponds to this path.
     */
    OutputName output;

    /**
     * The nested provenance of the derivation.
     */
    std::shared_ptr<const Provenance> provenance;

    // FIXME: do we need anything extra for CA derivations?

    virtual nlohmann::json to_json() const override;
};

struct CopiedProvenance : Provenance
{
    /**
     * Store URL (typically a binary cache) from which this store
     * path was copied.
     */
    std::string from;

    /**
     * Provenance of the store path in the upstream store, if any.
     */
    std::shared_ptr<const Provenance> provenance;

    CopiedProvenance(std::string_view from, std::shared_ptr<const Provenance> provenance)
        : from(from)
        , provenance(std::move(provenance))
    {
    }

    virtual nlohmann::json to_json() const override;
};

} // namespace nix
