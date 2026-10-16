#pragma once
///@file

#include "nix/util/logging.hh"

namespace nix {

std::unique_ptr<Logger> makeProgressBar();

/**
 * Create a multi-line progress indicator (`--log-format tui`).
 */
std::unique_ptr<Logger> makeProgressIndicator();

} // namespace nix
