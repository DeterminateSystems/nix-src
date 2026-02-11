#pragma once

#include "nix/util/provenance.hh"

namespace nix {

struct FlakeProvenance : Provenance
{
    std::shared_ptr<const Provenance> next;
    std::string flakeOutput;

    FlakeProvenance(std::shared_ptr<const Provenance> next, std::string flakeOutput)
        : next(std::move(next))
        , flakeOutput(std::move(flakeOutput)) {};

    nlohmann::json to_json() const override;
};

} // namespace nix
