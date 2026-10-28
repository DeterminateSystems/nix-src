#include "nix/main/progress-bar.hh"
#include "nix/util/terminal.hh"
#include "nix/util/sync.hh"
#include "nix/util/signals.hh"
#include "nix/store/path.hh"
#include "nix/util/file-system.hh"
#include "nix/store/names.hh"
#include "nix/store/store-api.hh"
#include "nix/store/active-builds.hh"
#include "nix/util/util.hh"

#include <deque>
#include <map>
#include <thread>
#include <sstream>
#include <iostream>
#include <chrono>

#ifndef _WIN32
#  include <poll.h>
#  include <termios.h>
#  include <unistd.h>
#endif

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
 * The store that the progress indicator queries for active builds.
 * Note: this object intentionally leaks to avoid destructor ordering
 * issues with the progress indicator's threads.
 */
static auto * const progressBarStore = new Sync<std::weak_ptr<Store>>();

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
        /**
         * For `actBuild` activities, the derivation being built, and
         * the remote machine it's being built on (empty for local
         * builds).
         */
        std::optional<std::string> drvPath;
        std::string machineName;
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

        /**
         * Whether we're on the alternate screen (see
         * `enterAltScreen()`).
         */
        bool altScreen = false;

        struct DeferredOutput
        {
            bool toStdout;
            std::string data;
        };

        /**
         * Output written while on the alternate screen, to be written
         * to the normal screen when we leave the alternate screen.
         */
        std::vector<DeferredOutput> deferredOutput;

        /**
         * The most recent lines of log output written while on the
         * alternate screen.
         */
        std::deque<std::string> recentLog;
    };

    /**
     * The maximum number of lines kept in `Screen::recentLog`.
     */
    static constexpr size_t maxRecentLog = 500;

    Sync<Screen> screen_;

    Sync<State> state_;

    std::thread updateThread;

    std::condition_variable quitCV, updateCV;

    std::atomic<bool> printBuildLogs = false;
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

    /**
     * Hook for subclasses that read keyboard input: stop reading from
     * the terminal and restore its settings, because something else
     * (e.g. the REPL, a prompt, or the shell after Ctrl-Z) needs it.
     * Called with the state lock held.
     */
    virtual void suspendInput(State & state) {}

    /**
     * Undo `suspendInput()`. Called with the state lock held, if the
     * progress bar is active and not paused.
     */
    virtual void resumeInput(State & state) {}

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

        /* On Ctrl-Z, unhide the cursor and give the terminal back
           before the process is stopped. */
        stopCallback = createSignalCallback(SignalType::Stop, [&]() {
            auto state(state_.lock());
            if (state->active && !state->isPaused()) {
                suspendInput(*state);
                unhideCursorIfNeeded();
            }
        });

        /* Redraw the progress bar when the process is resumed or the
           terminal size changes. */
        auto redrawCallback = [&](bool resumed) {
            auto state(state_.lock());
            if (state->active && !state->isPaused()) {
                if (resumed) {
                    /* The shell has printed stuff since we were
                       stopped, so don't overwrite it. */
                    screen_.lock()->linesShown = 0;
                    resumeInput(*state);
                }
                /* Force a redraw, since the new output may be
                   identical to the one cached by redraw(). */
                invalidateRedrawCache();
                hideCursorIfNeeded();
                update(*state);
            }
        };
        contCallback = createSignalCallback(SignalType::Cont, [=]() { redrawCallback(true); });
        winchCallback = createSignalCallback(SignalType::Winch, [=]() { redrawCallback(false); });

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
                suspendInput(*state);
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
            suspendInput(*state);
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
            log(*state,
                lvlError,
                "nix::ProgressBar: resume() called without a matching preceding pause(). This is a bug.");
            return;
        } else {
            state->suspensions--;
        }
        if (state->suspensions == 0) {
            if (state->active) {
                clearProgressDisplay();
                hideCursorIfNeeded();
                resumeInput(*state);
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
            i->drvPath = getS(fields, 0);
            i->machineName = machineName;
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

        /* On the alternate screen, we own the entire screen, so we can
           draw from the top-left corner (`\e[H`). */
        auto s = (screen->altScreen ? "\e[H" : cursorToFrameStart(screen->linesShown)) + frame;
        /* If the new frame is shorter, erase from the cursor to the
           end of the screen to remove the remainder of the old
           frame. */
        if (screen->altScreen || lines.size() < screen->linesShown)
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
     * line, then print `s` there. On the alternate screen, `s` is
     * deferred until we leave the alternate screen instead.
     */
    void clearProgressDisplay(std::string_view s = "")
    {
        auto screen(screen_.lock());
        if (screen->altScreen) {
            defer(*screen, false, s);
            return;
        }
        clearProgressDisplay(*screen);
        writeToStderr(s);
    }

    static void clearProgressDisplay(Screen & screen)
    {
        writeToStderr(screen.linesShown > 1 ? cursorToFrameStart(screen.linesShown) + "\e[J" : std::string("\r\e[K"));
        screen.lastFrame.reset();
        screen.linesShown = 0;
    }

    static void defer(Screen & screen, bool toStdout, std::string_view s)
    {
        if (s.empty())
            return;
        if (!screen.deferredOutput.empty() && screen.deferredOutput.back().toStdout == toStdout)
            screen.deferredOutput.back().data += s;
        else
            screen.deferredOutput.push_back({.toStdout = toStdout, .data = std::string(s)});
        if (!toStdout) {
            for (auto & line : tokenizeString<std::vector<std::string>>(s, "\n"))
                screen.recentLog.push_back(std::move(line));
            while (screen.recentLog.size() > maxRecentLog)
                screen.recentLog.pop_front();
        }
    }

    /**
     * Switch to the terminal's alternate screen, which we can use in
     * its entirety without disturbing the normal screen. Output written
     * while on the alternate screen is deferred until
     * `leaveAltScreen()`.
     */
    void enterAltScreen()
    {
        auto screen(screen_.lock());
        if (screen->altScreen)
            return;
        /* Erase the frame on the normal screen first, so there is no
           stale frame when we come back. */
        clearProgressDisplay(*screen);
        writeToStderr("\e[?1049h");
        screen->altScreen = true;
    }

    /**
     * Switch back to the normal screen and write the output deferred
     * while on the alternate screen.
     */
    void leaveAltScreen()
    {
        auto screen(screen_.lock());
        if (!screen->altScreen)
            return;
        writeToStderr("\e[?1049l");
        screen->altScreen = false;
        screen->lastFrame.reset();
        screen->linesShown = 0;
        for (auto & output : screen->deferredOutput) {
            if (output.toStdout) {
                try {
                    writeFull(getStandardOutput(), output.data, false);
                } catch (SystemError &) {
                }
            } else
                writeToStderr(output.data);
        }
        screen->deferredOutput.clear();
        screen->recentLog.clear();
    }

    bool isOnAltScreen()
    {
        return screen_.lock()->altScreen;
    }

    /**
     * Return the last `n` lines of log output written while on the
     * alternate screen.
     */
    std::vector<std::string> getRecentLog(size_t n)
    {
        auto screen(screen_.lock());
        n = std::min(n, screen->recentLog.size());
        return {screen->recentLog.end() - n, screen->recentLog.end()};
    }

    std::chrono::milliseconds draw(State & state) noexcept
    {
        auto nextWakeup = std::chrono::milliseconds::max();

        state.haveUpdate = false;
        if (state.isPaused() || !state.active)
            return nextWakeup;

        auto lines = renderFrame(state, std::chrono::steady_clock::now(), nextWakeup);

        /* Don't draw more lines than fit in the terminal, since then
           we can't move the cursor back to the start of the frame. On
           the alternate screen, we draw from the top of the screen, so
           we can use every row. */
        auto rows = getWindowSize().first;
        size_t maxRows = isOnAltScreen() ? rows : rows - 1;
        if (rows > 1 && lines.size() > maxRows)
            lines.resize(maxRows);

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
            {
                /* If stdout is the terminal, defer the output while on
                   the alternate screen. */
                auto screen(screen_.lock());
                if (screen->altScreen) {
                    if (nix::isTTY(getStandardOutput()))
                        defer(*screen, true, std::string(s) + "\n");
                    else
                        Logger::writeToStdout(s);
                    return;
                }
            }
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
        suspendInput(*state);
        clearProgressDisplay(fmt("%s ", msg));
        unhideCursorIfNeeded();
        auto s = trim(readLine(getStandardInput(), true));
        hideCursorIfNeeded();
        if (!state->isPaused())
            resumeInput(*state);
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
     * The maximum height of the progress indicator, as a percentage of
     * the height of the terminal, but at least `minLines`.
     */
    static constexpr size_t maxHeightPercent = 30;
    static constexpr size_t minLines = 8;

    /**
     * Don't show activities in the list of other activities until
     * they've been running for this long, to avoid flicker.
     */
    static constexpr auto otherActivityDelay = std::chrono::milliseconds(300);

    /**
     * Interval between spinner updates.
     */
    static constexpr auto spinnerInterval = std::chrono::milliseconds(100);

    /**
     * Whether the help text is shown. Protected by the state lock.
     */
    bool helpShown = false;

    /**
     * If set, the display is paused, and these are the sections to
     * show instead of the current progress. Protected by the state
     * lock.
     */
    std::optional<std::vector<std::string>> frozenSections;

    /**
     * Whether full-screen mode is enabled, in which case we use the
     * entire alternate screen. Protected by the state lock.
     */
    bool fullScreen = false;

    /**
     * Whether we're currently reading keyboard input.
     */
    std::atomic<bool> inputEnabled = false;

#ifndef _WIN32
    struct Input
    {
        /**
         * Whether the input thread may read from stdin. If true, the
         * terminal has been put in non-canonical mode.
         */
        bool enabled = false;

        /**
         * The terminal settings to restore when input is disabled.
         */
        std::optional<struct termios> savedTermAttrs;

        /**
         * Set when stdin has reached EOF or the progress indicator is
         * being destroyed.
         */
        bool quit = false;
    };

    Sync<Input> input_;

    std::condition_variable inputCV;

    /**
     * Used to wake up the input thread from `poll()` when input is
     * disabled.
     */
    Pipe wakeupPipe;

    std::thread inputThread;
#endif

    /**
     * The minimum interval between queries for active builds.
     */
    static constexpr auto minActiveBuildsInterval = std::chrono::seconds(1);

    /**
     * Information about the running local builds, obtained from
     * `QueryActiveBuildsStore::queryActiveBuilds()` by the active
     * builds thread. Note: when both are needed, the state lock must
     * be acquired first.
     */
    struct ActiveBuilds
    {
        struct Build
        {
            ActiveBuildInfo info;

            /**
             * The CPU usage (in percent of one core) since the
             * previous sample, if any.
             */
            std::optional<double> cpuUsage;
        };

        /**
         * The active builds, keyed by derivation path.
         */
        std::map<std::string, Build> builds;

        bool quit = false;
    };

    Sync<ActiveBuilds> activeBuilds_;

    std::condition_variable activeBuildsCV;

    std::thread activeBuildsThread;

public:

    ProgressIndicator(bool isTTY)
        : ProgressBarBase(isTTY)
    {
    }

    ~ProgressIndicator()
    {
        stop();
#ifndef _WIN32
        if (inputThread.joinable()) {
            input_.lock()->quit = true;
            inputCV.notify_one();
            writeFull(wakeupPipe.writeSide.get(), "x", false);
            inputThread.join();
        }
#endif
        if (activeBuildsThread.joinable()) {
            activeBuilds_.lock()->quit = true;
            activeBuildsCV.notify_one();
            activeBuildsThread.join();
        }
    }

    /**
     * Start the thread that periodically queries the store for
     * information about running local builds (as shown by `nix ps`).
     */
    void startActiveBuildsThread()
    {
        if (!isTTY)
            return;

        activeBuildsThread = std::thread([this]() {
            try {
                activeBuildsThreadMain();
            } catch (...) {
                ignoreExceptionInDestructor();
            }
        });
    }

private:

    /**
     * Whether there are local builds for which we want to show
     * information.
     */
    bool wantActiveBuilds(State & state)
    {
        if (!state.active || state.isPaused() || frozenSections)
            return false;
        auto i = state.activitiesByType.find(actBuild);
        if (i == state.activitiesByType.end())
            return false;
        for (auto & [id, act] : i->second.its)
            if (act->drvPath && act->machineName.empty())
                return true;
        return false;
    }

    void activeBuildsThreadMain()
    {
        auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(minActiveBuildsInterval);

        /* The CPU times of the builds in the previous sample. */
        std::map<std::string, std::chrono::microseconds> prevCpuTimes;
        std::optional<std::chrono::steady_clock::time_point> prevSample;

        while (true) {
            {
                auto activeBuilds(activeBuilds_.lock());
                activeBuilds.wait_for(activeBuildsCV, interval, [&] { return activeBuilds->quit; });
                if (activeBuilds->quit)
                    return;
            }

            if (!wantActiveBuilds(*state_.lock())) {
                activeBuilds_.lock()->builds.clear();
                prevCpuTimes.clear();
                prevSample.reset();
                continue;
            }

            auto store = progressBarStore->lock()->lock();
            if (!store)
                continue;

            auto queryStore = dynamic_cast<QueryActiveBuildsStore *>(&*store);
            if (!queryStore)
                return;

            /* Note: this may call the logger (e.g. to show messages
               from the daemon), so we must not hold any locks. If the
               query fails (e.g. because the daemon is too old), give
               up. */
            auto before = std::chrono::steady_clock::now();
            std::vector<ActiveBuildInfo> infos;
            try {
                infos = queryStore->queryActiveBuilds();
            } catch (...) {
                return;
            }
            auto now = std::chrono::steady_clock::now();

            /* Don't spend more than ~10% of the time querying. */
            interval = std::max<std::chrono::steady_clock::duration>(minActiveBuildsInterval, (now - before) * 10);

            /* Compute the CPU usage since the previous sample. A build
               that wasn't in the previous sample started after it, so
               all of its CPU time was consumed in this interval. */
            auto elapsed = prevSample ? std::chrono::duration_cast<std::chrono::microseconds>(now - *prevSample)
                                      : std::chrono::microseconds(0);

            std::map<std::string, ActiveBuilds::Build> builds;
            decltype(prevCpuTimes) cpuTimes;

            for (auto & info : infos) {
                auto drvPath = store->printStorePath(info.derivation);
                auto cpuTime = info.getCpuTime();
                std::optional<double> cpuUsage;
                if (elapsed.count() > 0) {
                    auto prev = prevCpuTimes.find(drvPath);
                    auto prevTime = prev == prevCpuTimes.end() ? std::chrono::microseconds(0) : prev->second;
                    cpuUsage = std::max<double>((cpuTime - prevTime).count(), 0) * 100.0 / elapsed.count();
                }
                cpuTimes[drvPath] = cpuTime;
                builds.insert_or_assign(drvPath, ActiveBuilds::Build{.info = std::move(info), .cpuUsage = cpuUsage});
            }

            prevCpuTimes = std::move(cpuTimes);
            prevSample = now;

            activeBuilds_.lock()->builds = std::move(builds);

            update(*state_.lock());
        }
    }

public:

    /**
     * Start reading keyboard input, if stdin is a terminal.
     */
    void startInput()
    {
#ifndef _WIN32
        if (!isTTY || !isatty(STDIN_FILENO))
            return;

        wakeupPipe.create();

        inputThread = std::thread([this]() {
            try {
                inputThreadMain();
            } catch (...) {
                ignoreExceptionInDestructor();
            }
        });

        auto state(state_.lock());
        if (state->active && !state->isPaused())
            resumeInput(*state);
#endif
    }

private:

#ifndef _WIN32
    void suspendInput(State & state) override
    {
        leaveAltScreen();

        auto input(input_.lock());
        if (!input->enabled)
            return;
        input->enabled = false;
        inputEnabled = false;
        if (input->savedTermAttrs) {
            tcsetattr(STDIN_FILENO, TCSANOW, &*input->savedTermAttrs);
            input->savedTermAttrs.reset();
        }
        /* Interrupt the input thread's poll(). */
        writeFull(wakeupPipe.writeSide.get(), "x", false);
        update(state);
    }

    void resumeInput(State & state) override
    {
        if (!inputThread.joinable())
            return;

        auto input(input_.lock());
        if (input->enabled || input->quit)
            return;

        /* Don't touch the terminal if we're in the background, since
           that would cause SIGTTOU/SIGTTIN. */
        if (tcgetpgrp(STDIN_FILENO) != getpgrp())
            return;

        /* Disable line buffering and echoing so we get keys
           immediately. Note that we keep ISIG enabled, so Ctrl-C and
           Ctrl-Z still send signals, and OPOST, so we don't need to
           translate "\n" to "\r\n". */
        struct termios term;
        if (tcgetattr(STDIN_FILENO, &term))
            return;
        input->savedTermAttrs = term;
        term.c_lflag &= ~(ICANON | ECHO);
        term.c_cc[VMIN] = 1;
        term.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &term)) {
            input->savedTermAttrs.reset();
            return;
        }

        input->enabled = true;
        inputEnabled = true;
        inputCV.notify_one();

        if (fullScreen)
            enterAltScreen();

        update(state);
    }

    void inputThreadMain()
    {
        while (true) {
            {
                auto input(input_.lock());
                while (!input->quit && !input->enabled)
                    input.wait(inputCV);
                if (input->quit)
                    return;
            }

            std::array<struct pollfd, 2> fds{{
                {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0},
                {.fd = wakeupPipe.readSide.get(), .events = POLLIN, .revents = 0},
            }};

            if (poll(fds.data(), fds.size(), -1) == -1) {
                if (errno == EINTR)
                    continue;
                throw SysError("polling for keyboard input");
            }

            if (fds[1].revents & POLLIN) {
                char buf[64];
                [[maybe_unused]] auto n = ::read(wakeupPipe.readSide.get(), buf, sizeof(buf));
                continue;
            }

            if (!fds[0].revents)
                continue;

            char c;
            {
                /* Read while holding the lock, so that we don't
                   consume input after `suspendInput()` has
                   returned. */
                auto input(input_.lock());
                if (!input->enabled)
                    continue;
                auto n = ::read(STDIN_FILENO, &c, 1);
                if (n == -1 && errno == EINTR)
                    continue;
                if (n != 1) {
                    /* EOF or error, so stop reading. */
                    input->quit = true;
                    return;
                }
            }

            handleKey(c);
        }
    }

    void handleKey(char c)
    {
        c = std::tolower((unsigned char) c);

        if (c == 'q') {
            /* Note: this calls the interrupt callback, which calls
               pause(), so we must not hold the state lock. */
            unix::triggerInterrupt();
            return;
        }

        auto state(state_.lock());

        switch (c) {

        case 'l':
            printBuildLogs = !printBuildLogs;
            log(*state,
                lvlError,
                printBuildLogs ? ANSI_BOLD "Enabling build logs." : ANSI_BOLD "Disabling build logs.");
            break;

        case 'v':
        case '+':
        case '=':
            if (verbosity < lvlVomit) {
                verbosity = (Verbosity) (verbosity + 1);
                log(*state, lvlError, ANSI_BOLD "Increasing verbosity.");
            }
            break;

        case '-':
            if (verbosity > lvlError) {
                verbosity = (Verbosity) (verbosity - 1);
                log(*state, lvlError, ANSI_BOLD "Decreasing verbosity.");
            }
            break;

        case 'h':
        case '?':
            helpShown = !helpShown;
            update(*state);
            break;

        case 'p':
        case ' ':
            if (frozenSections)
                frozenSections.reset();
            else {
                auto nextWakeup = std::chrono::milliseconds::max();
                frozenSections = renderSections(
                    *state, std::chrono::steady_clock::now(), nextWakeup, getMaxLines(renderTrailer(true).size()));
            }
            update(*state);
            break;

        case 'f':
            fullScreen = !fullScreen;
            if (fullScreen)
                enterAltScreen();
            else
                leaveAltScreen();
            update(*state);
            break;
        }
    }
#endif

    /**
     * Render the statistics of a running build: the number of
     * processes, CPU usage and memory usage.
     */
    static std::vector<std::string> renderActiveBuildStats(const ActiveBuilds::Build & build)
    {
        auto & info = build.info;
        std::vector<std::string> res;
        auto n = info.processes.size();
        res.push_back(fmt("%d %s", n, n == 1 ? "process" : "processes"));
        if (build.cpuUsage)
            res.push_back(fmt("%.0f%% CPU", *build.cpuUsage));
        if (auto rss = info.getRss())
            res.push_back(renderSize(*rss));
        return res;
    }

    /**
     * Return the program name of the most recently started process of
     * a running build, other than the builder's main process (if there
     * are others).
     */
    static std::optional<std::string> getLatestProgram(const ActiveBuilds::Build & build)
    {
        auto & info = build.info;
        const ActiveBuildInfo::ProcessInfo * latest = nullptr;
        for (auto & process : info.processes)
            if (!latest || (latest->pid == info.mainPid) || (process.pid != info.mainPid && process.pid > latest->pid))
                latest = &process;
        if (!latest || latest->argv.empty())
            return std::nullopt;
        auto & argv = latest->argv;
        /* Note: some processes overwrite their argv with a free-form
           title (like "sshd: user"), so only use the first word. */
        auto words = tokenizeString<std::vector<std::string>>(argv[0]);
        if (words.empty())
            return std::nullopt;
        auto program = std::string(baseNameOf(words[0]));
        /* For scripts (e.g. the gcc wrapper), show the name of the
           script rather than of the interpreter. */
        static const std::set<std::string, std::less<>> interpreters = {"sh", "bash", "dash", "perl", "python3"};
        if (argv.size() >= 2 && interpreters.contains(program) && !argv[1].empty() && argv[1][0] != '-')
            program = baseNameOf(argv[1]);
        return program;
    }

    /**
     * Return the maximum number of lines that the sections (and in
     * full-screen mode, the log) may occupy, given that the trailer
     * (see `renderTrailer()`) has `trailerLines` lines.
     */
    size_t getMaxLines(size_t trailerLines)
    {
        size_t rows = getWindowSize().first;
        if (rows == 0)
            rows = 24;
        /* In full-screen mode, everything must fit on the screen. */
        if (isOnAltScreen())
            return rows > trailerLines ? rows - trailerLines : 0;
        auto maxLines = std::max(minLines, rows * maxHeightPercent / 100);
        /* Leave room for the "Type 'h' for help" line. The help text
           is shown in addition to the sections. */
        if (inputEnabled && !helpShown)
            maxLines--;
        return maxLines;
    }

    /**
     * Render the lines shown below the sections: the pause message,
     * and the help text or a hint on how to show it.
     */
    std::vector<std::string> renderTrailer(bool paused)
    {
        std::vector<std::string> lines;

        if (paused)
            lines.push_back(ANSI_BOLD "Display paused. Press 'p' or space to resume.");

        if (inputEnabled) {
            if (helpShown) {
                lines.push_back(ANSI_BOLD "The following keys are available:");
                lines.push_back(
                    fmt("  " ANSI_BOLD "l" ANSI_NORMAL "  %s build logs", printBuildLogs ? "hide" : "show"));
                lines.push_back("  " ANSI_BOLD "v" ANSI_NORMAL "  increase verbosity");
                lines.push_back("  " ANSI_BOLD "-" ANSI_NORMAL "  decrease verbosity");
                lines.push_back(fmt("  " ANSI_BOLD "p" ANSI_NORMAL "  %s the display", paused ? "resume" : "pause"));
                lines.push_back(
                    fmt("  " ANSI_BOLD "f" ANSI_NORMAL "  %s full-screen mode", fullScreen ? "leave" : "enter"));
                lines.push_back("  " ANSI_BOLD "h" ANSI_NORMAL "  hide this help");
                lines.push_back("  " ANSI_BOLD "q" ANSI_NORMAL "  quit");
            } else if (!paused)
                lines.push_back(ANSI_FAINT "Type 'h' for help.");
        }

        return lines;
    }

    std::vector<std::string> renderFrame(
        State & state, std::chrono::steady_clock::time_point now, std::chrono::milliseconds & nextWakeup) override
    {
        auto paused = frozenSections.has_value();
        auto trailer = renderTrailer(paused);
        auto maxLines = getMaxLines(trailer.size());

        auto lines = paused ? *frozenSections : renderSections(state, now, nextWakeup, maxLines);

        auto altScreen = isOnAltScreen();

        /* In full-screen mode, show the most recent log output in the
           remaining space, since it's not visible otherwise. */
        if (altScreen && lines.size() + 2 <= maxLines) {
            auto log = getRecentLog(maxLines - lines.size() - 1);
            if (!log.empty()) {
                lines.push_back(ANSI_FAINT "── log ──");
                std::ranges::move(log, std::back_inserter(lines));
            }
        }

        /* Don't show the help hint if there is nothing else to show. */
        if (!lines.empty() || paused || helpShown) {
            /* In full-screen mode, show the trailer at the bottom of
               the screen. */
            if (altScreen && lines.size() < maxLines)
                lines.resize(maxLines);
            std::ranges::move(trailer, std::back_inserter(lines));
        }

        return lines;
    }

    /**
     * A section of the progress indicator, showing the progress of
     * some kind of work.
     */
    struct Section
    {
        /**
         * Lines that are always shown (e.g. a title and progress bar).
         */
        std::vector<std::string> header;

        /**
         * The running items (e.g. builds), in display order. Each item
         * consists of one or more lines.
         */
        std::vector<std::vector<std::string>> items;
    };

    /**
     * Combine the sections into at most `maxLines` lines. Section
     * headers are always shown, even if they exceed `maxLines`. The
     * remaining lines are distributed round-robin among the sections'
     * items, i.e. every section gets to show its first item before any
     * section shows its second item. Hidden items are summarized by a
     * "… and N more" line.
     */
    static std::vector<std::string> layoutSections(const std::vector<Section> & sections, size_t maxLines)
    {
        /* The number of items shown for each section. */
        std::vector<size_t> shown(sections.size(), 0);

        /* Initially, every section with items needs a line saying how
           many items are hidden. */
        size_t used = 0;
        for (auto & section : sections)
            used += section.header.size() + (section.items.empty() ? 0 : 1);

        bool progress = true;
        while (progress) {
            progress = false;
            for (auto [n, section] : enumerate(sections)) {
                if (shown[n] == section.items.size())
                    continue;
                /* If this is the last item, we don't need the "… and N
                   more" line anymore. */
                auto height = section.items[shown[n]].size();
                auto delta = height - (shown[n] + 1 == section.items.size() ? 1 : 0);
                if (used + delta > maxLines)
                    continue;
                used += delta;
                shown[n]++;
                progress = true;
            }
        }

        std::vector<std::string> lines;
        for (auto [n, section] : enumerate(sections)) {
            std::ranges::copy(section.header, std::back_inserter(lines));
            for (size_t i = 0; i < shown[n]; ++i)
                std::ranges::copy(section.items[i], std::back_inserter(lines));
            auto hidden = section.items.size() - shown[n];
            if (hidden)
                lines.push_back(
                    shown[n] ? fmt(ANSI_FAINT "   … and %d more" ANSI_NORMAL, hidden)
                             : fmt(ANSI_FAINT "   … %d more" ANSI_NORMAL, hidden));
        }
        return lines;
    }

    /**
     * Render the sections showing the progress of each kind of work,
     * using at most `maxLines` lines (see `layoutSections()`).
     */
    std::vector<std::string> renderSections(
        State & state,
        std::chrono::steady_clock::time_point now,
        std::chrono::milliseconds & nextWakeup,
        size_t maxLines)
    {
        std::vector<Section> sections;

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
        size_t barWidth = width < 15 ? 10 : std::min(60u, width - 5);

        /* Render a progress bar showing the fraction of failed (red),
           done (green) and running (yellow) items. */
        auto renderBar = [&](uint64_t done, uint64_t failed, uint64_t running, uint64_t expected) {
            expected = std::max(expected, (uint64_t) 1);
            auto chars = [&](uint64_t n) { return (size_t) (barWidth * std::min((double) n / expected, 1.0)); };
            auto chars1 = chars(failed);
            auto chars2 = chars(failed + done);
            auto chars3 = chars(failed + done + running);
            return "   " ANSI_RED + repeat("█", chars1) + ANSI_GREEN + repeat("█", chars2 - chars1) + ANSI_WARNING
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

        /* Render the given activities, oldest first. `renderItem`
           returns the lines for an activity. If `isActive` is given,
           show the activities for which it returns true first. */
        auto renderItems = [&](std::vector<ActInfo *> items,
                               auto renderItem,
                               std::function<bool(const ActInfo &)> isActive = nullptr) {
            std::ranges::sort(items, {}, &ActInfo::startTime);
            if (isActive)
                std::ranges::stable_partition(items, [&](ActInfo * act) { return isActive(*act); });
            std::vector<std::vector<std::string>> res;
            for (auto item : items)
                res.push_back(renderItem(*item));
            return res;
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
            sections.push_back({.items = renderItems(items, [&](const ActInfo & act) -> std::vector<std::string> {
                                    return {fmt("%s %s%s", busyMark, act.s, elapsed(act))};
                                })});
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
                sections.push_back({
                    .header =
                        {fmt("%s Downloaded %s", active ? busyMark : doneMark, renderSizes(stats.done, stats.expected)),
                         renderBar(stats.done, 0, 0, stats.expected)},
                    .items = renderItems(
                        items,
                        [&](const ActInfo & act) -> std::vector<std::string> {
                            return {
                                fmt("   ‣ %s%s%s",
                                    act.s,
                                    act.expected ? " " + renderSizes(act.done, act.expected) : "",
                                    elapsed(act))};
                        }),
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
                sections.push_back({
                    .header =
                        {fmt("%s Fetched %d/%d paths (%s unpacked)%s",
                             paths.failed && !active ? failMark
                             : active                ? busyMark
                                                     : doneMark,
                             paths.done,
                             paths.expected,
                             renderSizes(bytes.done, bytes.expected),
                             paths.failed ? fmt(", " ANSI_RED "%d failed" ANSI_NORMAL, paths.failed) : ""),
                         renderBar(paths.done, paths.failed, paths.running, paths.expected)},
                    .items = renderItems(
                        items,
                        [&](const ActInfo & act) -> std::vector<std::string> {
                            return {fmt("   ‣ %s%s", act.s, elapsed(act))};
                        }),
                });
            }
        }

        /* Builds. */
        {
            auto builds = getActivityStats(state, actBuilds);
            if (builds.done || builds.expected) {
                auto items = getItems(actBuild);
                bool active = builds.running || builds.done < builds.expected;
                busy |= active;
                Section section;
                section.header.push_back(
                    fmt("%s Built %d/%d derivations%s%s",
                        builds.failed && !active ? failMark
                        : active                 ? busyMark
                                                 : doneMark,
                        builds.done,
                        builds.expected,
                        builds.running ? fmt(", %d running", builds.running) : "",
                        builds.failed ? fmt(", " ANSI_RED "%d failed" ANSI_NORMAL, builds.failed) : ""));
                section.header.push_back(renderBar(builds.done, builds.failed, builds.running, builds.expected));

                auto activeBuilds(activeBuilds_.lock());

                auto getActiveBuild = [&](const ActInfo & act) -> const ActiveBuilds::Build * {
                    if (!act.drvPath || !act.machineName.empty())
                        return nullptr;
                    auto i = activeBuilds->builds.find(*act.drvPath);
                    return i == activeBuilds->builds.end() ? nullptr : &i->second;
                };

                /* Note: build activities are also started for builds
                   that are waiting for a build slot, so show the
                   builds that are actually running first. */
                section.items = renderItems(
                    items,
                    [&](const ActInfo & act) {
                        std::vector<std::string> lines;
                        auto activeBuild = getActiveBuild(act);
                        auto s = fmt("   ‣ %s", act.s);
                        if (!act.phase.empty())
                            s += fmt(" (%s)", act.phase);
                        if (activeBuild) {
                            /* Show the elapsed time and statistics.
                               Use the time at which the build actually
                               started, i.e. not including waiting for a
                               build slot. */
                            std::vector<std::string> stats;
                            auto d = std::chrono::system_clock::now()
                                     - std::chrono::system_clock::from_time_t(activeBuild->info.startTime);
                            if (d >= std::chrono::seconds(1))
                                stats.push_back(renderDuration(d));
                            std::ranges::move(renderActiveBuildStats(*activeBuild), std::back_inserter(stats));
                            s += " " ANSI_FAINT + concatStringsSep(", ", stats);
                            if (auto program = getLatestProgram(*activeBuild))
                                s += fmt(" (%s)", *program);
                            s += ANSI_NORMAL;
                        } else
                            s += elapsed(act);
                        lines.push_back(std::move(s));
                        /* Show the last log line below the build. Note:
                           if build logs are enabled, `lastLine` is not
                           updated. */
                        if (!act.lastLine.empty() && !printBuildLogs)
                            lines.push_back(fmt("     " ANSI_FAINT "> %s" ANSI_NORMAL, act.lastLine));
                        return lines;
                    },
                    [&](const ActInfo & act) {
                        return getActiveBuild(act) || !act.phase.empty() || !act.lastLine.empty();
                    });
                sections.push_back(std::move(section));
            }
        }

        /* Path verification. */
        {
            auto verify = getActivityStats(state, actVerifyPaths);
            if (verify.done || verify.expected) {
                auto bad = std::min(verify.done, state.corruptedPaths + state.untrustedPaths + verify.failed);
                bool active = verify.done < verify.expected;
                busy |= active;
                Section section;
                section.header.push_back(
                    fmt("%s Verified %d/%d paths%s%s%s",
                        active ? busyMark
                        : bad  ? failMark
                               : doneMark,
                        verify.done,
                        verify.expected,
                        state.corruptedPaths ? fmt(", " ANSI_RED "%d corrupted" ANSI_NORMAL, state.corruptedPaths) : "",
                        state.untrustedPaths ? fmt(", " ANSI_RED "%d untrusted" ANSI_NORMAL, state.untrustedPaths) : "",
                        verify.failed ? fmt(", " ANSI_RED "%d failed" ANSI_NORMAL, verify.failed) : ""));
                section.header.push_back(renderBar(verify.done - bad, bad, verify.running, verify.expected));
                sections.push_back(std::move(section));
            }
        }

        /* Store optimisation. */
        {
            auto optimise = getActivityStats(state, actOptimiseStore);
            if (optimise.done || optimise.expected) {
                bool active = optimise.done < optimise.expected;
                busy |= active;
                sections.push_back({
                    .header =
                        {fmt("%s Optimised %d/%d paths, %s / %d inodes freed",
                             active ? busyMark : doneMark,
                             optimise.done,
                             optimise.expected,
                             renderSize(state.bytesLinked),
                             state.filesLinked),
                         renderBar(optimise.done, 0, 0, optimise.expected)},
                });
            }
        }

        /* Keep the spinner and durations moving. */
        if (busy)
            nextWakeup = std::min(nextWakeup, std::chrono::duration_cast<std::chrono::milliseconds>(spinnerInterval));

        return layoutSections(sections, maxLines);
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
    logger->startInput();
    logger->startActiveBuildsThread();
    return logger;
}

void setProgressBarStore(ref<Store> store)
{
    *progressBarStore->lock() = store.get_ptr();
}

} // namespace nix
