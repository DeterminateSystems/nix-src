#pragma once
///@file

#include "nix/util/serialise.hh"
#include "nix/store/store-api.hh"

#include <functional>

namespace nix::daemon {

enum RecursiveFlag : bool { NotRecursive = false, Recursive = true };

/**
 * Serve a client on the given file descriptors.
 *
 * @param setupTelemetry Called once after the handshake and logger
 * setup, with the W3C `traceparent` string received from the client
 * (empty if the client sent none or the protocol feature was not
 * negotiated). Allows the caller to set up distributed tracing for
 * the connection, e.g. by adding a tracing logger.
 */
void processConnection(
    ref<Store> store,
    FdSource && from,
    FdSink && to,
    TrustedFlag trusted,
    RecursiveFlag recursive,
    std::function<void(std::string_view traceparent)> setupTelemetry = {});

} // namespace nix::daemon
