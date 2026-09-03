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
 */
class OpenTelemetryLogger : public Logger
{
public:
    /**
     * Record an error on the root span.
     */
    virtual void setRootError(std::string_view description) noexcept = 0;
};

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
 * if tracing support is not compiled in, or if tracing is already
 * initialized.
 */
void initOtel(std::string_view serviceName);

/**
 * Flush all pending spans and shut down the exporter, with bounded
 * timeouts. Safe to call if tracing was never initialized, and safe
 * to call more than once. Must be called explicitly before process
 * exit: nothing is flushed from static destructors (cf. the
 * OPENSSL_INIT_NO_ATEXIT note in util.cc). Spans that should be
 * included must be ended first (e.g. via `logger->stop()`).
 */
void flushOtelAndShutdown();

/**
 * Discard all tracing state inherited from the parent process after a
 * fork(): the exporter's worker thread does not exist in the child,
 * so the inherited state can be neither used nor destroyed safely.
 * Afterwards `initOtel()` can be called again to start fresh tracing
 * in the child.
 */
void resetOtelAfterFork();

} // namespace nix
