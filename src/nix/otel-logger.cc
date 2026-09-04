#include "otel-logger.hh"

#include "cli-config-private.hh"

#if HAVE_OTEL
#  include "nix/util/environment-variables.hh"
#  include "nix/util/sync.hh"
#  include "nix/util/terminal.hh"

#  include <atomic>
#  include <exception>
#  include <map>

#  include <nlohmann/json.hpp>

#  include <opentelemetry/context/context.h>
#  include <opentelemetry/context/propagation/text_map_propagator.h>
#  include <opentelemetry/exporters/otlp/otlp_http_exporter_factory.h>
#  include <opentelemetry/nostd/shared_ptr.h>
#  include <opentelemetry/sdk/resource/resource.h>
#  include <opentelemetry/sdk/trace/batch_span_processor_factory.h>
#  include <opentelemetry/sdk/trace/batch_span_processor_options.h>
#  include <opentelemetry/sdk/trace/samplers/always_off.h>
#  include <opentelemetry/sdk/trace/samplers/always_on.h>
#  include <opentelemetry/sdk/trace/samplers/parent.h>
#  include <opentelemetry/sdk/trace/samplers/trace_id_ratio.h>
#  include <opentelemetry/sdk/trace/tracer_provider.h>
#  include <opentelemetry/sdk/trace/tracer_provider_factory.h>
#  include <opentelemetry/semconv/service_attributes.h>
#  include <opentelemetry/trace/context.h>
#  include <opentelemetry/trace/default_span.h>
#  include <opentelemetry/trace/propagation/http_trace_context.h>
#  include <opentelemetry/trace/span.h>
#  include <opentelemetry/trace/span_context.h>
#  include <opentelemetry/trace/span_startoptions.h>
#  include <opentelemetry/trace/tracer.h>
#endif

namespace nix {

#if HAVE_OTEL

namespace {

struct OtelState
{
    std::unique_ptr<opentelemetry::sdk::trace::TracerProvider> provider;
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> tracer;

    /**
     * The root span's trace ID, to be printed on shutdown if
     * NIX_OTEL_DEBUG is set.
     */
    std::string debugTraceId;
};

/* Owned here rather than via opentelemetry's global Provider
   singleton, and deliberately leaked: the provider's destructor joins
   the batch exporter's worker thread, which must never happen from
   static destructors at exit() time (cf. the OPENSSL_INIT_NO_ATEXIT
   note in util.cc; in the daemon's forked children that thread does
   not even exist). Teardown happens only via an explicit
   flushOtelAndShutdown(). */
std::atomic<OtelState *> otelState{nullptr};

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

/**
 * Parse a W3C `traceparent` value into a span context usable as a
 * remote parent. Returns std::nullopt on an empty or invalid value.
 * (Extract() returns the input context unchanged on a parse failure,
 * leaving an invalid SpanContext.)
 */
std::optional<opentelemetry::trace::SpanContext> parseTraceparent(std::string_view traceparent)
{
    if (traceparent.empty())
        return std::nullopt;
    ExtractCarrier carrier;
    carrier.traceparent = toNostd(traceparent);
    opentelemetry::context::Context emptyCtx;
    auto ctx = opentelemetry::trace::propagation::HttpTraceContext{}.Extract(carrier, emptyCtx);
    auto spanContext = opentelemetry::trace::GetSpan(ctx)->GetContext();
    if (!spanContext.IsValid())
        return std::nullopt;
    return spanContext;
}

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

/**
 * The span name for an activity: the name of the enum value without
 * the `act` prefix, e.g. `OptimiseStore`. An empty result means the
 * activity's text should be used instead.
 *
 * TODO: Use C++26 reflection to derive this from the enum
 * definition generically instead of enumerating the values here.
 */
std::string_view activityName(ActivityType type)
{
    switch (type) {
    case actUnknown:
        return {};
#  define ACTIVITY_NAME(name) \
  case act##name:             \
      return #name;
        ACTIVITY_NAME(CopyPath)
        ACTIVITY_NAME(FileTransfer)
        ACTIVITY_NAME(Realise)
        ACTIVITY_NAME(CopyPaths)
        ACTIVITY_NAME(Builds)
        ACTIVITY_NAME(Build)
        ACTIVITY_NAME(OptimiseStore)
        ACTIVITY_NAME(VerifyPaths)
        ACTIVITY_NAME(Substitute)
        ACTIVITY_NAME(QueryPathInfo)
        ACTIVITY_NAME(PostBuildHook)
        ACTIVITY_NAME(BuildWaiting)
        ACTIVITY_NAME(FetchTree)
#  undef ACTIVITY_NAME
    case actStringly:
        /* Only reached via a Logger that flattens string-named
           activities; the text fallback is the best we can do. */
        return {};
    }
    return {};
}

/* Defensive field accessors, like the progress bar's. */
std::string_view getS(const Logger::Fields & fields, size_t n)
{
    if (n < fields.size()) {
        if (auto p = std::get_if<std::string>(&fields[n].raw))
            return *p;
    }
    return {};
}

uint64_t getI(const Logger::Fields & fields, size_t n)
{
    if (n < fields.size()) {
        if (auto p = std::get_if<uint64_t>(&fields[n].raw))
            return *p;
    }
    return 0;
}

class OpenTelemetryLoggerImpl : public OpenTelemetryLogger
{
    using SpanPtr = opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span>;

    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> tracer;

    SpanPtr rootSpan;

    Sync<std::map<ActivityId, SpanPtr>> spans_;

    static Headers injectContext(const SpanPtr & span)
    {
        if (!span->GetContext().IsValid())
            return {};
        InjectCarrier carrier;
        opentelemetry::context::Context ctx;
        auto withSpan = opentelemetry::trace::SetSpan(ctx, span);
        opentelemetry::trace::propagation::HttpTraceContext{}.Inject(carrier, withSpan);
        return std::move(carrier.headers);
    }

public:
    OpenTelemetryLoggerImpl(OtelState & state, std::string_view rootSpanName, std::string_view remoteParentTraceparent)
        : tracer(state.tracer)
    {
        opentelemetry::trace::StartSpanOptions options;
        if (!remoteParentTraceparent.empty()) {
            options.kind = opentelemetry::trace::SpanKind::kServer;
            if (auto spanContext = parseTraceparent(remoteParentTraceparent))
                options.parent = *spanContext;
        }
        rootSpan = tracer->StartSpan(toNostd(rootSpanName), options);

        if (getEnv("NIX_OTEL_DEBUG")) {
            char buf[2 * opentelemetry::trace::TraceId::kSize];
            rootSpan->GetContext().trace_id().ToLowerBase16(buf);
            /* Printed by flushOtelAndShutdown() once the trace has
               been uploaded. */
            state.debugTraceId = std::string(buf, sizeof(buf));
        }
    }

    void log(Verbosity lvl, std::string_view s) noexcept override {}

    void logEI(const ErrorInfo & ei) noexcept override {}

    void startActivity(
        ActivityId act,
        Verbosity lvl,
        ActivityType type,
        const std::string & s,
        const Fields & fields,
        ActivityId parent) noexcept override
    {
        try {
            if (isRemoteLogSource())
                return;

            opentelemetry::trace::StartSpanOptions options;

            auto name = activityName(type);
            bool textIsName = name.empty();
            if (textIsName)
                name = s.empty() ? "activity" : std::string_view(s);

            auto spans(spans_.lock());

            if (auto i = spans->find(parent); i != spans->end())
                options.parent = i->second->GetContext();
            else
                options.parent = rootSpan->GetContext();

            auto span = tracer->StartSpan(toNostd(name), options);

            if (!s.empty() && !textIsName)
                span->SetAttribute("nix.activity.text", toNostd(filterANSIEscapes(s, true)));

// Allow handling a subset of enum values
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wswitch-enum"
            switch (type) {
            case actFileTransfer:
                span->SetAttribute("url.full", toNostd(getS(fields, 0)));
                break;
            case actBuild:
            case actPostBuildHook:
                span->SetAttribute("nix.drv.path", toNostd(getS(fields, 0)));
                if (auto machine = getS(fields, 1); !machine.empty())
                    span->SetAttribute("nix.machine", toNostd(machine));
                break;
            case actSubstitute:
            case actQueryPathInfo:
                span->SetAttribute("nix.store.path", toNostd(getS(fields, 0)));
                span->SetAttribute("nix.substituter", toNostd(getS(fields, 1)));
                break;
            case actCopyPath:
                span->SetAttribute("nix.store.path", toNostd(getS(fields, 0)));
                span->SetAttribute("nix.src.store", toNostd(getS(fields, 1)));
                span->SetAttribute("nix.dst.store", toNostd(getS(fields, 2)));
                break;
            default:
                break;
            }
#  pragma GCC diagnostic pop

            spans->emplace(act, std::move(span));
        } catch (...) {
        }
    }

    void startActivity(
        ActivityId act,
        Verbosity lvl,
        std::string_view name,
        ActivityMetadata metadata,
        std::string_view s,
        ActivityId parent) noexcept override
    {
        try {
            if (isRemoteLogSource())
                return;

            /* An activity carrying a `traceparent` metadata field
               exists only to link its child activities to a span in
               another process (e.g. the client activity on whose
               behalf the daemon is performing an operation). Don't
               emit a span for the activity itself — there can be very
               many of them (one per daemon operation) — but record
               the remote context, or the local parent if the trace
               context is absent or invalid, for parent lookups by
               child activities. */
            for (auto & [key, value] : metadata) {
                if (key != "traceparent")
                    continue;
                std::optional<opentelemetry::trace::SpanContext> spanContext;
                if (auto str = std::get_if<std::string>(&value.raw))
                    spanContext = parseTraceparent(*str);
                auto spans(spans_.lock());
                if (!spanContext) {
                    if (auto i = spans->find(parent); i != spans->end())
                        spanContext = i->second->GetContext();
                    else
                        spanContext = rootSpan->GetContext();
                }
                spans->emplace(act, SpanPtr(new opentelemetry::trace::DefaultSpan(*spanContext)));
                return;
            }

            opentelemetry::trace::StartSpanOptions options;

            /* Per the OpenTelemetry semantic conventions, HTTP client
               spans are named after the request method. */
            auto spanName = name;
            for (auto & [key, value] : metadata)
                if (key == "http.request.method")
                    if (auto method = std::get_if<std::string>(&value.raw)) {
                        options.kind = opentelemetry::trace::SpanKind::kClient;
                        spanName = *method;
                    }

            auto spans(spans_.lock());

            if (auto i = spans->find(parent); i != spans->end())
                options.parent = i->second->GetContext();
            else
                options.parent = rootSpan->GetContext();

            auto span = tracer->StartSpan(toNostd(spanName), options);

            if (!s.empty())
                span->SetAttribute("nix.activity.text", toNostd(filterANSIEscapes(s, true)));

            for (auto & [key, value] : metadata) {
                if (auto str = std::get_if<std::string>(&value.raw))
                    span->SetAttribute(toNostd(key), toNostd(*str));
                else if (auto n = std::get_if<uint64_t>(&value.raw))
                    span->SetAttribute(toNostd(key), (int64_t) *n);
            }

            spans->emplace(act, std::move(span));
        } catch (...) {
        }
    }

    void stopActivity(ActivityId act) noexcept override
    {
        try {
            if (isRemoteLogSource())
                return;
            auto spans(spans_.lock());
            if (auto i = spans->find(act); i != spans->end()) {
                /* If the activity is being stopped while an exception
                   is in flight, i.e. the `Activity` is being destroyed
                   by stack unwinding, assume that the activity
                   failed. */
                if (std::uncaught_exceptions())
                    i->second->SetStatus(
                        opentelemetry::trace::StatusCode::kError, "activity terminated by an exception");
                i->second->End();
                spans->erase(i);
            }
        } catch (...) {
        }
    }

    void result(ActivityId act, ResultType type, const nlohmann::json & json) noexcept override
    {
        try {
            if (isRemoteLogSource())
                return;
            if (type == resHttpStatus) {
                auto spans(spans_.lock());
                if (auto i = spans->find(act); i != spans->end()) {
                    if (auto status = json.find("httpStatus"); status != json.end() && status->is_number())
                        i->second->SetAttribute("http.response.status_code", status->get<int64_t>());
                    if (auto bodySize = json.find("bodySize"); bodySize != json.end() && bodySize->is_number())
                        i->second->SetAttribute("http.response.body.size", bodySize->get<int64_t>());
                }
            }
        } catch (...) {
        }
    }

    Headers getTraceContext(ActivityId act) override
    {
        try {
            if (act) {
                auto spans(spans_.lock());
                if (auto i = spans->find(act); i != spans->end())
                    return injectContext(i->second);
            }
            return injectContext(rootSpan);
        } catch (...) {
            return {};
        }
    }

    void setRootError(std::string_view description) noexcept override
    {
        try {
            rootSpan->SetStatus(
                opentelemetry::trace::StatusCode::kError, toNostd(filterANSIEscapes(description, true)));
        } catch (...) {
        }
    }

    void stop() override
    {
        try {
            auto spans(spans_.lock());
            for (auto & [_, span] : *spans)
                span->End();
            spans->clear();
            rootSpan->End();
        } catch (...) {
        }
    }
};

} // namespace

void initOtel(std::string_view serviceName)
{
    if (otelState.load(std::memory_order_acquire))
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

    /* Support the standard OTEL_TRACES_SAMPLER / OTEL_TRACES_SAMPLER_ARG
       environment variables, which the C++ SDK does not read itself.
       "parentbased" samplers follow the sampling decision of the
       parent span, which propagates in the sampled flag of the W3C
       trace context — so the daemon follows the client's decision. */
    auto sampler = [&]() -> std::unique_ptr<sdktrace::Sampler> {
        auto ratio = [&]() -> double {
            auto arg = getEnv("OTEL_TRACES_SAMPLER_ARG");
            if (!arg)
                return 1.0;
            try {
                return std::stod(*arg);
            } catch (...) {
                warn("invalid OTEL_TRACES_SAMPLER_ARG '%s'; assuming 1.0", *arg);
                return 1.0;
            }
        };
        auto parentBased = [](std::shared_ptr<sdktrace::Sampler> delegate) -> std::unique_ptr<sdktrace::Sampler> {
            return std::make_unique<sdktrace::ParentBasedSampler>(std::move(delegate));
        };
        auto name = getEnv("OTEL_TRACES_SAMPLER").value_or("parentbased_always_on");
        if (name == "always_on")
            return std::make_unique<sdktrace::AlwaysOnSampler>();
        if (name == "always_off")
            return std::make_unique<sdktrace::AlwaysOffSampler>();
        if (name == "traceidratio")
            return std::make_unique<sdktrace::TraceIdRatioBasedSampler>(ratio());
        if (name == "parentbased_always_off")
            return parentBased(std::make_shared<sdktrace::AlwaysOffSampler>());
        if (name == "parentbased_traceidratio")
            return parentBased(std::make_shared<sdktrace::TraceIdRatioBasedSampler>(ratio()));
        if (name != "parentbased_always_on")
            warn("unknown OTEL_TRACES_SAMPLER '%s'; assuming 'parentbased_always_on'", name);
        return parentBased(std::make_shared<sdktrace::AlwaysOnSampler>());
    }();

    auto state = std::make_unique<OtelState>();
    state->provider = sdktrace::TracerProviderFactory::Create(std::move(processor), resource, std::move(sampler));
    state->tracer = state->provider->GetTracer("nix");

    OtelState * expected = nullptr;
    if (otelState.compare_exchange_strong(expected, state.get()))
        state.release();
}

void flushOtelAndShutdown()
{
    auto * state = otelState.exchange(nullptr);
    if (!state)
        return;
    /* Bound the timeouts: the SDK defaults are microseconds::max(),
       and a hung collector must not hang process exit. */
    auto timeout = std::chrono::microseconds(std::chrono::seconds(5));
    state->provider->ForceFlush(timeout);
    state->provider->Shutdown(timeout);

    if (!state->debugTraceId.empty())
        writeToStderr(fmt("OpenTelemetry trace ID: %s\n", state->debugTraceId));

    /* Deliberately leak `state`: outstanding span handles may still
       reference the tracer; after Shutdown() their spans are simply
       dropped. */
}

void resetOtelAfterFork()
{
    /* Deliberately leak the old state: it references a worker thread
       that does not exist in this process, so it can be neither
       flushed nor destroyed safely. */
    otelState.exchange(nullptr);
}

std::unique_ptr<OpenTelemetryLogger>
makeOpenTelemetryLogger(std::string_view rootSpanName, std::string_view remoteParentTraceparent)
{
    auto * state = otelState.load(std::memory_order_acquire);
    if (!state)
        return nullptr;
    return std::make_unique<OpenTelemetryLoggerImpl>(*state, rootSpanName, remoteParentTraceparent);
}

#else

void initOtel(std::string_view) {}

void flushOtelAndShutdown() {}

void resetOtelAfterFork() {}

std::unique_ptr<OpenTelemetryLogger> makeOpenTelemetryLogger(std::string_view, std::string_view)
{
    return nullptr;
}

#endif // HAVE_OTEL

} // namespace nix
