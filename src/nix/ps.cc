#include "nix/cmd/command.hh"
#include "nix/main/common-args.hh"
#include "nix/main/shared.hh"
#include "nix/store/store-api.hh"
#include "nix/store/store-cast.hh"
#include "nix/store/active-builds.hh"
#include "nix/util/table.hh"
#include "nix/util/terminal.hh"
#include "nix/util/file-system.hh"
#include "nix/util/finally.hh"
#include "nix/util/signals.hh"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <sstream>

#ifndef _WIN32
#  include <poll.h>
#  include <termios.h>
#  include <unistd.h>
#endif

using namespace nix;

/* Helper to format user info: show name if available, else UID. */
static std::string formatUser(const UserInfo & user)
{
    return user.name ? *user.name : std::to_string(user.uid);
}

static std::string formatCpuTime(std::chrono::microseconds cpuTime)
{
    return fmt(
        "%.1fs",
        std::chrono::duration_cast<std::chrono::duration<float, std::chrono::seconds::period>>(cpuTime).count());
}

static TableCell formatRss(std::optional<uint64_t> rss)
{
    return {rss ? renderSize(*rss) : "", TableCell::Alignment::Right};
}

/**
 * Call `visit` on the processes of `build` in tree order. `prefix` is the tree drawing to put in front of the
 * process.
 */
static void visitProcessTree(
    const ActiveBuildInfo & build,
    std::function<void(const ActiveBuildInfo::ProcessInfo & process, std::string_view prefix)> visit)
{
    /* Recover the tree structure of the processes. */
    std::set<pid_t> pids;
    for (auto & process : build.processes)
        pids.insert(process.pid);

    using Processes = std::set<const ActiveBuildInfo::ProcessInfo *>;
    std::map<pid_t, Processes> children;
    Processes rootProcesses;
    for (auto & process : build.processes) {
        if (pids.contains(process.parentPid))
            children[process.parentPid].insert(&process);
        else
            rootProcesses.insert(&process);
    }

    [&](this auto const & recurse, const Processes & processes, std::string_view prefix) -> void {
        for (const auto & [n, process] : enumerate(processes)) {
            bool last = n + 1 == processes.size();
            visit(*process, fmt("%s%s", prefix, last ? treeLast : treeConn));
            recurse(children[process->pid], last ? std::string(prefix) + treeNull : std::string(prefix) + treeLine);
        }
    }(rootProcesses, "");
}

struct CmdPs : MixJSON, StoreCommand
{
    std::string description() override
    {
        return "list active builds";
    }

    Category category() override
    {
        return catUtility;
    }

    std::string doc() override
    {
        return
#include "ps.md"
            ;
    }

    void run(ref<Store> store) override
    {
        auto & tracker = require<QueryActiveBuildsStore>(*store);

        auto builds = tracker.queryActiveBuilds();

        if (json) {
            printJSON(nlohmann::json(builds));
            return;
        }

        if (builds.empty()) {
            notice("No active builds.");
            return;
        }

        Table table;

        /* Add column headers. */
        table.push_back(
            {{"USER"},
             {"PID"},
             {"CPU", TableCell::Alignment::Right},
             {"RSS", TableCell::Alignment::Right},
             {"DERIVATION/COMMAND"}});

        for (const auto & build : builds) {
            /* Add build summary row. */
            table.push_back(
                {formatUser(build.mainUser),
                 std::to_string(build.mainPid),
                 {formatCpuTime(build.getCpuTime()), TableCell::Alignment::Right},
                 formatRss(build.getRss()),
                 fmt(ANSI_BOLD "%s" ANSI_NORMAL " (wall=%ds)",
                     store->printStorePath(build.derivation),
                     time(nullptr) - build.startTime)});

            if (build.processes.empty()) {
                table.push_back(
                    {formatUser(build.mainUser),
                     std::to_string(build.mainPid),
                     {"", TableCell::Alignment::Right},
                     {"", TableCell::Alignment::Right},
                     fmt("%s" ANSI_ITALIC "(no process info)" ANSI_NORMAL, treeLast)});
            } else {
                /* Render the process tree. */
                visitProcessTree(build, [&](const ActiveBuildInfo::ProcessInfo & process, std::string_view prefix) {
                    auto cpuTime = process.getCpuTime();
                    table.push_back(
                        {formatUser(process.user),
                         std::to_string(process.pid),
                         {cpuTime ? formatCpuTime(*cpuTime) : "", TableCell::Alignment::Right},
                         formatRss(process.rss),
                         fmt("%s%s", prefix, process.formatArgv())});
                });
            }
        }

        auto width = isTTY() && isatty(STDOUT_FILENO) ? getWindowWidth() : std::numeric_limits<unsigned int>::max();

        printTable(std::cout, table, width);
    }
};

static auto rCmdPs = registerCommand2<CmdPs>({"ps"});

#ifndef _WIN32

static std::string formatWallTime(time_t secs)
{
    secs = std::max<time_t>(secs, 0);
    if (secs < 60)
        return fmt("%ds", secs);
    if (secs < 3600)
        return fmt("%dm%02ds", secs / 60, secs % 60);
    return fmt("%dh%02dm", secs / 3600, (secs / 60) % 60);
}

struct CmdTop : StoreCommand
{
    std::string description() override
    {
        return "interactively show active builds";
    }

    Category category() override
    {
        return catUtility;
    }

    std::string doc() override
    {
        return
#  include "top.md"
            ;
    }

    using Clock = std::chrono::steady_clock;

    enum class SortOrder { Cpu, Rss, Wall, Name };

    static constexpr std::array sortNames = {"%CPU", "RSS", "wall time", "name"};

    /* Builds are identified by the Nix process that runs them, their main process and their derivation. */
    using BuildKey = std::tuple<pid_t, pid_t, StorePath>;

    static BuildKey getBuildKey(const ActiveBuildInfo & build)
    {
        return {build.nixPid, build.mainPid, build.derivation};
    }

    /**
     * The CPU time of a process itself, excluding reaped children (so that a shell doesn't show a CPU spike
     * when a child exits).
     */
    static std::optional<std::chrono::microseconds> getOwnCpuTime(const ActiveBuildInfo::ProcessInfo & process)
    {
        if (!process.utime && !process.stime)
            return std::nullopt;
        return process.utime.value_or(std::chrono::microseconds(0))
               + process.stime.value_or(std::chrono::microseconds(0));
    }

    void run(ref<Store> store) override
    {
        auto & tracker = require<QueryActiveBuildsStore>(*store);

        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
            throw UsageError("'nix top' requires a terminal");

        /* Put the terminal in non-canonical mode without echo. Signal generation is also disabled so that Ctrl-Z
           cannot leave the shell in the alternate screen; Ctrl-C is handled as a key instead. */
        struct termios savedTermios;
        if (tcgetattr(STDIN_FILENO, &savedTermios))
            throw SysError("getting terminal attributes");

        logger->pause();
        Finally restoreLogger([&] { logger->resume(); });

        auto rawTermios = savedTermios;
        rawTermios.c_lflag &= ~(ICANON | ECHO | ISIG);
        rawTermios.c_cc[VMIN] = 0;
        rawTermios.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &rawTermios))
            throw SysError("setting terminal attributes");

        Finally restoreTerminal([&] {
            try {
                /* Show the cursor and leave the alternate screen. */
                writeFull(STDOUT_FILENO, "\e[?25h\e[?1049l", false);
            } catch (...) {
                ignoreExceptionInDestructor();
            }
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &savedTermios);
        });

        /* Enter the alternate screen and hide the cursor. */
        writeFull(STDOUT_FILENO, "\e[?1049h\e[?25l");

        std::chrono::milliseconds refreshInterval{2000};
        SortOrder sortOrder = SortOrder::Cpu;
        bool showTree = true;
        bool compact = true;
        size_t scroll = 0;
        bool clearScreen = true;

        /* The most recent sample. */
        std::vector<ActiveBuildInfo> builds;
        std::optional<std::string> queryError;
        std::optional<Clock::time_point> lastSample;

        /* CPU times from the most recent sample, and CPU usage computed from the last two samples. */
        std::map<BuildKey, std::chrono::microseconds> buildCpuTimes;
        std::map<pid_t, std::chrono::microseconds> processCpuTimes;
        std::map<BuildKey, double> buildCpuUsage;
        std::map<pid_t, double> processCpuUsage;

        auto takeSample = [&]() {
            try {
                builds = tracker.queryActiveBuilds();
                queryError.reset();
            } catch (Interrupted &) {
                throw;
            } catch (Error & e) {
                queryError = e.info().msg.str();
                return;
            }

            auto now = Clock::now();
            auto elapsed =
                lastSample ? std::chrono::duration_cast<std::chrono::microseconds>(now - *lastSample).count() : 0;

            /* A build or process that wasn't in the previous sample started after it, so all of its CPU time was
               consumed in this interval. */
            auto usage =
                [&](auto & prevTimes, const auto & key, std::chrono::microseconds cpuTime) -> std::optional<double> {
                if (elapsed <= 0)
                    return std::nullopt;
                auto prev = prevTimes.find(key);
                auto prevTime = prev == prevTimes.end() ? std::chrono::microseconds(0) : prev->second;
                return std::max<double>((cpuTime - prevTime).count(), 0) * 100.0 / elapsed;
            };

            decltype(buildCpuTimes) newBuildCpuTimes;
            decltype(processCpuTimes) newProcessCpuTimes;
            buildCpuUsage.clear();
            processCpuUsage.clear();

            for (auto & build : builds) {
                auto key = getBuildKey(build);
                auto cpuTime = build.getCpuTime();
                if (auto u = usage(buildCpuTimes, key, cpuTime))
                    buildCpuUsage[key] = *u;
                newBuildCpuTimes[key] = cpuTime;

                for (auto & process : build.processes) {
                    if (auto cpuTime = getOwnCpuTime(process)) {
                        if (auto u = usage(processCpuTimes, process.pid, *cpuTime))
                            processCpuUsage[process.pid] = *u;
                        newProcessCpuTimes[process.pid] = *cpuTime;
                    }
                }
            }

            buildCpuTimes = std::move(newBuildCpuTimes);
            processCpuTimes = std::move(newProcessCpuTimes);
            lastSample = now;
        };

        auto formatUsage = [](const auto & map, const auto & key) -> std::string {
            auto i = map.find(key);
            return i == map.end() ? "" : fmt("%.1f", i->second);
        };

        auto render = [&]() {
            auto [rows, cols] = getWindowSize();
            if (rows == 0 || cols == 0) {
                rows = 24;
                cols = 80;
            }

            /* Sort the builds. */
            std::vector<const ActiveBuildInfo *> sorted;
            for (auto & build : builds)
                sorted.push_back(&build);
            std::ranges::stable_sort(sorted, [&](const ActiveBuildInfo * a, const ActiveBuildInfo * b) {
                switch (sortOrder) {
                case SortOrder::Cpu: {
                    auto ua = buildCpuUsage.find(getBuildKey(*a));
                    auto ub = buildCpuUsage.find(getBuildKey(*b));
                    return (ua == buildCpuUsage.end() ? -1.0 : ua->second)
                           > (ub == buildCpuUsage.end() ? -1.0 : ub->second);
                }
                case SortOrder::Rss:
                    return a->getRss().value_or(0) > b->getRss().value_or(0);
                case SortOrder::Wall:
                    return a->startTime < b->startTime;
                case SortOrder::Name:
                    return std::pair(a->derivation.name(), a->derivation.to_string())
                           < std::pair(b->derivation.name(), b->derivation.to_string());
                }
                unreachable();
            });

            /* Render the table. */
            Table table;
            table.push_back(
                {{"USER"},
                 {"PID", TableCell::Alignment::Right},
                 {"%CPU", TableCell::Alignment::Right},
                 {"TIME", TableCell::Alignment::Right},
                 {"RSS", TableCell::Alignment::Right},
                 {"WALL", TableCell::Alignment::Right},
                 {"DERIVATION/COMMAND"}});

            auto now = time(nullptr);
            size_t nrProcesses = 0;
            double totalUsage = 0;
            uint64_t totalRss = 0;

            for (auto build : sorted) {
                auto key = getBuildKey(*build);
                nrProcesses += build->processes.size();
                if (auto i = buildCpuUsage.find(key); i != buildCpuUsage.end())
                    totalUsage += i->second;
                totalRss += build->getRss().value_or(0);

                table.push_back(
                    {formatUser(build->mainUser),
                     {std::to_string(build->mainPid), TableCell::Alignment::Right},
                     {formatUsage(buildCpuUsage, key), TableCell::Alignment::Right},
                     {formatCpuTime(build->getCpuTime()), TableCell::Alignment::Right},
                     formatRss(build->getRss()),
                     {formatWallTime(now - build->startTime), TableCell::Alignment::Right},
                     fmt(ANSI_BOLD "%s" ANSI_NORMAL, store->printStorePath(build->derivation))});

                if (!showTree)
                    continue;

                if (build->processes.empty())
                    table.push_back(
                        {formatUser(build->mainUser),
                         {"", TableCell::Alignment::Right},
                         {"", TableCell::Alignment::Right},
                         {"", TableCell::Alignment::Right},
                         {"", TableCell::Alignment::Right},
                         {"", TableCell::Alignment::Right},
                         fmt("%s" ANSI_ITALIC "(no process info)" ANSI_NORMAL, treeLast)});
                else
                    visitProcessTree(
                        *build, [&](const ActiveBuildInfo::ProcessInfo & process, std::string_view prefix) {
                            auto cpuTime = process.getCpuTime();
                            table.push_back(
                                {formatUser(process.user),
                                 {std::to_string(process.pid), TableCell::Alignment::Right},
                                 {formatUsage(processCpuUsage, process.pid), TableCell::Alignment::Right},
                                 {cpuTime ? formatCpuTime(*cpuTime) : "", TableCell::Alignment::Right},
                                 formatRss(process.rss),
                                 {"", TableCell::Alignment::Right},
                                 fmt("%s%s",
                                     prefix,
                                     compact ? abbreviateStorePaths(process.formatArgv(true), store->storeDir)
                                             : process.formatArgv())});
                        });
            }

            std::stringstream tableStream;
            printTable(tableStream, table);

            std::vector<std::string> tableLines;
            for (std::string line; std::getline(tableStream, line);)
                tableLines.push_back(std::move(line));
            assert(!tableLines.empty());

            /* Compute the visible part of the table body. */
            size_t bodyHeight = rows > 3 ? rows - 3 : 1;
            size_t bodySize = tableLines.size() - 1;
            scroll = std::min(scroll, bodySize > bodyHeight ? bodySize - bodyHeight : 0);

            std::vector<std::string> lines;

            /* Status line. */
            char timeStr[16];
            struct tm tm;
            strftime(timeStr, sizeof(timeStr), "%H:%M:%S", localtime_r(&now, &tm));
            auto status =
                fmt(ANSI_BOLD "nix top" ANSI_NORMAL " - %s  %d builds, %d processes, %.1f%% CPU, %s RSS",
                    timeStr,
                    builds.size(),
                    nrProcesses,
                    totalUsage,
                    renderSize(totalRss));
            if (queryError)
                status += fmt("  " ANSI_RED "%s" ANSI_NORMAL, *queryError);
            lines.push_back(status);

            /* Column headers in reverse video, spanning the full width. */
            auto header = filterANSIEscapes(tableLines[0], true, cols);
            header.resize(std::max<size_t>(header.size(), cols), ' ');
            lines.push_back("\e[7m" + header + ANSI_NORMAL);

            /* Table body. */
            if (builds.empty())
                lines.push_back(ANSI_FAINT "No active builds." ANSI_NORMAL);
            else
                for (size_t i = 0; i < bodyHeight && scroll + 1 + i < tableLines.size(); ++i)
                    lines.push_back(tableLines[scroll + 1 + i]);
            while (lines.size() < bodyHeight + 2)
                lines.push_back("");

            /* Footer. */
            auto footer =
                fmt(ANSI_FAINT "q quit  ↑↓ PgUp PgDn scroll  t tree  c compact  s sort (%s)  +/- refresh (%.1fs)",
                    sortNames[static_cast<size_t>(sortOrder)],
                    std::chrono::duration<double>(refreshInterval).count());
            if (bodySize > bodyHeight)
                footer += fmt("  [%d-%d/%d]", scroll + 1, std::min(scroll + bodyHeight, bodySize), bodySize);
            lines.push_back(footer + ANSI_NORMAL);

            /* Write the frame in one go, overwriting the previous one. Don't write a newline after the last line
               to prevent scrolling. */
            std::string frame = clearScreen ? "\e[2J\e[H" : "\e[H";
            clearScreen = false;
            for (const auto & [n, line] : enumerate(lines)) {
                if (n == rows)
                    break;
                if (n)
                    frame += "\n";
                frame += filterANSIEscapes(line, false, cols);
                frame += ANSI_NORMAL "\e[K";
            }
            frame += "\e[J";
            writeFull(STDOUT_FILENO, frame);
        };

        enum class Key {
            None,
            Quit,
            Up,
            Down,
            PageUp,
            PageDown,
            Home,
            End,
            ToggleTree,
            ToggleCompact,
            Sort,
            Faster,
            Slower,
            Refresh,
            Redraw
        };

        auto parseKey = [](std::string_view & input) -> Key {
            auto c = input[0];
            if (c == '\e') {
                /* Parse an escape sequence (CSI or SS3). Incomplete or unknown sequences are discarded. */
                if (input.size() < 3 || (input[1] != '[' && input[1] != 'O')) {
                    input.remove_prefix(1);
                    return Key::None;
                }
                size_t end = 2;
                while (end < input.size() && !(input[end] >= 0x40 && input[end] <= 0x7e))
                    end++;
                if (end == input.size()) {
                    input = {};
                    return Key::None;
                }
                auto seq = input.substr(1, end);
                input.remove_prefix(end + 1);
                if (seq == "[A" || seq == "OA")
                    return Key::Up;
                if (seq == "[B" || seq == "OB")
                    return Key::Down;
                if (seq == "[5~")
                    return Key::PageUp;
                if (seq == "[6~")
                    return Key::PageDown;
                if (seq == "[H" || seq == "OH" || seq == "[1~" || seq == "[7~")
                    return Key::Home;
                if (seq == "[F" || seq == "OF" || seq == "[4~" || seq == "[8~")
                    return Key::End;
                return Key::None;
            }
            input.remove_prefix(1);
            switch (c) {
            case 'q':
            case 'Q':
            case 0x03: // Ctrl-C
                return Key::Quit;
            case 'k':
                return Key::Up;
            case 'j':
                return Key::Down;
            case ' ':
                return Key::Refresh;
            case 'g':
                return Key::Home;
            case 'G':
                return Key::End;
            case 't':
            case '\r':
            case '\n':
                return Key::ToggleTree;
            case 'c':
                return Key::ToggleCompact;
            case 's':
                return Key::Sort;
            case '+':
            case '=':
                return Key::Slower;
            case '-':
                return Key::Faster;
            case 0x0c: // Ctrl-L
                return Key::Redraw;
            default:
                return Key::None;
            }
        };

        /* Take the second sample quickly so that CPU usage is shown soon after startup. */
        auto nextSample = Clock::now();
        bool firstSample = true;
        bool dirty = true;
        auto lastWindowSize = getWindowSize();

        while (true) {
            checkInterrupt();

            if (Clock::now() >= nextSample) {
                takeSample();
                nextSample =
                    Clock::now()
                    + (firstSample ? std::min(refreshInterval, std::chrono::milliseconds(500)) : refreshInterval);
                firstSample = false;
                dirty = true;
            }

            if (auto windowSize = getWindowSize(); windowSize != lastWindowSize) {
                lastWindowSize = windowSize;
                clearScreen = true;
                dirty = true;
            }

            if (dirty) {
                render();
                dirty = false;
            }

            /* Wait for input or the next sample. Wake up periodically to notice window size changes and
               interrupts. */
            auto timeout = std::clamp(
                std::chrono::duration_cast<std::chrono::milliseconds>(nextSample - Clock::now()),
                std::chrono::milliseconds(0),
                std::chrono::milliseconds(250));
            struct pollfd pfd{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
            auto res = poll(&pfd, 1, timeout.count());
            if (res == -1) {
                if (errno == EINTR)
                    continue;
                throw SysError("waiting for terminal input");
            }
            if (res == 0)
                continue;

            char buf[256];
            auto n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n == -1) {
                if (errno == EINTR || errno == EAGAIN)
                    continue;
                throw SysError("reading from the terminal");
            }
            if (n == 0) {
                /* The terminal has gone away. */
                if (pfd.revents & (POLLHUP | POLLERR))
                    return;
                continue;
            }

            auto pageSize = std::max<size_t>(getWindowSize().first, 4) - 3;

            std::string_view input(buf, n);
            while (!input.empty()) {
                switch (parseKey(input)) {
                case Key::None:
                    break;
                case Key::Quit:
                    return;
                case Key::Up:
                    scroll = scroll > 0 ? scroll - 1 : 0;
                    break;
                case Key::Down:
                    scroll++;
                    break;
                case Key::PageUp:
                    scroll = scroll > pageSize ? scroll - pageSize : 0;
                    break;
                case Key::PageDown:
                    scroll += pageSize;
                    break;
                case Key::Home:
                    scroll = 0;
                    break;
                case Key::End:
                    scroll = std::numeric_limits<size_t>::max() / 2;
                    break;
                case Key::ToggleTree:
                    showTree = !showTree;
                    break;
                case Key::ToggleCompact:
                    compact = !compact;
                    break;
                case Key::Sort:
                    sortOrder = static_cast<SortOrder>((static_cast<int>(sortOrder) + 1) % sortNames.size());
                    break;
                case Key::Slower:
                    refreshInterval += std::chrono::milliseconds(500);
                    nextSample = lastSample.value_or(Clock::now()) + refreshInterval;
                    break;
                case Key::Faster:
                    refreshInterval =
                        std::max(refreshInterval - std::chrono::milliseconds(500), std::chrono::milliseconds(500));
                    nextSample = lastSample.value_or(Clock::now()) + refreshInterval;
                    break;
                case Key::Refresh:
                    nextSample = Clock::now();
                    break;
                case Key::Redraw:
                    clearScreen = true;
                    break;
                }
            }

            dirty = true;
        }
    }
};

static auto rCmdTop = registerCommand2<CmdTop>({"top"});

#endif
