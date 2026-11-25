#pragma once

#include "nix/util/fun.hh"

namespace nix {

extern fun<void(const char *, const char *)> setSentryTag;

}
