#pragma once

#include "nix/util/provenance.hh"
#include "nix/fetchers/fetchers.hh"

namespace nix {

struct TreeProvenance : Provenance
{
    ref<nlohmann::json> attrs;

    TreeProvenance(const fetchers::Input & input);

    nlohmann::json to_json() const override;
};

} // namespace nix
