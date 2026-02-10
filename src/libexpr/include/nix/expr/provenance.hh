#pragma once

#include "nix/util/provenance.hh"

namespace nix {

/**
 * Provenance indicating that this store path was instantiated by the `derivation` builtin function. Its main purpose is
 * to record `meta` fields.
 */
struct MetaProvenance : Provenance
{
    std::shared_ptr<const Provenance> next;
    ref<nlohmann::json> meta;

    MetaProvenance(std::shared_ptr<const Provenance> next, ref<nlohmann::json> meta)
        : next(std::move(next))
        , meta(std::move(meta)) {};

    nlohmann::json to_json() const override;
};

} // namespace nix
