#pragma once

#include <signal.h>

#include <optional>

namespace nix {
namespace unix {

extern sigset_t savedSignalMask;
extern bool savedSignalMaskIsSet;

/**
 * The disposition of `SIGXFSZ` before we started ignoring it, to be
 * restored (along with the signal mask) before executing another
 * program.
 */
extern std::optional<struct sigaction> savedSigxfszAction;

} // namespace unix
} // namespace nix
