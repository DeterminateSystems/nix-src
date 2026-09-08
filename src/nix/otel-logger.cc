#include "otel-logger.hh"

#include "cli-config-private.hh"

#if HAVE_OTEL
#  include "nix/store/filetransfer.hh"
#  include "nix/util/base-n.hh"
#  include "nix/util/compression.hh"
#  include "nix/util/environment-variables.hh"
#  include "nix/util/exit.hh"
#  include "nix/util/processes.hh"
#  include "nix/util/serialise.hh"
#  include "nix/util/sync.hh"
#  include "nix/util/terminal.hh"
#  include "nix/util/url.hh"

#  include <atomic>
#  include <exception>
#  include <map>

#  include <boost/unordered/concurrent_flat_set.hpp>

#  include <nlohmann/json.hpp>

#  include <opentelemetry/context/context.h>
#  include <opentelemetry/context/propagation/text_map_propagator.h>
#  include <opentelemetry/nostd/shared_ptr.h>
#  include <opentelemetry/nostd/variant.h>
#  include <opentelemetry/sdk/common/exporter_utils.h>
#  include <opentelemetry/sdk/trace/exporter.h>
#  include <opentelemetry/sdk/trace/span_data.h>
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
   not even exist). So it's never destroyed; pending spans are
   exported by `OpenTelemetryLogger::flush()` instead. */
std::atomic<OtelState *> otelState{nullptr};

/* The exporter's worker thread doesn't exist in a forked child, so
   don't trace there unless the child sets up tracing itself (i.e. by
   calling `initOtel()`). The old state is deliberately leaked, since
   it can be neither flushed nor destroyed safely. */
static RegisterForkCallback resetOtel([]() { otelState.exchange(nullptr); });

inline opentelemetry::nostd::string_view toNostd(std::string_view sv) noexcept
{
    return {sv.data(), sv.size()};
}

/**
 * The name of the activity under which we upload spans. Since the
 * upload itself creates activities (namely the file transfer), we
 * must not create spans for this activity or its children, since
 * that would create an infinite regress.
 */
constexpr std::string_view uploadActivityName = "UploadOpenTelemetry";

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

class OpenTelemetryLoggerImpl : public OpenTelemetryLogger
{
    using SpanPtr = opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span>;

    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> tracer;

    SpanPtr rootSpan;

    Sync<std::map<ActivityId, SpanPtr>> spans_;

    /**
     * Activities for which we don't create spans, namely the
     * `UploadOpenTelemetry` activities and their children.
     */
    boost::concurrent_flat_set<ActivityId> ignoredActs;

    /**
     * Whether this activity should be ignored, i.e. whether it's an
     * `UploadOpenTelemetry` activity or a child of one. If so, record
     * it so that its children are ignored as well.
     */
    bool ignoreActivity(ActivityId act, std::string_view name, ActivityId parent)
    {
        if (name != uploadActivityName && !ignoredActs.contains(parent))
            return false;
        ignoredActs.insert(act);
        return true;
    }

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
            /* Printed by `flush()` once the trace has been
               uploaded. */
            state.debugTraceId = std::string(buf, sizeof(buf));
        }
    }

    void log(Verbosity lvl, std::string_view s) noexcept override {}

    void logEI(const ErrorInfo & ei) noexcept override {}

    void printException(const std::exception_ptr & ex, std::string_view programName) noexcept override
    {
        try {
            std::rethrow_exception(ex);
        } catch (Exit &) {
            /* Not a failure: this is how commands like `--version`
               return. */
        } catch (std::exception & e) {
            // FIXME: privacy
            rootSpan->SetStatus(opentelemetry::trace::StatusCode::kError, toNostd(filterANSIEscapes(e.what(), true)));
        } catch (...) {
            rootSpan->SetStatus(opentelemetry::trace::StatusCode::kError, "unknown exception");
        }
    }

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

            if (ignoreActivity(act, name, parent))
                return;

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

            if (ignoreActivity(act, name, parent))
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
            if (ignoredActs.erase(act))
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

    void flush() override
    {
        auto * state = otelState.load(std::memory_order_acquire);
        if (!state)
            return;

        /* Bound the timeout: the SDK default is microseconds::max(),
           and a hung collector must not hang process exit. */
        state->provider->ForceFlush(std::chrono::microseconds(std::chrono::seconds(5)));

        if (!state->debugTraceId.empty())
            writeToStderr(fmt("OpenTelemetry trace ID: %s\n", state->debugTraceId));
    }

    void resetAfterFork() override
    {
        /* Deliberately leak the old state: it may reference a worker
           thread that does not exist in this process, so it can be
           neither flushed nor destroyed safely. */
        otelState.exchange(nullptr);
    }
};

/**
 * Serialize an attribute value to its OTLP/JSON representation, i.e.
 * an `AnyValue` object such as `{"stringValue": "foo"}`. Note that
 * 64-bit integers are represented as strings in OTLP/JSON.
 */
nlohmann::json toAnyValue(const opentelemetry::sdk::common::OwnedAttributeValue & value)
{
    return opentelemetry::nostd::visit(
        [](const auto & v) -> nlohmann::json {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>)
                return {{"boolValue", v}};
            else if constexpr (std::is_same_v<T, double>)
                return {{"doubleValue", v}};
            else if constexpr (std::is_same_v<T, std::string>)
                return {{"stringValue", v}};
            else if constexpr (std::is_integral_v<T>)
                return {{"intValue", std::to_string(v)}};
            else if constexpr (std::is_same_v<T, std::vector<uint8_t>>)
                return {{"bytesValue", base64::encode(std::as_bytes(std::span{v}))}};
            else {
                /* Any other vector: an OTLP array of AnyValues. Note
                   that we can't take the elements by reference, since
                   `std::vector<bool>` yields proxy references. */
                auto values = nlohmann::json::array();
                for (auto x : v) {
                    using E = std::decay_t<decltype(x)>;
                    if constexpr (std::is_same_v<E, bool>)
                        values.push_back({{"boolValue", (bool) x}});
                    else if constexpr (std::is_same_v<E, double>)
                        values.push_back({{"doubleValue", x}});
                    else if constexpr (std::is_same_v<E, std::string>)
                        values.push_back({{"stringValue", x}});
                    else
                        values.push_back({{"intValue", std::to_string(x)}});
                }
                return {{"arrayValue", {{"values", std::move(values)}}}};
            }
        },
        value);
}

/**
 * Serialize an attribute map to an OTLP/JSON `KeyValue` array.
 */
template<typename Map>
nlohmann::json toAttributes(const Map & map)
{
    auto res = nlohmann::json::array();
    for (auto & [key, value] : map)
        res.push_back({{"key", key}, {"value", toAnyValue(value)}});
    return res;
}

template<typename Id>
std::string toHex(const Id & id)
{
    char buf[2 * Id::kSize];
    id.ToLowerBase16(buf);
    return std::string(buf, sizeof(buf));
}

std::string toUnixNano(std::chrono::nanoseconds t)
{
    return std::to_string(t.count());
}

/**
 * A span exporter that serializes spans to OTLP/JSON and uploads them
 * using Nix's own `FileTransfer`. Compared to the exporter that comes
 * with opentelemetry-cpp, this avoids a dependency on protobuf (which
 * is very large), and it reuses the HTTP client that Nix already has.
 */
class OtlpJsonSpanExporter final : public opentelemetry::sdk::trace::SpanExporter
{
    std::string endpoint;
    Headers headers;
    bool compress;

    std::atomic<bool> isShutdown{false};

public:

    OtlpJsonSpanExporter(std::string endpoint, Headers headers, bool compress)
        : endpoint(std::move(endpoint))
        , headers(std::move(headers))
        , compress(compress)
    {
    }

    std::unique_ptr<opentelemetry::sdk::trace::Recordable> MakeRecordable() noexcept override
    {
        return std::make_unique<opentelemetry::sdk::trace::SpanData>();
    }

    opentelemetry::sdk::common::ExportResult Export(
        const opentelemetry::nostd::span<std::unique_ptr<opentelemetry::sdk::trace::Recordable>> & recordables) noexcept
        override
    {
        using opentelemetry::sdk::common::ExportResult;

        if (isShutdown.load(std::memory_order_acquire))
            return ExportResult::kFailure;

        try {
            if (recordables.empty())
                return ExportResult::kSuccess;

            /* All spans in a batch come from the same tracer provider,
               so they share a resource and (in our case) a scope. */
            auto spans = nlohmann::json::array();
            const opentelemetry::sdk::resource::Resource * resource = nullptr;
            const opentelemetry::sdk::trace::InstrumentationScope * scope = nullptr;

            for (auto & recordable : recordables) {
                /* Safe: `MakeRecordable()` only ever returns `SpanData`. */
                auto & span = static_cast<opentelemetry::sdk::trace::SpanData &>(*recordable);

                resource = &span.GetResource();
                scope = &span.GetInstrumentationScope();

                nlohmann::json json{
                    {"traceId", toHex(span.GetTraceId())},
                    {"spanId", toHex(span.GetSpanId())},
                    {"name", std::string(span.GetName().data(), span.GetName().size())},
                    /* `SpanKind` is declared in the same order as in
                       OTLP, which starts counting at `unspecified`. */
                    {"kind", (int) span.GetSpanKind() + 1},
                    {"startTimeUnixNano", toUnixNano(span.GetStartTime().time_since_epoch())},
                    {"endTimeUnixNano", toUnixNano(span.GetStartTime().time_since_epoch() + span.GetDuration())},
                    {"attributes", toAttributes(span.GetAttributes())},
                    {"flags", span.GetFlags().flags()},
                };

                if (span.GetParentSpanId().IsValid())
                    json["parentSpanId"] = toHex(span.GetParentSpanId());

                if (span.GetStatus() != opentelemetry::trace::StatusCode::kUnset) {
                    auto description = span.GetDescription();
                    json["status"] = {
                        {"code", (int) span.GetStatus()},
                        {"message", std::string(description.data(), description.size())},
                    };
                }

                spans.push_back(std::move(json));
            }

            nlohmann::json doc{
                {"resourceSpans",
                 {{
                     {"resource", {{"attributes", toAttributes(resource->GetAttributes())}}},
                     {"scopeSpans",
                      {{
                          {"scope", {{"name", scope->GetName()}, {"version", scope->GetVersion()}}},
                          {"spans", std::move(spans)},
                      }}},
                 }}},
            };

            upload(doc.dump());

            return ExportResult::kSuccess;
        } catch (...) {
            /* Note that nothing retries a failed export, so all we can
               do is drop the spans. Don't let the exception escape,
               since this method is noexcept. Also don't bother the
               user about it: failing to export telemetry should not
               be noise on top of whatever they're actually doing. */
            ignoreExceptionInDestructor(lvlDebug);
            return ExportResult::kFailure;
        }
    }

    void upload(std::string payload)
    {
        /* The upload creates activities of its own, which would be
           exported as spans, which would create more activities, ad
           infinitum. So do it inside an activity that the
           OpenTelemetryLogger ignores, along with its children. */
        Activity act(*logger, lvlDebug, uploadActivityName, {}, "", 0);
        PushActivity pact(act.id);

        FileTransferRequest request(parseURL(endpoint));
        request.method = HttpMethod::Post;
        request.mimeType = "application/json";
        request.headers = headers;

        if (compress) {
            payload = nix::compress(CompressionAlgo::gzip, payload);
            request.headers.emplace_back("Content-Encoding", "gzip");
        }

        StringSource source{payload};
        request.data = {source};

        /* Don't hold up the process at exit retrying telemetry. */
        request.retryAttempts = 0;

        getFileTransfer()->upload(request);
    }

    bool ForceFlush(std::chrono::microseconds) noexcept override
    {
        /* We upload synchronously in `Export()`, so there is never
           anything buffered here. */
        return true;
    }

    bool Shutdown(std::chrono::microseconds) noexcept override
    {
        isShutdown.store(true, std::memory_order_release);
        return true;
    }
};

/**
 * Parse `OTEL_EXPORTER_OTLP_HEADERS`, a comma-separated list of
 * percent-encoded `name=value` pairs.
 */
Headers parseOtlpHeaders(std::string_view s)
{
    Headers headers;
    for (auto & item : tokenizeString<Strings>(s, ",")) {
        auto eq = item.find('=');
        if (eq == std::string::npos)
            continue;
        auto name = trim(item.substr(0, eq));
        auto value = trim(item.substr(eq + 1));
        if (!name.empty())
            headers.emplace_back(percentDecode(name), percentDecode(value));
    }
    return headers;
}

} // namespace

void initOtel(std::string_view serviceName)
{
    /* Without an explicitly configured endpoint, stay off; we don't
       want to export to some default endpoint behind the user's
       back. */
    auto endpoint = [&]() -> std::string {
        if (auto s = getEnv("OTEL_EXPORTER_OTLP_TRACES_ENDPOINT"))
            return *s;
        if (auto s = getEnv("OTEL_EXPORTER_OTLP_ENDPOINT"))
            return *s + "/v1/traces";
        return "";
    }();
    if (endpoint.empty())
        return;

    namespace sdktrace = opentelemetry::sdk::trace;

    auto exporter = std::make_unique<OtlpJsonSpanExporter>(
        endpoint,
        parseOtlpHeaders(getEnv("OTEL_EXPORTER_OTLP_HEADERS").value_or("")),
        getEnv("OTEL_EXPORTER_OTLP_COMPRESSION").value_or("gzip") == "gzip");
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

    /* Any previously initialized state is deliberately leaked rather
       than destroyed, since we may be in a child process where its
       worker thread doesn't exist. */
    otelState.exchange(state.release(), std::memory_order_release);
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

std::unique_ptr<OpenTelemetryLogger> makeOpenTelemetryLogger(std::string_view, std::string_view)
{
    return nullptr;
}

#endif // HAVE_OTEL

} // namespace nix
