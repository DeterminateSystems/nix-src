#pragma once
///@file

#include "nix/util/logging.hh"
#include "nix/util/ref.hh"

namespace nix {

struct Store;

std::unique_ptr<Logger> makeProgressBar();

/**
 * Create a multi-line progress indicator (`--log-format tui`).
 */
std::unique_ptr<Logger> makeProgressIndicator();

/**
 * Tell the progress indicator which store is being used, so that it
 * can query it for information about active builds. The progress
 * indicator only keeps a weak reference to the store.
 */
void setProgressBarStore(ref<Store> store);

} // namespace nix
