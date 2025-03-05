#pragma once

namespace nix::flake_schemas {

struct PackageOption
{
    FlakeRef flakeRef;
    std::string fragment;
};

// FIXME: generalize list support.
using Option = std::variant<std::string, Explicit<bool>, PackageOption, std::vector<PackageOption>, NixInt>;

using Options = std::map<std::string, Option>;

}
