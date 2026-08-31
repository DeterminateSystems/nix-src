#pragma once
///@file

#include <string>

namespace nix {

/**
 * The version of Nix itself.
 *
 * This is not `const`, so that the Nix CLI can provide a more detailed version
 * number including the git revision, without having to "re-compile" the entire
 * set of Nix libraries to include that version, even when those libraries are
 * not affected by the change.
 */
extern std::string nixVersion;

/**
 * The Determinate Nix version.
 */
extern const std::string determinateNixVersion;

} // namespace nix
