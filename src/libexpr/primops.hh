#pragma once

#include "expr-config-private.hh"

namespace nix {

void registerContextPrimOps();
void registerFetchClosurePrimOp();
void registerFetchMercurialPrimOp();
void registerFetchTreePrimOps();
void registerFromTomlPrimOp();

#if NIX_USE_WASMTIME
void registerWasmPrimOp();
#endif

} // namespace nix
