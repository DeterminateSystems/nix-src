#pragma once
///@file

#include "nix/util/logging.hh"

#include <memory>
#include <string_view>

namespace nix {

/**
 * A `Logger` that maps Nix activities onto OpenTelemetry spans and
 * exports them via OTLP/HTTP. Intended to be added to the global
 * logger using `applyExtraLogger()`.
 *
 * Activities replayed from another process (cf. `RemoteLogSource`)
 * are ignored, since the originating process is responsible for
 * exporting them.
 *
 * `flush()` exports all pending spans and shuts down the exporter,
 * with bounded timeouts. Since loggers are generally not destroyed,
 * it has to be called explicitly before the process exits;
 * `handleExceptions()` does so.
 *
 * `resetAfterFork()` discards the tracing state without exporting
 * anything, turning tracing off in this process. Note that a process
 * that wants to trace after a `fork()` doesn't need this: `initOtel()`
 * discards the inherited state by itself.
 */
class OpenTelemetryLogger : public Logger
{};

/**
 * Create an `OpenTelemetryLogger` whose root span is named
 * `rootSpanName`. Activities without a known parent become children
 * of the root span.
 *
 * If `remoteParentTraceparent` is non-empty, the root span is created
 * as a server span whose parent is the given W3C trace context (used
 * for daemon connections, where the client sends its trace context
 * during the handshake). An invalid value yields an unparented root
 * span.
 *
 * Returns null if tracing support is not compiled in or `initOtel()`
 * did not enable tracing; the caller should then not attach a logger.
 */
std::unique_ptr<OpenTelemetryLogger>
makeOpenTelemetryLogger(std::string_view rootSpanName, std::string_view remoteParentTraceparent = {});

/**
 * Initialize OpenTelemetry tracing for this process. Does nothing
 * (and is cheap) if `OTEL_EXPORTER_OTLP_ENDPOINT` /
 * `OTEL_EXPORTER_OTLP_TRACES_ENDPOINT` is not set in the environment,
 * or if tracing support is not compiled in.
 *
 * Any previously initialized tracing state is discarded rather than
 * reused, so this can be called in a child process after a `fork()`,
 * where the exporter's worker thread no longer exists.
 */
void initOtel(std::string_view serviceName);

} // namespace nix
