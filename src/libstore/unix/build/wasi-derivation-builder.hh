#pragma once

#include "nix/store/build/derivation-builder.hh"

namespace nix {

DerivationBuilderUnique makeWasiDerivationBuilder(
    LocalStore & store, std::shared_ptr<DerivationBuilderCallbacks> miscMethods, DerivationBuilderParams params);

}