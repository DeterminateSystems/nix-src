#pragma once

#include "nix/store/build/derivation-builder.hh"

namespace nix {

DerivationBuilderUnique makeWasiDerivationBuilder(
    std::shared_ptr<BuildingStore> store,
    std::shared_ptr<DerivationBuilderCallbacks> miscMethods,
    DerivationBuilderParams params);

}