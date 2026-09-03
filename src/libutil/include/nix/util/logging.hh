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

#include <nlohmann/json_fwd.hpp>

namespace nix {

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
     * The type under which string-named activities (created via the
     * name-based `Activity` constructor) are reported on legacy code
     * paths, cf. the name-based `Logger::startActivity()`. Do not use
     * directly.
     */
    actStringly = 113,
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
    resHashMismatch = 109,
    resBuildResult = 110,
    /**
     * Emitted via the JSON `result()` overload: an object describing
     * the response to an HTTP request, with the fields `httpStatus`
     * (the status code, absent for non-HTTP protocols) and `bodySize`
     * (the number of body bytes received). More fields may be added
     * in the future.
     */
    resHttpStatus = 111,
} ResultType;

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

    struct Field
    {
        std::variant<std::string, uint64_t> raw;

        Field(const std::string & s)
            : raw(s)
        {
        }

        Field(const char * s)
            : raw(std::string(s))
        {
        }

        Field(const uint64_t & i)
            : raw(i)
        {
        }
    };

    typedef std::vector<Field> Fields;

    /**
     * Key/value meta-information about a string-named activity, cf.
     * the name-based `startActivity()`.
     */
    using ActivityMetadata = std::span<const std::pair<std::string_view, Field>>;

    virtual ~Logger();

    virtual void stop() {};

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
        const Fields & fields,
        ActivityId parent) noexcept {};

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

    virtual void result(ActivityId act, ResultType type, const Fields & fields) noexcept {};

    virtual void result(ActivityId act, ResultType type, const nlohmann::json & json) noexcept {};

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

    virtual std::optional<char> ask(std::string_view s)
    {
        return {};
    }

    virtual void setPrintBuildLogs(bool printBuildLogs) {}
};

/**
 * A variadic template that does nothing.
 *
 * Useful to call a function with each argument in a parameter pack.
 */
struct nop
{
    template<typename... T>
    nop(T...)
    {
    }
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
        const Logger::Fields & fields = {},
        ActivityId parent = getCurActivity());

    Activity(
        Logger & logger, ActivityType type, const Logger::Fields & fields = {}, ActivityId parent = getCurActivity())
        : Activity(logger, lvlError, type, "", fields, parent) {};

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
    void result(ResultType type, const Args &... args) const
    {
        Logger::Fields fields;
        nop{(fields.emplace_back(Logger::Field(args)), 1)...};
        result(type, fields);
    }

    void result(ResultType type, const Logger::Fields & fields) const
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
std::optional<nlohmann::json> parseJSONMessage(const std::string & msg, std::string_view source);

/**
 * @param source A noun phrase describing the source of the message, e.g. "the builder".
 */
bool handleJSONLogMessage(
    nlohmann::json & json,
    const Activity & act,
    std::map<ActivityId, Activity> & activities,
    std::string_view source,
    bool trusted);

/**
 * @param source A noun phrase describing the source of the message, e.g. "the builder".
 */
bool handleJSONLogMessage(
    const std::string & msg,
    const Activity & act,
    std::map<ActivityId, Activity> & activities,
    std::string_view source,
    bool trusted);

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
