#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include "nix/flake/input-attr-path.hh"
#include "nix/flake/flakeref.hh"
#include "nix/util/error.hh"
#include "nix/util/strings.hh"

namespace nix::flake {

InputAttrPath parseInputAttrPath(std::string_view s)
{
    InputAttrPath path;

    for (auto & elem : tokenizeString<std::vector<std::string>>(s, "/")) {
        if (!std::regex_match(elem, flakeIdRegex))
            throw UsageError("invalid flake input attribute path element '%s'", elem);
        path.push_back(elem);
    }

    return path;
}

std::string printInputAttrPath(const InputAttrPath & path)
{
    return concatStringsSep("/", path);
}

std::optional<NonEmptyInputAttrPath> NonEmptyInputAttrPath::parse(std::string_view s)
{
    auto path = parseInputAttrPath(s);
    return make(std::move(path));
}

std::optional<NonEmptyInputAttrPath> NonEmptyInputAttrPath::make(InputAttrPath path)
{
    if (path.empty())
        return std::nullopt;
    return NonEmptyInputAttrPath{std::move(path)};
}

} // namespace nix::flake
