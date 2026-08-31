#include "nix/util/version.hh"

#include "util-config-private.hh"

namespace nix {

std::string nixVersion = PACKAGE_VERSION;

const std::string determinateNixVersion = DETERMINATE_NIX_VERSION;

} // namespace nix
