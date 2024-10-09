#pragma once

#include "outputs-spec.hh"
#include "path.hh"

#include "nlohmann/json_fwd.hpp"

namespace nix {

/**
 * This struct describes the provenance of a store path, i.e. a link
 * back to the source code from which the store path was originally
 * built.
 */
struct Provenance
{
    /**
     * Type that denotes a store path that was produced by a
     * derivation.
     */
    struct ProvDerivation
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
    };

    /**
     * Type that denotes a store path that was copied (substituted)
     * from another store.
     */
    struct ProvSubstituted
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
    };

    /**
     * Type that denotes a store path (typically a .drv file or
     * derivation input source) that was produced by the evaluation of
     * a flake.
     */
    struct ProvFlake
    {
        std::string flakeRef;
    };

    using Raw = std::variant<ProvDerivation, ProvSubstituted, ProvFlake>;

    Raw raw;

    Provenance(Raw raw) : raw(std::move(raw))
    { }

    // FIXME: ugly, nlohmann::json wants a default constructor.
    Provenance()
        : raw(ProvFlake{.flakeRef = ""})
    { }

    std::string type() const;
};

void to_json(nlohmann::json & j, const Provenance & p);
void to_json(nlohmann::json & j, const Provenance::ProvDerivation & p);
void to_json(nlohmann::json & j, const Provenance::ProvSubstituted & p);
void to_json(nlohmann::json & j, const Provenance::ProvFlake & p);

void from_json(const nlohmann::json & j, Provenance & p);

}