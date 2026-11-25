#pragma once

#include "nix/util/provenance.hh"

namespace nix {

struct FlakeProvenance : Provenance
{
    std::shared_ptr<const Provenance> next;
    std::string flakeOutput;
    bool pure = true;

    FlakeProvenance(std::shared_ptr<const Provenance> next, std::string flakeOutput, bool pure)
        : next(std::move(next))
        , flakeOutput(std::move(flakeOutput))
        , pure(pure) {};

    nlohmann::json to_json() const override;
};

} // namespace nix
