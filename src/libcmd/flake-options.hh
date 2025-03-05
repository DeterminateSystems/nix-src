#pragma once

namespace nix::flake_schemas {

struct PackageOption
{
    FlakeRef flakeRef;
    std::string fragment;
};

using Option = std::variant<std::string, Explicit<bool>, PackageOption, NixInt>;

using Options = std::map<std::string, Option>;

}
