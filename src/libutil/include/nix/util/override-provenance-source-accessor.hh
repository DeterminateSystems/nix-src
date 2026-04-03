#pragma once

#include "nix/util/forwarding-source-accessor.hh"

namespace nix {

struct OverrideProvenanceSourceAccessor : ForwardingSourceAccessor
{
    OverrideProvenanceSourceAccessor(ref<SourceAccessor> next, std::shared_ptr<const Provenance> provenance)
        : ForwardingSourceAccessor(std::move(next))
    {
        this->provenance = std::move(provenance);
    }

    std::shared_ptr<const Provenance> getProvenance(const CanonPath & path) override
    {
        return provenance;
    }
};

} // namespace nix
