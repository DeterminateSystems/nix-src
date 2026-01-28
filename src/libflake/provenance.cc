#include "nix/flake/provenance.hh"

#include <nlohmann/json.hpp>

namespace nix {

nlohmann::json FlakeProvenance::to_json() const
{
    return nlohmann::json{
        {"type", "flake"},
        {"next", next ? next->to_json() : nlohmann::json(nullptr)},
        {"flakeOutput", flakeOutput},
    };
}

} // namespace nix
