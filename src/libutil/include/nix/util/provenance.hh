#pragma once

#include "nix/util/ref.hh"
#include "nix/util/canon-path.hh"

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

#if 0
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
    std::shared_ptr<const Provenance> next;

    // FIXME: do we need anything extra for CA derivations?

    nlohmann::json to_json() const override;
};
#endif

// FIXME: move to libstore
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
    std::shared_ptr<const Provenance> next;

    CopiedProvenance(std::string_view from, std::shared_ptr<const Provenance> next)
        : from(from)
        , next(std::move(next))
    {
    }

    nlohmann::json to_json() const override;
};

} // namespace nix
