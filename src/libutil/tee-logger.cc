#include "nix/util/logging.hh"

namespace nix {

namespace {

class TeeLogger final : public Logger
{
    std::vector<std::unique_ptr<Logger>> loggers;

public:
    TeeLogger(std::vector<std::unique_ptr<Logger>> && loggers)
        : loggers(std::move(loggers))
    {
    }

    void stop() override
    {
        for (auto & logger : loggers)
            logger->stop();
    };

    void pause() override
    {
        for (auto & logger : loggers)
            logger->pause();
    };

    void resume() override
    {
        for (auto & logger : loggers)
            logger->resume();
    };

    void log(Verbosity lvl, std::string_view s) noexcept override
    {
        for (auto & logger : loggers)
            logger->log(lvl, s);
    }

    void logEI(const ErrorInfo & ei) noexcept override
    {
        for (auto & logger : loggers)
            logger->logEI(ei);
    }

    void startActivity(
        ActivityId act,
        Verbosity lvl,
        ActivityType type,
        const std::string & s,
        const Fields & fields,
        ActivityId parent) noexcept override
    {
        for (auto & logger : loggers)
            logger->startActivity(act, lvl, type, s, fields, parent);
    }

    void stopActivity(ActivityId act) noexcept override
    {
        for (auto & logger : loggers)
            logger->stopActivity(act);
    }

    void result(ActivityId act, ResultType type, const Fields & fields) noexcept override
    {
        for (auto & logger : loggers)
            logger->result(act, type, fields);
    }

    void result(ActivityId act, ResultType type, const nlohmann::json & json) noexcept override
    {
        for (auto & logger : loggers)
            logger->result(act, type, json);
    }

    Headers getTraceContext(ActivityId act) override
    {
        for (auto & logger : loggers) {
            auto headers = logger->getTraceContext(act);
            if (!headers.empty())
                return headers;
        }
        return {};
    }

    void addLogger(std::unique_ptr<Logger> logger)
    {
        loggers.push_back(std::move(logger));
    }

    void writeToStdout(std::string_view s) override
    {
        for (auto & logger : loggers) {
            /* Let only the first logger write to stdout to avoid
               duplication. This means that the first logger needs to
               be the one managing stdout/stderr
               (e.g. `ProgressBar`). */
            logger->writeToStdout(s);
            break;
        }
    }

    std::optional<char> ask(std::string_view s) override
    {
        for (auto & logger : loggers) {
            auto c = logger->ask(s);
            if (c)
                return c;
        }
        return std::nullopt;
    }

    void setPrintBuildLogs(bool printBuildLogs) override
    {
        for (auto & logger : loggers)
            logger->setPrintBuildLogs(printBuildLogs);
    }
};

} // namespace

std::unique_ptr<Logger>
makeTeeLogger(std::unique_ptr<Logger> mainLogger, std::vector<std::unique_ptr<Logger>> && extraLoggers)
{
    std::vector<std::unique_ptr<Logger>> allLoggers;
    allLoggers.push_back(std::move(mainLogger));
    for (auto & l : extraLoggers)
        allLoggers.push_back(std::move(l));
    return std::make_unique<TeeLogger>(std::move(allLoggers));
}

void applyExtraLogger(std::unique_ptr<Logger> extraLogger)
{
    if (auto teeLogger = dynamic_cast<TeeLogger *>(logger))
        teeLogger->addLogger(std::move(extraLogger));
    else {
        std::vector<std::unique_ptr<Logger>> loggers;
        loggers.push_back(std::move(extraLogger));
        try {
            logger = makeTeeLogger(std::unique_ptr<Logger>(logger), std::move(loggers)).release();
        } catch (...) {
            // `logger` is now gone so give up.
            abort();
        }
    }
}

} // namespace nix
