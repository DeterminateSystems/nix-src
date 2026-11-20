#include <nlohmann/json.hpp>

#include "nix/store/store-dir-config.hh"
#include "nix/util/json-utils.hh"

namespace nix {

void BadStorePathName::anchor() {}

void BadStorePath::anchor() {}

static bool isValidNameChar(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '+' || c == '-'
           || c == '.' || c == '_' || c == '?' || c == '=';
}

static bool isValidHashChar(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z' && c != 'e' && c != 'o' && c != 'u' && c != 't');
}

std::expected<void, BadStorePathReason> validateStorePathName(std::string_view name)
{
    using enum BadStorePathReason;
    if (name.empty())
        return std::unexpected(NameEmpty);
    if (name.size() > StorePath::MaxPathLen)
        return std::unexpected(NameTooLong);
    // See nameRegexStr for the definition
    if (name[0] == '.') {
        // check against "." and "..", followed by end or dash
        if (name.size() == 1)
            return std::unexpected(NameReserved);
        if (name[1] == '-')
            return std::unexpected(NameReservedComponent);
        if (name[1] == '.') {
            if (name.size() == 2)
                return std::unexpected(NameReserved);
            if (name[2] == '-')
                return std::unexpected(NameReservedComponent);
        }
    }
    for (auto c : name)
        if (!isValidNameChar(c))
            return std::unexpected(NameIllegalCharacter);
    return {};
}

/**
 * Throw the `BadStorePathName` error corresponding to a failed
 * `validateStorePathName()`.
 */
[[noreturn]] static void throwBadName(BadStorePathReason reason, std::string_view name)
{
    using enum BadStorePathReason;
    switch (reason) {
    case NameEmpty:
        throw BadStorePathName("name must not be empty");
    case NameTooLong:
        throw BadStorePathName("name '%s' must be no longer than %d characters", name, StorePath::MaxPathLen);
    case NameReserved:
        throw BadStorePathName("name '%s' is not valid", name);
    case NameReservedComponent:
        throw BadStorePathName(
            "name '%s' is not valid: first dash-separated component must not be '%s'",
            name,
            name.starts_with("..") ? ".." : ".");
    case NameIllegalCharacter:
        for (auto c : name)
            if (!isValidNameChar(c))
                throw BadStorePathName("name '%s' contains illegal character '%s'", name, c);
        break;
    case TooShort:
    case NoDashAfterHash:
    case IllegalHashCharacter:
    case EmptyPath:
    case NotInStore:
        break;
    }
    unreachable();
}

void checkName(std::string_view name)
{
    if (auto res = validateStorePathName(name); !res)
        throwBadName(res.error(), name);
}

[[noreturn]] static void throwBadPathName(BadStorePathReason reason, std::string_view path, std::string_view name)
{
    try {
        throwBadName(reason, name);
    } catch (BadStorePathName & e) {
        throw BadStorePath("path '%s' is not a valid store path: %s", path, Uncolored(e.message()));
    }
}

static std::expected<void, BadStorePathReason> validateBaseName(std::string_view baseName)
{
    using enum BadStorePathReason;
    if (baseName.size() < StorePath::HashLen + 1)
        return std::unexpected(TooShort);
    if (baseName[StorePath::HashLen] != '-')
        return std::unexpected(NoDashAfterHash);
    for (auto c : baseName.substr(0, StorePath::HashLen))
        if (!isValidHashChar(c))
            return std::unexpected(IllegalHashCharacter);
    return validateStorePathName(baseName.substr(StorePath::HashLen + 1));
}

/**
 * Throw the `BadStorePath` error corresponding to a failed
 * `validateBaseName()`.
 */
[[noreturn]] static void throwBadBaseName(BadStorePathReason reason, std::string_view baseName)
{
    using enum BadStorePathReason;
    switch (reason) {
    case TooShort:
        throw BadStorePath("'%s' is too short to be a valid store path", baseName);
    case NoDashAfterHash:
        throw BadStorePath("'%s' can't name a store path because the hash part is not followed by a '-'", baseName);
    case IllegalHashCharacter:
        for (auto c : baseName.substr(0, StorePath::HashLen))
            if (!isValidHashChar(c))
                throw BadStorePath("store path '%s' contains illegal base-32 character '%s'", baseName, c);
        break;
    case NameEmpty:
    case NameTooLong:
    case NameReserved:
    case NameReservedComponent:
    case NameIllegalCharacter:
        throwBadPathName(reason, baseName, baseName.substr(StorePath::HashLen + 1));
    case EmptyPath:
    case NotInStore:
        break;
    }
    unreachable();
}

StorePath::StorePath(std::string_view _baseName)
    : baseName(_baseName)
{
    if (auto res = validateBaseName(baseName); !res)
        throwBadBaseName(res.error(), baseName);
}

std::expected<StorePath, BadStorePathReason> StorePath::parse(std::string_view baseName)
{
    if (auto res = validateBaseName(baseName); !res)
        return std::unexpected(res.error());
    return StorePath(baseName, Unchecked{});
}

StorePath::StorePath(const Hash & hash, std::string_view _name)
    : baseName((hash.to_string(HashFormat::Nix32, false) + "-").append(std::string(_name)))
{
    if (auto res = validateStorePathName(name()); !res)
        throwBadPathName(res.error(), baseName, name());
}

bool StorePath::isDerivation() const noexcept
{
    return hasSuffix(name(), drvExtension);
}

void StorePath::requireDerivation() const
{
    if (!isDerivation())
        throw FormatError("store path '%s' is not a valid derivation path", to_string());
}

StorePath StorePath::dummy("ffffffffffffffffffffffffffffffff-x");

StorePath StorePath::random(std::string_view name)
{
    return StorePath(Hash::random(HashAlgorithm::SHA1), name);
}

} // namespace nix

namespace nlohmann {

using namespace nix;

StorePath adl_serializer<StorePath>::from_json(const json & json)
{
    return StorePath{getString(json)};
}

void adl_serializer<StorePath>::to_json(json & json, const StorePath & storePath)
{
    json = storePath.to_string();
}

} // namespace nlohmann
