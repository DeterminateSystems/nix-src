#include "nix/util/otel.hh"

#include "util-config-private.hh"

#include <mutex>

#if HAVE_OTEL
#  include "nix/util/environment-variables.hh"
#  include "nix/util/terminal.hh"

#  include <atomic>

#  include <opentelemetry/context/context.h>
#  include <opentelemetry/context/propagation/text_map_propagator.h>
#  include <opentelemetry/exporters/otlp/otlp_http_exporter_factory.h>
#  include <opentelemetry/nostd/shared_ptr.h>
#  include <opentelemetry/sdk/resource/resource.h>
#  include <opentelemetry/sdk/trace/batch_span_processor_factory.h>
#  include <opentelemetry/sdk/trace/batch_span_processor_options.h>
#  include <opentelemetry/sdk/trace/tracer_provider.h>
#  include <opentelemetry/sdk/trace/tracer_provider_factory.h>
#  include <opentelemetry/semconv/service_attributes.h>
#  include <opentelemetry/trace/context.h>
#  include <opentelemetry/trace/propagation/http_trace_context.h>
#  include <opentelemetry/trace/span.h>
#  include <opentelemetry/trace/span_context.h>
#  include <opentelemetry/trace/span_startoptions.h>
#  include <opentelemetry/trace/tracer.h>
#endif

namespace nix::otel {

#if HAVE_OTEL

struct SpanImpl
{
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span;
};

namespace {

struct State
{
    std::unique_ptr<opentelemetry::sdk::trace::TracerProvider> provider;
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> tracer;
};

/* Owned here rather than via opentelemetry's global Provider
   singleton, and deliberately leaked: the provider's destructor joins
   the batch exporter's worker thread, which must never happen from
   static destructors at exit() time (cf. the OPENSSL_INIT_NO_ATEXIT
   note in util.cc; in the daemon's forked children that thread does
   not even exist). Teardown happens only via an explicit
   forceFlushAndShutdown(). */
std::atomic<State *> theState{nullptr};

opentelemetry::trace::SpanKind mapKind(SpanKind kind)
{
    switch (kind) {
    case SpanKind::Client:
        return opentelemetry::trace::SpanKind::kClient;
    case SpanKind::Server:
        return opentelemetry::trace::SpanKind::kServer;
    case SpanKind::Internal:
    default:
        return opentelemetry::trace::SpanKind::kInternal;
    }
}

inline opentelemetry::nostd::string_view toNostd(std::string_view sv) noexcept
{
    return {sv.data(), sv.size()};
}

struct ExtractCarrier : opentelemetry::context::propagation::TextMapCarrier
{
    opentelemetry::nostd::string_view traceparent, tracestate;

    opentelemetry::nostd::string_view Get(opentelemetry::nostd::string_view key) const noexcept override
    {
        if (key == "traceparent")
            return traceparent;
        if (key == "tracestate")
            return tracestate;
        return {};
    }

    void Set(opentelemetry::nostd::string_view, opentelemetry::nostd::string_view) noexcept override {}
};

struct InjectCarrier : opentelemetry::context::propagation::TextMapCarrier
{
    Headers headers;

    opentelemetry::nostd::string_view Get(opentelemetry::nostd::string_view) const noexcept override
    {
        return {};
    }

    void Set(opentelemetry::nostd::string_view key, opentelemetry::nostd::string_view value) noexcept override
    {
        /* Copy immediately: `value` may point into a stack buffer of
           the propagator. */
        headers.emplace_back(std::string(key.data(), key.size()), std::string(value.data(), value.size()));
    }
};

} // namespace

void init(std::string_view serviceName)
{
    if (theState.load(std::memory_order_acquire))
        return;

    /* Without an explicitly configured endpoint, stay off: the SDK's
       default would silently export to http://localhost:4318. */
    if (!getEnv("OTEL_EXPORTER_OTLP_ENDPOINT") && !getEnv("OTEL_EXPORTER_OTLP_TRACES_ENDPOINT"))
        return;

    namespace sdktrace = opentelemetry::sdk::trace;

    /* Default-constructed options read the OTEL_EXPORTER_OTLP_*
       environment variables (endpoint, headers, TLS, compression). */
    auto exporter = opentelemetry::exporter::otlp::OtlpHttpExporterFactory::Create();
    auto processor =
        sdktrace::BatchSpanProcessorFactory::Create(std::move(exporter), sdktrace::BatchSpanProcessorOptions{});
    auto resource = opentelemetry::sdk::resource::Resource::Create({
        {opentelemetry::semconv::service::kServiceName, std::string(serviceName)},
    });

    auto state = std::make_unique<State>();
    state->provider = sdktrace::TracerProviderFactory::Create(std::move(processor), resource);
    state->tracer = state->provider->GetTracer("nix");

    State * expected = nullptr;
    if (theState.compare_exchange_strong(expected, state.get()))
        state.release();
}

bool isEnabled() noexcept
{
    return theState.load(std::memory_order_acquire) != nullptr;
}

void resetAfterFork()
{
    /* Deliberately leak the old state: it references a worker thread
       that does not exist in this process, so it can neither flush
       nor be destroyed safely. */
    theState.exchange(nullptr);
    setRootSpan(Span());
}

void forceFlushAndShutdown(std::chrono::milliseconds timeout)
{
    auto * state = theState.exchange(nullptr);
    if (!state)
        return;
    /* Bound the timeouts: the SDK defaults are microseconds::max(),
       and a hung collector must not hang process exit. */
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(timeout);
    state->provider->ForceFlush(us);
    state->provider->Shutdown(us);
    /* Deliberately leak `state`: outstanding Span handles may still
       reference the tracer; after Shutdown() their spans are simply
       dropped. */
}

Span startSpan(std::string_view name, SpanKind kind)
{
    auto * state = theState.load(std::memory_order_acquire);
    if (!state)
        return Span();
    opentelemetry::trace::StartSpanOptions options;
    options.kind = mapKind(kind);
    return Span(std::make_shared<SpanImpl>(SpanImpl{state->tracer->StartSpan(toNostd(name), options)}));
}

Span startSpan(std::string_view name, const Span & parent, SpanKind kind)
{
    auto * state = theState.load(std::memory_order_acquire);
    if (!state)
        return Span();
    opentelemetry::trace::StartSpanOptions options;
    options.kind = mapKind(kind);
    if (parent.impl)
        options.parent = parent.impl->span->GetContext();
    return Span(std::make_shared<SpanImpl>(SpanImpl{state->tracer->StartSpan(toNostd(name), options)}));
}

Span startSpanFromRemoteParent(
    std::string_view name, std::string_view traceparent, std::string_view tracestate, SpanKind kind)
{
    auto * state = theState.load(std::memory_order_acquire);
    if (!state)
        return Span();
    opentelemetry::trace::StartSpanOptions options;
    options.kind = mapKind(kind);
    ExtractCarrier carrier;
    carrier.traceparent = toNostd(traceparent);
    carrier.tracestate = toNostd(tracestate);
    /* Extract() returns the input context unchanged on a parse
       failure, leaving an invalid SpanContext, i.e. a root span. */
    opentelemetry::context::Context emptyCtx;
    auto ctx = opentelemetry::trace::propagation::HttpTraceContext{}.Extract(carrier, emptyCtx);
    auto spanContext = opentelemetry::trace::GetSpan(ctx)->GetContext();
    if (spanContext.IsValid())
        options.parent = spanContext;
    return Span(std::make_shared<SpanImpl>(SpanImpl{state->tracer->StartSpan(toNostd(name), options)}));
}

bool Span::isRecording() const noexcept
{
    return impl && impl->span->IsRecording();
}

void Span::setAttribute(std::string_view key, std::string_view value)
{
    if (impl)
        impl->span->SetAttribute(toNostd(key), toNostd(value));
}

void Span::setAttribute(std::string_view key, int64_t value)
{
    if (impl)
        impl->span->SetAttribute(toNostd(key), value);
}

void Span::setAttribute(std::string_view key, bool value)
{
    if (impl)
        impl->span->SetAttribute(toNostd(key), value);
}

void Span::setError(std::string_view description)
{
    if (impl) {
        auto filtered = filterANSIEscapes(description, true);
        impl->span->SetStatus(opentelemetry::trace::StatusCode::kError, toNostd(filtered));
    }
}

void Span::end()
{
    if (impl)
        impl->span->End();
}

Headers Span::injectContext() const
{
    if (!impl || !impl->span->GetContext().IsValid())
        return {};
    InjectCarrier carrier;
    opentelemetry::context::Context ctx;
    auto withSpan = opentelemetry::trace::SetSpan(ctx, impl->span);
    opentelemetry::trace::propagation::HttpTraceContext{}.Inject(carrier, withSpan);
    return std::move(carrier.headers);
}

#else

struct SpanImpl
{};

void init(std::string_view) {}

bool isEnabled() noexcept
{
    return false;
}

void resetAfterFork() {}

void forceFlushAndShutdown(std::chrono::milliseconds) {}

Span startSpan(std::string_view, SpanKind)
{
    return Span();
}

Span startSpan(std::string_view, const Span &, SpanKind)
{
    return Span();
}

Span startSpanFromRemoteParent(std::string_view, std::string_view, std::string_view, SpanKind)
{
    return Span();
}

bool Span::isRecording() const noexcept
{
    return false;
}

void Span::setAttribute(std::string_view, std::string_view) {}

void Span::setAttribute(std::string_view, int64_t) {}

void Span::setAttribute(std::string_view, bool) {}

void Span::setError(std::string_view) {}

void Span::end() {}

Headers Span::injectContext() const
{
    return {};
}

#endif // HAVE_OTEL

Span::Span() noexcept = default;
Span::Span(const Span &) = default;
Span::Span(Span &&) noexcept = default;
Span & Span::operator=(const Span &) = default;
Span & Span::operator=(Span &&) noexcept = default;
Span::~Span() = default;

Span::Span(std::shared_ptr<SpanImpl> impl)
    : impl(std::move(impl))
{
}

static std::mutex rootSpanMutex;
static std::weak_ptr<SpanImpl> rootSpanImpl;

void setRootSpan(const Span & span)
{
    std::lock_guard<std::mutex> lock(rootSpanMutex);
    rootSpanImpl = span.impl;
}

Span rootSpan()
{
    std::lock_guard<std::mutex> lock(rootSpanMutex);
    return Span(rootSpanImpl.lock());
}

} // namespace nix::otel
