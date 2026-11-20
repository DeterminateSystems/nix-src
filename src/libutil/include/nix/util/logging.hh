#pragma once
///@file

#include "nix/util/error.hh"
#include "nix/util/configuration.hh"
#include "nix/util/types.hh"
#include "nix/util/file-descriptor.hh"
#include "nix/util/finally.hh"
#include "nix/util/fun.hh"

#include <filesystem>
#include <span>
#include <type_traits>

#include <nlohmann/json_fwd.hpp>

namespace nix {

/* TODO: Make these enum classes. */

typedef enum {
    actUnknown = 0,
    actCopyPath = 100,
    actFileTransfer = 101,
    actRealise = 102,
    actCopyPaths = 103,
    actBuilds = 104,
    actBuild = 105,
    actOptimiseStore = 106,
    actVerifyPaths = 107,
    actSubstitute = 108,
    actQueryPathInfo = 109,
    actPostBuildHook = 110,
    actBuildWaiting = 111,
    actFetchTree = 112,
    /**
     * Upstream Nix's activity type for a source path being copied to
     * the store or hashed by `fetchToStore()`. Fields: [0] = source
     * path (string), [1] = whether the path is only being hashed (1)
     * rather than copied (0). Determinate Nix does not emit this (it
     * uses the string-named `CopySourcePath` and `HashSourcePath`
     * activities instead), but accepts it in log messages from other
     * Nix versions.
     */
    actFetchToStore = 113,
    /**
     * The last of the contiguous (upstream) activity types. Determinate
     * Nix-specific types (10000 and up) come after this; use
     * `isKnownActivityType()` to validate a raw value.
     */
    actLast = actFetchToStore,
    /**
     * The type under which string-named activities (created via the
     * name-based `Activity` constructor) are reported on legacy code
     * paths, cf. the name-based `Logger::startActivity()`. Do not use
     * directly.
     */
    actStringly = 10113,
} ActivityType;

typedef enum {
    resFileLinked = 100,
    resBuildLogLine = 101,
    resUntrustedPath = 102,
    resCorruptedPath = 103,
    resSetPhase = 104,
    resProgress = 105,
    resSetExpected = 106,
    resPostBuildLogLine = 107,
    resFetchStatus = 108,
    /**
     * The resulting store path of a `CopySourcePath` or
     * `HashSourcePath` activity (see `fetchToStore()`), emitted once
     * the operation completes. Fields: [0] = store path (string).
     */
    resFetchToStore = 109,
    /**
     * The last of the contiguous (upstream) result types. Determinate
     * Nix-specific types (10000 and up) come after this; use
     * `isKnownResultType()` to validate a raw value.
     */
    resLast = resFetchToStore,
    resBuildResult = 10110,
    /**
     * Emitted via the JSON `result()` overload: an object describing
     * the response to an HTTP request, with the fields `httpStatus`
     * (the status code, absent for non-HTTP protocols) and `bodySize`
     * (the number of body bytes received). More fields may be added
     * in the future.
     */
    resHttpStatus = 10111,
    /**
     * Progress of a garbage collection (the `CollectGarbage`
     * activity), as absolute counts: the number of paths found to be
     * alive so far, the number of paths deleted (or, in a dry run,
     * found to be dead) so far, and the number of bytes freed so far.
     */
    resGCProgress = 10112,
    resHashMismatch = 10113,
} ResultType;

/**
 * Whether `raw` (e.g. received from an untrusted source such as a
 * builder) denotes a known activity type.
 */
inline bool isKnownActivityType(uint64_t raw)
{
    return raw == actUnknown || (raw >= actCopyPath && raw <= actLast) || raw == actStringly;
}

/**
 * Whether `raw` denotes a known result type.
 */
inline bool isKnownResultType(uint64_t raw)
{
    return (raw >= resFileLinked && raw <= resLast) || (raw >= resBuildResult && raw <= resHashMismatch);
}

typedef uint64_t ActivityId;

class LoggerSettings : public Config
{
    void anchor() override;

public:
    Setting<bool> showTrace{
        this,
        false,
        "show-trace",
        R"(
          Whether Nix should print out a stack trace in case of Nix
          expression evaluation errors.
        )"};

    Setting<std::optional<AbsolutePath>> jsonLogPath{
        this,
        {},
        "json-log-path",
        R"(
          A file or Unix domain socket to which JSON records of Nix's log output are
          written, in the same format as `--log-format internal-json`
          (without the `@nix ` prefixes on each line).
          Concurrent writes to the same file by multiple Nix processes are not supported and
          may result in interleaved or corrupted log records.
        )"};

    Setting<std::string> sessionId{
        this,
        "",
        "session-id",
        R"(
          An identifier for the current Nix session, which is included in JSON log output to
          allow grouping of log messages from the same session. This defaults to a random UUID.
        )"};
};

extern LoggerSettings loggerSettings;

class Logger
{
    friend struct Activity;

public:
    /* Note that there are no members and invariants in the class itself
       inheriting is fine, because slicing won't post any issues. There are
       public constructors for implicit conversions and convenience. */
    struct Field : public std::variant<uint64_t, std::string>
    {
        Field(const std::string & s)
            : variant(s)
        {
        }

        Field(std::string && s)
            : variant(std::move(s))
        {
        }

        Field(const char * s)
            : variant(s)
        {
        }

        Field(uint64_t i)
            : variant(i)
        {
        }
    };

    /**
     * Key/value meta-information about a string-named activity, cf.
     * the name-based `startActivity()`.
     */
    using ActivityMetadata = std::span<const std::pair<std::string_view, Field>>;

    virtual ~Logger();

    virtual void stop() {};

    /**
     * Flush any buffered state to its final destination. Loggers that
     * upload to a remote service (such as the OpenTelemetry logger) use
     * this to perform the upload on exit.
     */
    virtual void flush() {};

    /**
     * Guard object to resume the logger when done.
     */
    struct Suspension
    {
        Finally<fun<void()>> _finalize;
    };

    Suspension suspend();

    std::optional<Suspension> suspendIf(bool cond);

    virtual void pause() {};
    virtual void resume() {};

    // Whether the logger prints the whole build log
    virtual bool isVerbose()
    {
        return false;
    }

    /* Note: logging functions must be noexcept, since they're often
       called in contexts where exceptions cannot be handled (such as
       in completion callbacks or destructors). Implementations should
       handle failure to write the message (e.g. by ignoring it or
       disabling the logger). */
    virtual void log(Verbosity lvl, std::string_view s) noexcept = 0;

    void log(std::string_view s) noexcept
    {
        log(lvlInfo, s);
    }

    virtual void logEI(const ErrorInfo & ei) noexcept = 0;

    /**
     * Report the exception that terminated the program (see
     * `handleExceptions()`). The default implementation prints it;
     * loggers that report to a monitoring system can override this to
     * record it there as well (or instead).
     */
    virtual void printException(const std::exception_ptr & ex, std::string_view programName) noexcept;

    void logEI(Verbosity lvl, ErrorInfo ei) noexcept
    {
        ei.level = lvl;
        logEI(ei);
    }

    virtual void warn(const std::string & msg) noexcept;

    virtual void startActivity(
        ActivityId act,
        Verbosity lvl,
        ActivityType type,
        const std::string & s,
        std::span<const Field> fields,
        ActivityId parent) noexcept
    {
    }

    /**
     * Start a string-named activity carrying key/value
     * meta-information. The default implementation reports it via the
     * `ActivityType`-based overload as `actStringly`, discarding the
     * name and metadata, so legacy loggers work unchanged. Metadata
     * keys follow the OpenTelemetry attribute naming conventions
     * where applicable (e.g. `http.request.method`); Nix-specific
     * keys use a `nix.` prefix.
     */
    virtual void startActivity(
        ActivityId act,
        Verbosity lvl,
        std::string_view name,
        ActivityMetadata metadata,
        std::string_view s,
        ActivityId parent) noexcept
    {
        startActivity(act, lvl, actStringly, std::string(s), {}, parent);
    };

    virtual void stopActivity(ActivityId act) noexcept {};

    virtual void result(ActivityId act, ResultType type, std::span<const Field> fields) noexcept {}

    virtual void result(ActivityId act, ResultType type, const nlohmann::json & json) noexcept {}

    /**
     * Return distributed tracing context for the given activity as
     * W3C Trace Context headers (`traceparent`, `tracestate`), for
     * propagation to another process (e.g. as HTTP request headers).
     * `act == 0` denotes the root context of this process. Returns an
     * empty list if this logger does not do tracing or has no context
     * for `act`.
     */
    virtual Headers getTraceContext(ActivityId act)
    {
        return {};
    }

    virtual void writeToStdout(std::string_view s);

    template<typename... Args>
    inline void cout(const Args &... args)
    {
        writeToStdout(fmt(args...));
    }

    /**
     * Write text styled with ANSI escape sequences to standard output,
     * dropping the sequences unless `shouldANSI(StandardOutputStream::Stdout)`.
     */
    void writeStyledToStdout(std::string_view s);

    template<typename... Args>
    inline void coutStyled(const Args &... args)
    {
        writeStyledToStdout(fmt(args...));
    }

    virtual std::optional<char> ask(std::string_view s)
    {
        return {};
    }

    virtual void setPrintBuildLogs(bool printBuildLogs) {}
};

ActivityId getCurActivity();
void setCurActivity(const ActivityId activityId);

struct Activity
{
    Logger & logger;

    const ActivityId id;

    Activity(
        Logger & logger,
        Verbosity lvl,
        ActivityType type,
        const std::string & s = "",
        std::span<const Logger::Field> fields = {},
        ActivityId parent = getCurActivity());

    Activity(
        Logger & logger,
        ActivityType type,
        std::span<const Logger::Field> fields = {},
        ActivityId parent = getCurActivity())
        : Activity(logger, lvlError, type, "", fields, parent)
    {
    }

    /**
     * Start a string-named activity carrying key/value
     * meta-information, cf. the name-based `Logger::startActivity()`.
     */
    Activity(
        Logger & logger,
        Verbosity lvl,
        std::string_view name,
        Logger::ActivityMetadata metadata = {},
        std::string_view s = {},
        ActivityId parent = getCurActivity());

    Activity(const Activity & act) = delete;

    ~Activity();

    void progress(uint64_t done = 0, uint64_t expected = 0, uint64_t running = 0, uint64_t failed = 0) const
    {
        result(resProgress, done, expected, running, failed);
    }

    void setExpected(ActivityType type2, uint64_t expected) const
    {
        result(resSetExpected, type2, expected);
    }

    void result(ResultType type, const nlohmann::json & json) const
    {
        logger.result(id, type, json);
    }

    template<typename... Args>
        requires(!(std::is_constructible_v<std::span<const Logger::Field>, std::remove_cvref_t<Args>> || ...))
    void result(ResultType type, Args &&... args) const
    {
        std::array<Logger::Field, sizeof...(args)> fields = {std::forward<Args>(args)...};
        result(type, fields);
    }

    void result(ResultType type, std::span<const Logger::Field> fields) const
    {
        logger.result(id, type, fields);
    }

    friend class Logger;
};

struct PushActivity
{
    const ActivityId prevAct;

    PushActivity(ActivityId act)
        : prevAct(getCurActivity())
    {
        setCurActivity(act);
    }

    ~PushActivity()
    {
        setCurActivity(prevAct);
    }
};

extern Logger * logger;

std::unique_ptr<Logger> makeSimpleLogger(bool printBuildLogs = true);

std::unique_ptr<Logger> makeJSONLogger(Descriptor fd, bool includeNixPrefix = true);

std::unique_ptr<Logger> makeJSONLogger(const std::filesystem::path & path, bool includeNixPrefix = true);

/**
 * Add an additional logger to the global `logger` by combining them
 * into a `TeeLogger`. The current logger keeps responsibility for
 * stdout and user interaction.
 */
void applyExtraLogger(std::unique_ptr<Logger> extraLogger);

void applyJSONLogger();

/**
 * Extract the value of the `traceparent` header from trace context
 * headers returned by `Logger::getTraceContext()`, or the empty
 * string if there is none.
 */
std::string getTraceparent(const Headers & headers);

/**
 * Marks, for the duration of its existence, Logger calls made on this
 * thread as replaying messages that originated in another process
 * (e.g. activities forwarded from the daemon to its client). Loggers
 * that export telemetry should ignore such messages, since the
 * originating process is responsible for exporting them.
 */
struct RemoteLogSource
{
    RemoteLogSource();
    ~RemoteLogSource();
};

/**
 * Whether Logger calls on this thread are currently replaying
 * messages from another process, cf. `RemoteLogSource`.
 */
bool isRemoteLogSource();

/**
 * @param source A noun phrase describing the source of the message, e.g. "the builder".
 */
std::optional<nlohmann::json> parseJSONMessage(std::string_view msg, std::string_view source);

/**
 * @param source A noun phrase describing the source of the message, e.g. "the builder".
 */
bool handleJSONLogMessage(
    const nlohmann::json & json,
    const Activity & act,
    std::map<ActivityId, Activity> & activities,
    std::string_view source,
    bool trusted);

/**
 * @param source A noun phrase describing the source of the message, e.g. "the builder".
 */
bool handleJSONLogMessage(
    std::string_view msg,
    const Activity & act,
    std::map<ActivityId, Activity> & activities,
    std::string_view source,
    bool trusted);

inline bool handleJSONLogMessage(
    const std::string & msg,
    const Activity & act,
    std::map<ActivityId, Activity> & activities,
    std::string_view source,
    bool trusted)
{
    return handleJSONLogMessage(std::string_view(msg), act, activities, source, trusted);
}

/**
 * suppress msgs > this
 */
extern Verbosity verbosity;

/**
 * Print a message with the standard ErrorInfo format.
 * In general, use these 'log' macros for reporting problems that may require user
 * intervention or that need more explanation.  Use the 'print' macros for more
 * lightweight status messages.
 */
#define logErrorInfo(level, errorInfo...)      \
    do {                                       \
        if ((level) <= nix::verbosity) {       \
            logger->logEI((level), errorInfo); \
        }                                      \
    } while (0)

#define logError(errorInfo...) logErrorInfo(lvlError, errorInfo)
#define logWarning(errorInfo...) logErrorInfo(lvlWarn, errorInfo)

/**
 * Print a string message if the current log level is at least the specified
 * level. Note that this has to be implemented as a macro to ensure that the
 * arguments are evaluated lazily.
 */
#define printMsgUsing(loggerParam, level, args...) \
    do {                                           \
        auto __lvl = level;                        \
        if (__lvl <= nix::verbosity) {             \
            loggerParam->log(__lvl, fmt(args));    \
        }                                          \
    } while (0)
#define printMsg(level, args...) printMsgUsing(logger, level, args)

#define printError(args...) printMsg(lvlError, args)
#define notice(args...) printMsg(lvlNotice, args)
#define printInfo(args...) printMsg(lvlInfo, args)
#define printTalkative(args...) printMsg(lvlTalkative, args)
#define debug(args...) printMsg(lvlDebug, args)
#define vomit(args...) printMsg(lvlVomit, args)

/**
 * if verbosity >= lvlWarn, print a message with a yellow 'warning:' prefix.
 */
template<typename... Args>
inline void warn(const std::string & fs, const Args &... args)
{
    boost::format f(fs);
    formatHelper(f, args...);
    logger->warn(f.str());
}

#define warnOnce(haveWarned, args...) \
    if (!haveWarned) {                \
        haveWarned = true;            \
        warn(args);                   \
    }

void writeToStderr(std::string_view s) noexcept;

} // namespace nix
