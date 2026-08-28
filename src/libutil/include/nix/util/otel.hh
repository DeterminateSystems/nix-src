#pragma once
///@file
/**
 * Vendor-neutral distributed tracing facade. Backed by OpenTelemetry
 * (exporting via OTLP/HTTP) when built with `-Dotel=enabled`;
 * otherwise all operations are cheap no-ops. Even when compiled in,
 * tracing stays disabled unless `init()` is called and
 * `OTEL_EXPORTER_OTLP_ENDPOINT` (or
 * `OTEL_EXPORTER_OTLP_TRACES_ENDPOINT`) is set in the environment.
 */

#include "nix/util/types.hh"

#include <chrono>
#include <memory>
#include <string_view>

namespace nix::otel {

enum class SpanKind {
    Internal,
    Client,
    Server,
};

struct SpanImpl;

/**
 * A handle to an in-flight trace span.
 *
 * Value type with shared-handle semantics: copies refer to the same
 * underlying span. The span ends when `end()` is first called, or, as
 * a fallback, when the last handle is destroyed. Handles may be moved
 * or copied across threads; the underlying span is thread-safe.
 *
 * A default-constructed handle (or any handle obtained while tracing
 * is disabled) is inert: all operations are cheap no-ops.
 */
class Span
{
    friend Span startSpan(std::string_view name, SpanKind kind);
    friend Span startSpan(std::string_view name, const Span & parent, SpanKind kind);
    friend Span startSpanFromRemoteParent(
        std::string_view name, std::string_view traceparent, std::string_view tracestate, SpanKind kind);
    friend void setRootSpan(const Span & span);
    friend Span rootSpan();

    std::shared_ptr<SpanImpl> impl;

    explicit Span(std::shared_ptr<SpanImpl> impl);

public:
    Span() noexcept;
    Span(const Span &);
    Span(Span &&) noexcept;
    Span & operator=(const Span &);
    Span & operator=(Span &&) noexcept;
    ~Span();

    /**
     * Whether this span actually records data (i.e. tracing is
     * compiled in, initialized, and this span is sampled).
     */
    bool isRecording() const noexcept;

    void setAttribute(std::string_view key, std::string_view value);
    void setAttribute(std::string_view key, int64_t value);
    void setAttribute(std::string_view key, bool value);

    /**
     * Mark this span as failed, with a description of the error. ANSI
     * escape sequences are stripped from the description.
     */
    void setError(std::string_view description);

    /**
     * End the span. Idempotent; only the first call records the end
     * timestamp.
     */
    void end();

    /**
     * Return W3C Trace Context headers (`traceparent`, `tracestate`)
     * that propagate this span's context to another process, e.g. as
     * HTTP request headers. Returns an empty list if this span is
     * disabled.
     */
    Headers injectContext() const;
};

/**
 * Initialize tracing for this process. Does nothing (and is cheap) if
 * `OTEL_EXPORTER_OTLP_ENDPOINT` / `OTEL_EXPORTER_OTLP_TRACES_ENDPOINT`
 * is not set in the environment, if tracing support is not compiled
 * in, or if tracing is already initialized. Not called from library
 * code; the program's main entry point decides.
 */
void init(std::string_view serviceName);

/**
 * Whether tracing is compiled in, initialized and not yet shut down.
 */
bool isEnabled() noexcept;

/**
 * Start a new root span (no parent).
 */
Span startSpan(std::string_view name, SpanKind kind = SpanKind::Internal);

/**
 * Start a span as a child of `parent`. If `parent` is disabled, this
 * behaves like the parentless overload.
 */
Span startSpan(std::string_view name, const Span & parent, SpanKind kind = SpanKind::Internal);

/**
 * Start a span whose parent is a span in another process, identified
 * by W3C `traceparent` / `tracestate` header values (e.g. received by
 * the daemon from a client). Invalid values yield a root span.
 */
Span startSpanFromRemoteParent(
    std::string_view name,
    std::string_view traceparent,
    std::string_view tracestate = {},
    SpanKind kind = SpanKind::Internal);

/**
 * Register the process-wide root span, used as the default parent by
 * call sites that have no more specific parent (e.g. HTTP transfers).
 * Only a weak reference is kept, so this does not extend the span's
 * lifetime.
 */
void setRootSpan(const Span & span);

/**
 * The span registered with setRootSpan(), or an inert span if none
 * was registered or it no longer exists.
 */
Span rootSpan();

/**
 * Flush all pending spans and shut down the exporter. Safe to call if
 * tracing was never initialized, and safe to call more than once.
 * Must be called explicitly before process exit: nothing is flushed
 * from static destructors (cf. the OPENSSL_INIT_NO_ATEXIT note in
 * util.cc). Must not be called in a fork()ed child of the process
 * that called `init()`.
 */
void forceFlushAndShutdown(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

} // namespace nix::otel
