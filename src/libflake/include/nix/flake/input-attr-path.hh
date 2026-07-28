#pragma once
///@file

#include "nix/flake/flakeref.hh"

namespace nix::flake {

typedef std::vector<FlakeId> InputAttrPath;

/**
 * A non-empty input attribute path.
 *
 * Input attribute paths identify inputs in a flake. An empty path would
 * refer to the flake itself rather than an input, which contradicts the
 * purpose of operations like override or update.
 */
class NonEmptyInputAttrPath
{
    InputAttrPath path;

    explicit NonEmptyInputAttrPath(InputAttrPath && p)
        : path(std::move(p))
    {
        assert(!path.empty());
    }

public:
    /**
     * Parse and validate a non-empty input attribute path.
     * Returns std::nullopt if the path is empty.
     */
    static std::optional<NonEmptyInputAttrPath> parse(std::string_view s);

    /**
     * Construct from an already-parsed path.
     * Returns std::nullopt if the path is empty.
     */
    static std::optional<NonEmptyInputAttrPath> make(InputAttrPath path);

    /**
     * Append an element to a path, creating a non-empty path.
     * This is always safe because adding an element guarantees non-emptiness.
     */
    static NonEmptyInputAttrPath append(const InputAttrPath & prefix, const FlakeId & element)
    {
        InputAttrPath path = prefix;
        path.push_back(element);
        return NonEmptyInputAttrPath{std::move(path)};
    }

    const InputAttrPath & get() const
    {
        return path;
    }

    operator const InputAttrPath &() const
    {
        return path;
    }

    /**
     * Get the final component of the path (the input name).
     * For a path like "a/b/c", returns "c".
     */
    const FlakeId & inputName() const
    {
        return path.back();
    }

    /**
     * Get the parent path (all components except the last).
     * For a path like "a/b/c", returns "a/b".
     */
    InputAttrPath parent() const
    {
        InputAttrPath result = path;
        result.pop_back();
        return result;
    }

    auto operator<=>(const NonEmptyInputAttrPath & other) const = default;
};

InputAttrPath parseInputAttrPath(std::string_view s);

std::string printInputAttrPath(const InputAttrPath & path);

} // namespace nix::flake
