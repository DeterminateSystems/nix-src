#pragma once

#include "nix/util/provenance.hh"
#include "nix/fetchers/fetchers.hh"

namespace nix {

struct TreeProvenance : Provenance
{
    ref<nlohmann::json> attrs;

    TreeProvenance(const fetchers::Input & input);

    TreeProvenance(ref<nlohmann::json> attrs)
        : attrs(std::move(attrs))
    {
    }

    nlohmann::json to_json() const override;
};

struct FetchurlProvenance : Provenance
{
    std::string url;

    FetchurlProvenance(std::string url, bool sanitize = true);

    nlohmann::json to_json() const override;
};

} // namespace nix
