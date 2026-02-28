#pragma once

#include "nix/util/provenance.hh"
#include "nix/store/path.hh"
#include "nix/store/outputs-spec.hh"

namespace nix {

struct BuildProvenance : Provenance
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
     * The hostname of the machine on which the derivation was built, if known.
     */
    std::optional<std::string> buildHost;

    /**
     * User-defined tags from the build host.
     */
    std::map<std::string, std::string> tags;

    /**
     * The system type of the derivation.
     */
    std::string system;

    /**
     * The provenance of the derivation, if known.
     */
    std::shared_ptr<const Provenance> next;

    // FIXME: do we need anything extra for CA derivations?

    BuildProvenance(
        const StorePath & drvPath,
        const OutputName & output,
        std::optional<std::string> buildHost,
        std::map<std::string, std::string> tags,
        std::string system,
        std::shared_ptr<const Provenance> next)
        : drvPath(drvPath)
        , output(output)
        , buildHost(std::move(buildHost))
        , tags(std::move(tags))
        , system(std::move(system))
        , next(std::move(next))
    {
    }

    nlohmann::json to_json() const override;
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
    std::shared_ptr<const Provenance> next;

    CopiedProvenance(std::string_view from, std::shared_ptr<const Provenance> next)
        : from(from)
        , next(std::move(next))
    {
    }

    nlohmann::json to_json() const override;
};

} // namespace nix
