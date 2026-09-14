#pragma once
///@file

#include <string_view>

namespace nix {

/**
 * Initialize OpenTelemetry tracing for this process, and add a logger
 * to the global logger (using `applyExtraLogger()`) that maps Nix
 * activities onto OpenTelemetry spans, under a root span named
 * `rootSpanName`, and exports them via OTLP/HTTP.
 *
 * Does nothing (and is cheap) if tracing isn't configured (see the
 * `otlp` settings and the `OTEL_EXPORTER_OTLP_*` environment
 * variables), or if tracing support is not compiled in.
 *
 * If `remoteParentTraceparent` is non-empty, the root span's parent is
 * the given W3C trace context, e.g. received from the client during
 * the daemon handshake, or from a parent process via the `TRACEPARENT`
 * environment variable. An invalid value yields an unparented root
 * span. If `isServer` is set, the root span is a server span (i.e. it
 * handles a request from another process, as in the daemon).
 *
 * Any previously initialized tracing state is discarded rather than
 * reused, so this can be called in a child process after a `fork()`,
 * where the exporter's worker thread no longer exists.
 *
 * The logger's `flush()` exports all pending spans, with bounded
 * timeouts. Since loggers are generally not destroyed, it has to be
 * called explicitly before the process exits; `handleExceptions()`
 * does so.
 */
void initOtel(
    std::string_view serviceName,
    std::string_view rootSpanName,
    std::string_view remoteParentTraceparent = {},
    bool isServer = false);

} // namespace nix
