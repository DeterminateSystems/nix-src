#include "nix/main/progress-bar.hh"
#include "nix/util/terminal.hh"
#include "nix/util/sync.hh"
#include "nix/util/signals.hh"
#include "nix/store/path.hh"
#include "nix/util/file-system.hh"
#include "nix/store/names.hh"
#include "nix/util/util.hh"

#include <map>
#include <thread>
#include <sstream>
#include <iostream>
#include <chrono>

namespace nix {

namespace {

static std::string_view getS(const std::vector<Logger::Field> & fields, size_t n)
{
    if (n < fields.size()) {
        if (auto p = std::get_if<std::string>(&fields[n].raw))
            return *p;
    }
    throw Error("could not get expected log field of type 'string' at index %d", n);
}

static uint64_t getI(const std::vector<Logger::Field> & fields, size_t n)
{
    if (n < fields.size()) {
        if (auto p = std::get_if<uint64_t>(&fields[n].raw))
            return *p;
    }
    throw Error("could not get expected log field of type 'int' at index %d", n);
}

static std::string_view storePathToName(std::string_view path)
{
    auto base = baseNameOf(path);
    auto i = base.find('-');
    return i == std::string::npos ? base.substr(0, 0) : base.substr(i + 1);
}

static std::string_view storePathToNameWithoutDrvSuffix(std::string_view path)
{
    auto res = storePathToName(path);
    if (hasSuffix(res, drvExtension))
        res.remove_suffix(drvExtension.size());
    return res;
}

/**
 * Base class for loggers that keep track of the running activities
 * and display their progress in a "frame" of one or more lines at the
 * bottom of the terminal. Log messages are printed above the frame.
 * Subclasses only need to implement `renderFrame()`.
 *
 * Subclasses must call `start()` after construction, and `stop()` in
 * their destructor (since the update thread calls `renderFrame()`).
 */
class ProgressBarBase : public Logger
{
protected:
    struct ActInfo
    {
        std::string s, lastLine, phase;
        ActivityType type = actUnknown;
        uint64_t done = 0;
        uint64_t expected = 0;
        uint64_t running = 0;
        uint64_t failed = 0;
        std::map<ActivityType, uint64_t> expectedByType;
        bool visible = true;
        ActivityId parent;
        std::optional<std::string> name;
        std::chrono::time_point<std::chrono::steady_clock> startTime;
        bool logged = false;
    };

    struct ActivitiesByType
    {
        std::map<ActivityId, std::list<ActInfo>::iterator> its;
        uint64_t done = 0;
        uint64_t expected = 0;
        uint64_t failed = 0;
    };

    struct State
    {
        std::list<ActInfo> activities;
        std::map<ActivityId, std::list<ActInfo>::iterator> its;

        std::map<ActivityType, ActivitiesByType> activitiesByType;

        uint64_t filesLinked = 0, bytesLinked = 0;

        uint64_t corruptedPaths = 0, untrustedPaths = 0;

        bool active = true;
        size_t suspensions = 0;
        bool haveUpdate = true;

        bool isPaused() const
        {
            return suspensions > 0;
        }
    };

    /**
     * What we last wrote to the terminal.
     */
    struct Screen
    {
        /**
         * The frame last written, used to avoid unnecessary redraws
         * (see `redraw()`). `std::nullopt` forces a redraw.
         */
        std::optional<std::string> lastFrame;

        /**
         * The number of lines of the frame currently on the
         * terminal. The cursor is on the last of these lines.
         */
        size_t linesShown = 0;
    };

    Sync<Screen> screen_;

    Sync<State> state_;

    std::thread updateThread;

    std::condition_variable quitCV, updateCV;

    bool printBuildLogs = false;
    bool isTTY;

    std::unique_ptr<InterruptCallback> interruptCallback, stopCallback, contCallback, winchCallback;

    void hideCursorIfNeeded() const
    {
        if (isTTY)
            writeToStderr("\e[?25l");
    }

    void unhideCursorIfNeeded() const
    {
        if (isTTY)
            writeToStderr("\e[?25h");
    }

    ProgressBarBase(bool isTTY)
        : isTTY(isTTY)
    {
        state_.lock()->active = isTTY;
    }

    /**
     * Render the frame to be displayed, one string per line. Lines
     * are truncated to the terminal width by the caller. Set
     * `nextWakeup` to request a redraw after some time, even if
     * there are no activity updates.
     */
    virtual std::vector<std::string>
    renderFrame(State & state, std::chrono::steady_clock::time_point now, std::chrono::milliseconds & nextWakeup) = 0;

public:

    /**
     * Start the update thread and register signal callbacks. This
     * can't be done in the constructor since the update thread calls
     * `renderFrame()`.
     */
    void start()
    {
        hideCursorIfNeeded();

        interruptCallback = createInterruptCallback([&]() {
            /* Don't print the current activities, since we're about
               to abort them anyway. */
            pause(false);
            redraw({"shutting down"});
        });

        /* On Ctrl-Z, unhide the cursor before the process is
           stopped. */
        stopCallback = createSignalCallback(SignalType::Stop, [&]() {
            auto state(state_.lock());
            if (state->active && !state->isPaused())
                unhideCursorIfNeeded();
        });

        /* Redraw the progress bar when the process is resumed or the
           terminal size changes. */
        auto redrawCallback = [&]() {
            auto state(state_.lock());
            if (state->active && !state->isPaused()) {
                /* Force a redraw, since the new output may be
                   identical to the one cached by redraw(). */
                invalidateRedrawCache();
                hideCursorIfNeeded();
                update(*state);
            }
        };
        contCallback = createSignalCallback(SignalType::Cont, redrawCallback);
        winchCallback = createSignalCallback(SignalType::Winch, redrawCallback);

        updateThread = std::thread([&]() {
            auto state(state_.lock());
            auto nextWakeup = std::chrono::milliseconds::max();
            while (state->active) {
                if (!state->haveUpdate)
                    state.wait_for(updateCV, nextWakeup);
                nextWakeup = draw(*state);
                state.wait_for(quitCV, std::chrono::milliseconds(50));
            }
        });
    }

    ~ProgressBarBase()
    {
        stop();
    }

    /* Called by destructor, can't be overridden */
    void stop() override final
    {
        {
            auto state(state_.lock());
            if (state->active) {
                state->active = false;
                clearProgressDisplay();
                unhideCursorIfNeeded();
                updateCV.notify_one();
                quitCV.notify_one();
            }
        }
        if (updateThread.joinable())
            updateThread.join();
    }

    void pause() override
    {
        pause(true);
    }

    void pause(bool showActivities)
    {
        auto state(state_.lock());
        state->suspensions++;
        if (state->suspensions > 1) {
            // already paused
            return;
        }

        if (state->active) {
            clearProgressDisplay();
            /* Show activities that were previously only shown on the
               progress bar. Otherwise the user won't know what's
               happening. */
            if (showActivities)
                for (auto & act : state->activities)
                    logActivity(*state, lvlNotice, act);
            unhideCursorIfNeeded();
        }
    }

    void resume() override
    {
        auto state(state_.lock());
        if (state->suspensions == 0) {
            log(lvlError, "nix::ProgressBar: resume() called without a matching preceding pause(). This is a bug.");
            return;
        } else {
            state->suspensions--;
        }
        if (state->suspensions == 0) {
            if (state->active) {
                clearProgressDisplay();
                hideCursorIfNeeded();
            }
            state->haveUpdate = true;
            updateCV.notify_one();
        }
    }

    bool isVerbose() override
    {
        return printBuildLogs;
    }

    void log(Verbosity lvl, std::string_view s) noexcept override
    {
        if (lvl > verbosity)
            return;
        auto state(state_.lock());
        log(*state, lvl, s);
    }

    void logEI(const ErrorInfo & ei) noexcept override
    {
        auto state(state_.lock());

        std::ostringstream oss;
        showErrorInfo(oss, ei, loggerSettings.showTrace.get());

        log(*state, ei.level, oss.view());
    }

    void log(State & state, Verbosity lvl, std::string_view s) noexcept
    {
        if (state.active) {
            clearProgressDisplay(filterANSIEscapes(s, !isTTY) + ANSI_NORMAL "\n");
            draw(state);
        } else {
            writeToStderr(filterANSIEscapes(s, !isTTY) + "\n");
        }
    }

    void logActivity(State & state, Verbosity lvl, ActInfo & act) noexcept
    {
        if (!act.logged && lvl <= verbosity && !act.s.empty() && act.type != actBuildWaiting) {
            log(state, lvl, act.s + "...");
            act.logged = true;
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
        auto state(state_.lock());

        state->activities.emplace_back(
            ActInfo{.s = s, .type = type, .parent = parent, .startTime = std::chrono::steady_clock::now()});
        auto i = std::prev(state->activities.end());
        state->its.emplace(act, i);
        state->activitiesByType[type].its.emplace(act, i);

        logActivity(*state, lvl, *i);

        if (type == actBuild) {
            auto name = storePathToNameWithoutDrvSuffix(getS(fields, 0));
            i->s = fmt("building " ANSI_BOLD "%s" ANSI_NORMAL, name);
            auto machineName = getS(fields, 1);
            if (machineName != "")
                i->s += fmt(" on " ANSI_BOLD "%s" ANSI_NORMAL, machineName);
            i->name = DrvName(name).name;
        }

        if (type == actSubstitute) {
            auto name = storePathToName(getS(fields, 0));
            auto sub = getS(fields, 1);
            i->s =
                fmt(hasPrefix(sub, "local") ? "copying " ANSI_BOLD "%s" ANSI_NORMAL " from %s"
                                            : "fetching " ANSI_BOLD "%s" ANSI_NORMAL " from %s",
                    name,
                    sub);
        }

        if (type == actPostBuildHook) {
            auto name = storePathToNameWithoutDrvSuffix(getS(fields, 0));
            i->s = fmt("post-build " ANSI_BOLD "%s" ANSI_NORMAL, name);
            i->name = DrvName(name).name;
        }

        if (type == actQueryPathInfo) {
            auto name = storePathToName(getS(fields, 0));
            i->s = fmt("querying " ANSI_BOLD "%s" ANSI_NORMAL " on %s", name, getS(fields, 1));
        }

        if ((type == actFileTransfer && hasAncestor(*state, actCopyPath, parent))
            || (type == actFileTransfer && hasAncestor(*state, actQueryPathInfo, parent))
            || (type == actCopyPath && hasAncestor(*state, actSubstitute, parent)))
            i->visible = false;

        update(*state);
    }

    /* Check whether an activity has an ancestor with the specified
       type. */
    bool hasAncestor(State & state, ActivityType type, ActivityId act)
    {
        while (act != 0) {
            auto i = state.its.find(act);
            if (i == state.its.end())
                break;
            if (i->second->type == type)
                return true;
            act = i->second->parent;
        }
        return false;
    }

    void stopActivity(ActivityId act) noexcept override
    {
        auto state(state_.lock());

        auto i = state->its.find(act);
        if (i != state->its.end()) {

            auto & actByType = state->activitiesByType[i->second->type];
            actByType.done += i->second->done;
            actByType.failed += i->second->failed;

            for (auto & j : i->second->expectedByType)
                state->activitiesByType[j.first].expected -= j.second;

            actByType.its.erase(act);
            state->activities.erase(i->second);
            state->its.erase(i);
        }

        update(*state);
    }

    void result(ActivityId act, ResultType type, const std::vector<Field> & fields) noexcept override
    {
        auto state(state_.lock());

        if (type == resFileLinked) {
            state->filesLinked++;
            state->bytesLinked += getI(fields, 0);
            update(*state);
        }

        else if (type == resBuildLogLine || type == resPostBuildLogLine) {
            auto lastLine = chomp(getS(fields, 0));
            auto i = state->its.find(act);
            assert(i != state->its.end());
            ActInfo & info = *i->second;
            if (printBuildLogs) {
                auto suffix = "> ";
                if (type == resPostBuildLogLine) {
                    suffix = " (post)> ";
                }
                log(*state, lvlInfo, ANSI_FAINT + info.name.value_or("unnamed") + suffix + ANSI_NORMAL + lastLine);
            } else {
                /* Move this activity to the end of the list, making it
                   the most recent one. Note that splice() doesn't
                   invalidate iterators. */
                state->activities.splice(state->activities.end(), state->activities, i->second);
                info.lastLine = lastLine;
                update(*state);
            }
        }

        else if (type == resUntrustedPath) {
            state->untrustedPaths++;
            update(*state);
        }

        else if (type == resCorruptedPath) {
            state->corruptedPaths++;
            update(*state);
        }

        else if (type == resSetPhase) {
            auto i = state->its.find(act);
            assert(i != state->its.end());
            i->second->phase = getS(fields, 0);
            update(*state);
        }

        else if (type == resProgress) {
            auto i = state->its.find(act);
            assert(i != state->its.end());
            ActInfo & actInfo = *i->second;
            actInfo.done = getI(fields, 0);
            actInfo.expected = getI(fields, 1);
            actInfo.running = getI(fields, 2);
            actInfo.failed = getI(fields, 3);
            update(*state);
        }

        else if (type == resSetExpected) {
            auto i = state->its.find(act);
            assert(i != state->its.end());
            ActInfo & actInfo = *i->second;
            auto type = (ActivityType) getI(fields, 0);
            auto & j = actInfo.expectedByType[type];
            state->activitiesByType[type].expected -= j;
            j = getI(fields, 1);
            state->activitiesByType[type].expected += j;
            update(*state);
        }

        else if (type == resFetchStatus) {
            auto i = state->its.find(act);
            assert(i != state->its.end());
            ActInfo & actInfo = *i->second;
            actInfo.lastLine = getS(fields, 0);
            update(*state);
        }
    }

    void update(State & state) noexcept
    {
        state.haveUpdate = true;
        updateCV.notify_one();
    }

    /**
     * Replace the frame currently on the terminal by `lines`, if it
     * has changed.
     *
     * Excessive redrawing is noticeable on slow terminals, and it interferes
     * with text selection in some terminals, including libvte-based terminal
     * emulators.
     */
    void redraw(const std::vector<std::string> & lines) noexcept
    {
        std::string frame;
        for (const auto & [n, line] : enumerate(lines)) {
            if (n)
                frame += "\n";
            /* Reset attributes and erase the rest of the line. */
            frame += line + ANSI_NORMAL "\e[K";
        }

        auto screen(screen_.lock());
        if (frame == screen->lastFrame)
            return;

        auto s = cursorToFrameStart(screen->linesShown) + frame;
        /* If the new frame is shorter, erase from the cursor to the
           end of the screen to remove the remainder of the old
           frame. */
        if (lines.size() < screen->linesShown)
            s += "\e[J";
        writeToStderr(s);

        screen->lastFrame = std::move(frame);
        screen->linesShown = lines.size();
    }

    /**
     * Return the escape sequence that moves the cursor to the start of
     * the first line of a frame of `linesShown` lines.
     */
    static std::string cursorToFrameStart(size_t linesShown)
    {
        /* Move the cursor up N lines (`\e[<N>A`), then to the start of
           the line. */
        return linesShown > 1 ? fmt("\e[%dA\r", linesShown - 1) : "\r";
    }

    void invalidateRedrawCache()
    {
        screen_.lock()->lastFrame.reset();
    }

    /**
     * Erase the frame, leaving the cursor at the start of an empty
     * line, then print `s` there.
     */
    void clearProgressDisplay(std::string_view s = "")
    {
        auto screen(screen_.lock());
        writeToStderr(
            (screen->linesShown > 1 ? cursorToFrameStart(screen->linesShown) + "\e[J" : std::string("\r\e[K"))
            + std::string(s));
        screen->lastFrame.reset();
        screen->linesShown = 0;
    }

    std::chrono::milliseconds draw(State & state) noexcept
    {
        auto nextWakeup = std::chrono::milliseconds::max();

        state.haveUpdate = false;
        if (state.isPaused() || !state.active)
            return nextWakeup;

        auto lines = renderFrame(state, std::chrono::steady_clock::now(), nextWakeup);

        /* Don't draw more lines than fit in the terminal, since then
           we can't move the cursor back to the start of the frame. */
        auto rows = getWindowSize().first;
        if (rows > 1 && lines.size() >= rows)
            lines.resize(rows - 1);

        /* Truncate lines to the terminal width. In multi-line frames,
           leave the last column empty: writing a newline after a line
           that fills the entire width causes some terminals to move
           the cursor down two lines (once to wrap, once for the
           newline), so we would lose track of the cursor position. */
        auto width = getWindowWidth();
        if (lines.size() > 1 && width > 1 && width != std::numeric_limits<decltype(width)>::max())
            width--;
        for (auto & line : lines)
            line = filterANSIEscapes(line, false, width);

        redraw(lines);

        return nextWakeup;
    }

    struct ActivityStats
    {
        uint64_t done = 0;
        uint64_t expected = 0;
        uint64_t running = 0;
        uint64_t failed = 0;
    };

    /**
     * Get the aggregate progress of all activities of type `type`,
     * including those that have finished.
     */
    static ActivityStats getActivityStats(State & state, ActivityType type)
    {
        auto & act = state.activitiesByType[type];
        ActivityStats stats{.done = act.done, .expected = act.done, .failed = act.failed};
        for (auto & j : act.its) {
            stats.done += j.second->done;
            stats.expected += j.second->expected;
            stats.running += j.second->running;
            stats.failed += j.second->failed;
        }
        stats.expected = std::max(stats.expected, act.expected);
        return stats;
    }

    void writeToStdout(std::string_view s) override
    {
        auto state(state_.lock());
        if (state->active) {
            clearProgressDisplay();
            Logger::writeToStdout(s);
            draw(*state);
        } else {
            Logger::writeToStdout(s);
        }
    }

    std::optional<char> ask(std::string_view msg) override
    {
        auto state(state_.lock());
        if (!state->active)
            return {};
        clearProgressDisplay(fmt("%s ", msg));
        unhideCursorIfNeeded();
        auto s = trim(readLine(getStandardInput(), true));
        hideCursorIfNeeded();
        if (s.size() != 1)
            return {};
        draw(*state);
        return s[0];
    }

    void setPrintBuildLogs(bool printBuildLogs) override
    {
        this->printBuildLogs = printBuildLogs;
    }
};

/**
 * The classic one-line progress bar, showing the number of builds,
 * downloads etc. and the most recently started activity.
 */
class ProgressBar final : public ProgressBarBase
{
public:

    ProgressBar(bool isTTY)
        : ProgressBarBase(isTTY)
    {
    }

    ~ProgressBar()
    {
        stop();
    }

private:

    std::vector<std::string> renderFrame(
        State & state, std::chrono::steady_clock::time_point now, std::chrono::milliseconds & nextWakeup) override
    {
        std::string line;

        std::string status = getStatus(state);
        if (!status.empty()) {
            line += '[';
            line += status;
            line += "]";
        }

        if (!state.activities.empty()) {
            if (!status.empty())
                line += " ";
            auto i = state.activities.rbegin();

            while (i != state.activities.rend()) {
                if (i->visible && (!i->s.empty() || !i->lastLine.empty())) {
                    /* Don't show activities until some time has
                       passed, to avoid displaying very short
                       activities. */
                    auto delay = std::chrono::milliseconds(10);
                    if (i->startTime + delay < now)
                        break;
                    else
                        nextWakeup = std::min(
                            nextWakeup,
                            std::chrono::duration_cast<std::chrono::milliseconds>(delay - (now - i->startTime)));
                }
                ++i;
            }

            if (i != state.activities.rend()) {
                line += i->s;
                if (!i->phase.empty()) {
                    line += " (";
                    line += i->phase;
                    line += ")";
                }
                if (!i->lastLine.empty()) {
                    if (!i->s.empty())
                        line += ": ";
                    line += i->lastLine;
                }
            }
        }

        return {std::move(line)};
    }

    std::string getStatus(State & state) noexcept
    {
        std::string res;

        auto renderActivity =
            [&] [[nodiscard]] (
                ActivityType type, const std::string & itemFmt, const std::string & numberFmt = "%d", double unit = 1) {
                auto [done, expected, running, failed] = getActivityStats(state, type);

                std::string s;

                if (running || done || expected || failed) {
                    if (running)
                        if (expected != 0)
                            s =
                                fmt(ANSI_BLUE + numberFmt + ANSI_NORMAL "/" ANSI_GREEN + numberFmt + ANSI_NORMAL "/"
                                        + numberFmt,
                                    running / unit,
                                    done / unit,
                                    expected / unit);
                        else
                            s =
                                fmt(ANSI_BLUE + numberFmt + ANSI_NORMAL "/" ANSI_GREEN + numberFmt + ANSI_NORMAL,
                                    running / unit,
                                    done / unit);
                    else if (expected != done)
                        if (expected != 0)
                            s = fmt(ANSI_GREEN + numberFmt + ANSI_NORMAL "/" + numberFmt, done / unit, expected / unit);
                        else
                            s = fmt(ANSI_GREEN + numberFmt + ANSI_NORMAL, done / unit);
                    else
                        s = fmt(done ? ANSI_GREEN + numberFmt + ANSI_NORMAL : numberFmt, done / unit);
                    s = fmt(itemFmt, s);

                    if (failed)
                        s += fmt(" (" ANSI_RED "%d failed" ANSI_NORMAL ")", failed / unit);
                }

                return s;
            };

        auto renderSizeActivity = [&] [[nodiscard]] (ActivityType type, const std::string & itemFmt = "%s") {
            auto [done, expected, running, failed] = getActivityStats(state, type);

            std::optional<SizeUnit> commonUnit;
            std::string s;

            if (running || done || expected || failed) {
                if (running)
                    if (expected != 0) {
                        commonUnit = getCommonSizeUnit({(int64_t) running, (int64_t) done, (int64_t) expected});
                        s =
                            fmt(ANSI_BLUE "%s" ANSI_NORMAL "/" ANSI_GREEN "%s" ANSI_NORMAL "/%s",
                                commonUnit ? renderSizeWithoutUnit(running, *commonUnit) : renderSize(running),
                                commonUnit ? renderSizeWithoutUnit(done, *commonUnit) : renderSize(done),
                                commonUnit ? renderSizeWithoutUnit(expected, *commonUnit) : renderSize(expected));
                    } else {
                        commonUnit = getCommonSizeUnit({(int64_t) running, (int64_t) done});
                        s =
                            fmt(ANSI_BLUE "%s" ANSI_NORMAL "/" ANSI_GREEN "%s" ANSI_NORMAL,
                                commonUnit ? renderSizeWithoutUnit(running, *commonUnit) : renderSize(running),
                                commonUnit ? renderSizeWithoutUnit(done, *commonUnit) : renderSize(done));
                    }
                else if (expected != done)
                    if (expected != 0) {
                        commonUnit = getCommonSizeUnit({(int64_t) done, (int64_t) expected});
                        s =
                            fmt(ANSI_GREEN "%s" ANSI_NORMAL "/%s",
                                commonUnit ? renderSizeWithoutUnit(done, *commonUnit) : renderSize(done),
                                commonUnit ? renderSizeWithoutUnit(expected, *commonUnit) : renderSize(expected));
                    } else {
                        commonUnit = getSizeUnit(done);
                        s = fmt(ANSI_GREEN "%s" ANSI_NORMAL, renderSizeWithoutUnit(done, *commonUnit));
                    }
                else {
                    commonUnit = getSizeUnit(done);
                    s = fmt(done ? ANSI_GREEN "%s" ANSI_NORMAL : "%s", renderSizeWithoutUnit(done, *commonUnit));
                }

                if (commonUnit)
                    s = fmt("%s %siB", s, getSizeUnitSuffix(*commonUnit));

                s = fmt(itemFmt, s);

                if (failed)
                    s += fmt(" (" ANSI_RED "%s failed" ANSI_NORMAL ")", renderSize(failed));
            }

            return s;
        };

        auto maybeAppendToResult = [&](std::string_view s) {
            if (s.empty())
                return;
            if (!res.empty())
                res += ", ";
            res += s;
        };

        auto showActivity =
            [&](ActivityType type, const std::string & itemFmt, const std::string & numberFmt = "%d", double unit = 1) {
                maybeAppendToResult(renderActivity(type, itemFmt, numberFmt, unit));
            };

        showActivity(actBuilds, "%s built");

        auto s1 = renderActivity(actCopyPaths, "%s copied");
        auto s2 = renderSizeActivity(actCopyPath);

        if (!s1.empty() || !s2.empty()) {
            if (!res.empty())
                res += ", ";
            if (s1.empty())
                res += "0 copied";
            else
                res += s1;
            if (!s2.empty()) {
                res += " (";
                res += s2;
                res += ')';
            }
        }

        maybeAppendToResult(renderSizeActivity(actFileTransfer, "%s DL"));

        {
            auto s = renderActivity(actOptimiseStore, "%s paths optimised");
            if (s != "") {
                s += fmt(", %s / %d inodes freed", renderSize(state.bytesLinked), state.filesLinked);
                if (!res.empty())
                    res += ", ";
                res += s;
            }
        }

        // FIXME: don't show "done" paths in green.
        showActivity(actVerifyPaths, "%s paths verified");

        if (state.corruptedPaths) {
            if (!res.empty())
                res += ", ";
            res += fmt(ANSI_RED "%d corrupted" ANSI_NORMAL, state.corruptedPaths);
        }

        if (state.untrustedPaths) {
            if (!res.empty())
                res += ", ";
            res += fmt(ANSI_RED "%d untrusted" ANSI_NORMAL, state.untrustedPaths);
        }

        return res;
    }
};

static std::string renderDuration(std::chrono::steady_clock::duration d)
{
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(d).count();
    if (secs < 60)
        return fmt("%ds", secs);
    if (secs < 3600)
        return fmt("%dm%02ds", secs / 60, secs % 60);
    return fmt("%dh%02dm", secs / 3600, (secs / 60) % 60);
}

static std::string repeat(std::string_view s, size_t n)
{
    std::string res;
    res.reserve(s.size() * n);
    for (size_t i = 0; i < n; ++i)
        res += s;
    return res;
}

/**
 * A multi-line progress indicator that shows a section with a
 * progress bar and a list of running items for each kind of work
 * (downloads, substitutions, builds, ...), plus a list of other
 * running activities.
 */
class ProgressIndicator final : public ProgressBarBase
{
    const std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();

    /**
     * Maximum number of running items to show per section.
     */
    static constexpr size_t maxItems = 5;

    /**
     * Don't show activities in the list of other activities until
     * they've been running for this long, to avoid flicker.
     */
    static constexpr auto otherActivityDelay = std::chrono::milliseconds(300);

    /**
     * Interval between spinner updates.
     */
    static constexpr auto spinnerInterval = std::chrono::milliseconds(100);

public:

    ProgressIndicator(bool isTTY)
        : ProgressBarBase(isTTY)
    {
    }

    ~ProgressIndicator()
    {
        stop();
    }

private:

    std::vector<std::string> renderFrame(
        State & state, std::chrono::steady_clock::time_point now, std::chrono::milliseconds & nextWakeup) override
    {
        std::vector<std::string> lines;

        static constexpr std::array<std::string_view, 10> spinner = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
        /* Note: the spinner is padded with a space, since the done
           and fail marks (the same as used by `nix flake check`) are
           two columns wide. */
        auto busyMark = ANSI_BOLD
                        + std::string(
                            spinner
                                [std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime)
                                 / spinnerInterval % spinner.size()])
                        + " " ANSI_NORMAL;
        auto doneMark = "✅";
        auto failMark = "❌";

        bool busy = false;

        auto width = getWindowWidth();
        size_t barWidth = width < 14 ? 10 : std::min(60u, width - 4);

        /* Render a progress bar showing the fraction of failed (red),
           done (green) and running (yellow) items. */
        auto renderBar = [&](uint64_t done, uint64_t failed, uint64_t running, uint64_t expected) {
            expected = std::max(expected, (uint64_t) 1);
            auto chars = [&](uint64_t n) { return (size_t) (barWidth * std::min((double) n / expected, 1.0)); };
            auto chars1 = chars(failed);
            auto chars2 = chars(failed + done);
            auto chars3 = chars(failed + done + running);
            return "  " ANSI_RED + repeat("█", chars1) + ANSI_GREEN + repeat("█", chars2 - chars1) + ANSI_WARNING
                   + repeat("▓", chars3 - chars2) + ANSI_NORMAL + repeat("░", barWidth - chars3);
        };

        auto renderSizes = [&](uint64_t done, uint64_t expected) {
            if (auto unit = getCommonSizeUnit({(int64_t) done, (int64_t) expected}))
                return fmt(
                    "%s/%s %siB",
                    renderSizeWithoutUnit(done, *unit),
                    renderSizeWithoutUnit(expected, *unit),
                    getSizeUnitSuffix(*unit));
            return fmt("%s/%s", renderSize(done), renderSize(expected));
        };

        /* Render the given activities, oldest first, up to
           `maxItems`. If `isActive` is given, show the activities
           for which it returns true first. */
        auto renderItems = [&](std::vector<ActInfo *> items,
                               auto renderItem,
                               std::function<bool(const ActInfo &)> isActive = nullptr) {
            std::ranges::sort(items, {}, &ActInfo::startTime);
            if (isActive)
                std::ranges::stable_partition(items, [&](ActInfo * act) { return isActive(*act); });
            for (auto [n, item] : enumerate(items)) {
                if (n == maxItems) {
                    lines.push_back(fmt(ANSI_FAINT "  … and %d more" ANSI_NORMAL, items.size() - maxItems));
                    break;
                }
                lines.push_back(renderItem(*item));
            }
        };

        auto getItems = [&](ActivityType type) {
            std::vector<ActInfo *> items;
            for (auto & [id, i] : state.activitiesByType[type].its)
                if (i->visible)
                    items.push_back(&*i);
            return items;
        };

        auto elapsed = [&](const ActInfo & act) {
            auto d = now - act.startTime;
            return d >= std::chrono::seconds(1) ? " " ANSI_FAINT + renderDuration(d) + ANSI_NORMAL : "";
        };

        /* Other activities (e.g. evaluation, fetching flake inputs)
           that don't have their own section. */
        {
            std::vector<ActInfo *> items;
            for (auto & act : state.activities) {
                if (!act.visible || act.s.empty() || act.type == actBuild || act.type == actSubstitute
                    || act.type == actFileTransfer || act.type == actCopyPath)
                    continue;
                if (act.startTime + otherActivityDelay > now) {
                    nextWakeup = std::min(
                        nextWakeup,
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            act.startTime + otherActivityDelay - now));
                    continue;
                }
                items.push_back(&act);
            }
            if (!items.empty())
                busy = true;
            renderItems(items, [&](const ActInfo & act) { return fmt("%s %s%s", busyMark, act.s, elapsed(act)); });
        }

        /* Downloads. Note that this includes the downloads done by
           substitutions, but only other downloads (e.g. flake
           inputs) are listed as items. */
        {
            auto stats = getActivityStats(state, actFileTransfer);
            if (stats.done || stats.expected) {
                auto items = getItems(actFileTransfer);
                bool active = !items.empty() || stats.done < stats.expected;
                busy |= active;
                lines.push_back(
                    fmt("%s Downloaded %s", active ? busyMark : doneMark, renderSizes(stats.done, stats.expected)));
                lines.push_back(renderBar(stats.done, 0, 0, stats.expected));
                renderItems(items, [&](const ActInfo & act) {
                    return fmt(
                        "  ‣ %s%s%s",
                        act.s,
                        act.expected ? " " + renderSizes(act.done, act.expected) : "",
                        elapsed(act));
                });
            }
        }

        /* Substitutions. */
        {
            auto paths = getActivityStats(state, actCopyPaths);
            auto bytes = getActivityStats(state, actCopyPath);
            if (paths.done || paths.expected) {
                auto items = getItems(actSubstitute);
                bool active = paths.running || paths.done < paths.expected;
                busy |= active;
                lines.push_back(
                    fmt("%s Fetched %d/%d paths (%s unpacked)%s",
                        paths.failed && !active ? failMark
                        : active                ? busyMark
                                                : doneMark,
                        paths.done,
                        paths.expected,
                        renderSizes(bytes.done, bytes.expected),
                        paths.failed ? fmt(", " ANSI_RED "%d failed" ANSI_NORMAL, paths.failed) : ""));
                lines.push_back(renderBar(paths.done, paths.failed, paths.running, paths.expected));
                renderItems(items, [&](const ActInfo & act) { return fmt("  ‣ %s%s", act.s, elapsed(act)); });
            }
        }

        /* Builds. */
        {
            auto builds = getActivityStats(state, actBuilds);
            if (builds.done || builds.expected) {
                auto items = getItems(actBuild);
                bool active = builds.running || builds.done < builds.expected;
                busy |= active;
                lines.push_back(
                    fmt("%s Built %d/%d derivations%s%s",
                        builds.failed && !active ? failMark
                        : active                 ? busyMark
                                                 : doneMark,
                        builds.done,
                        builds.expected,
                        builds.running ? fmt(", %d running", builds.running) : "",
                        builds.failed ? fmt(", " ANSI_RED "%d failed" ANSI_NORMAL, builds.failed) : ""));
                lines.push_back(renderBar(builds.done, builds.failed, builds.running, builds.expected));
                /* Note: build activities are also started for builds
                   that are waiting for a build slot, so show the
                   builds that have produced output first. */
                renderItems(
                    items,
                    [&](const ActInfo & act) {
                        auto s = fmt("  ‣ %s", act.s);
                        if (!act.phase.empty())
                            s += fmt(" (%s)", act.phase);
                        s += elapsed(act);
                        if (!act.lastLine.empty())
                            s += fmt(": " ANSI_FAINT "%s" ANSI_NORMAL, act.lastLine);
                        return s;
                    },
                    [](const ActInfo & act) { return !act.phase.empty() || !act.lastLine.empty(); });
            }
        }

        /* Path verification. */
        {
            auto verify = getActivityStats(state, actVerifyPaths);
            if (verify.done || verify.expected) {
                auto bad = std::min(verify.done, state.corruptedPaths + state.untrustedPaths + verify.failed);
                bool active = verify.done < verify.expected;
                busy |= active;
                lines.push_back(
                    fmt("%s Verified %d/%d paths%s%s%s",
                        active ? busyMark
                        : bad  ? failMark
                               : doneMark,
                        verify.done,
                        verify.expected,
                        state.corruptedPaths ? fmt(", " ANSI_RED "%d corrupted" ANSI_NORMAL, state.corruptedPaths) : "",
                        state.untrustedPaths ? fmt(", " ANSI_RED "%d untrusted" ANSI_NORMAL, state.untrustedPaths) : "",
                        verify.failed ? fmt(", " ANSI_RED "%d failed" ANSI_NORMAL, verify.failed) : ""));
                lines.push_back(renderBar(verify.done - bad, bad, verify.running, verify.expected));
            }
        }

        /* Store optimisation. */
        {
            auto optimise = getActivityStats(state, actOptimiseStore);
            if (optimise.done || optimise.expected) {
                bool active = optimise.done < optimise.expected;
                busy |= active;
                lines.push_back(
                    fmt("%s Optimised %d/%d paths, %s / %d inodes freed",
                        active ? busyMark : doneMark,
                        optimise.done,
                        optimise.expected,
                        renderSize(state.bytesLinked),
                        state.filesLinked));
                lines.push_back(renderBar(optimise.done, 0, 0, optimise.expected));
            }
        }

        /* Keep the spinner and durations moving. */
        if (busy)
            nextWakeup = std::min(nextWakeup, std::chrono::duration_cast<std::chrono::milliseconds>(spinnerInterval));

        return lines;
    }
};

} // namespace

std::unique_ptr<Logger> makeProgressBar()
{
    auto logger = std::make_unique<ProgressBar>(isTTY());
    logger->start();
    return logger;
}

std::unique_ptr<Logger> makeProgressIndicator()
{
    auto logger = std::make_unique<ProgressIndicator>(isTTY());
    logger->start();
    return logger;
}

} // namespace nix
