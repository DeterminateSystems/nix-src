#pragma once
///@file

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "nix/util/file-descriptor.hh"

namespace nix {

/**
 * Determine whether \param fd is a terminal.
 */
bool isTTY(Descriptor fd);

/**
 * Determine whether ANSI escape sequences are appropriate for the
 * present output.
 */
bool isTTY();

/**
 * Truncate a string to 'width' printable characters. If 'filterAll'
 * is true, all ANSI escape sequences are filtered out. Otherwise,
 * some escape sequences (such as colour setting) are copied but not
 * included in the character count. Also, tabs are expanded to
 * spaces.
 */
std::string filterANSIEscapes(
    std::string_view s, bool filterAll = false, unsigned int width = std::numeric_limits<unsigned int>::max());

/**
 * Recalculate the window size, updating a global variable.
 *
 * Used in the `SIGWINCH` signal handler on Unix, for example.
 */
void updateWindowSize();

/**
 * @return the number of rows and columns of the terminal.
 *
 * The value is cached so this is quick. The cached result is computed
 * by `updateWindowSize()`.
 */
std::pair<unsigned short, unsigned short> getWindowSize();

/**
 * @return The number of columns of the terminal, or std::numeric_limits<unsigned int>::max() if unknown.
 */
unsigned int getWindowWidth();

/**
 * Get the slave name of a pseudoterminal in a thread-safe manner.
 *
 * @param fd The file descriptor of the pseudoterminal master
 * @return The slave device name as a string
 */
std::string getPtsName(int fd);

/**
 * Program Status Protocol states.
 * https://www.superlogical.com/rex/docs/build/program-status
 */
enum class ProgramState : uint8_t { Idle, Working, Done, Blocked, Error };

/** Action type for the user to do while blocked. */
enum class ProgramStatusKind { Permission, Question, Auth };

/**
 * Reports are always attributed to `app=nix`.
 *
 * @param id The record to report to, such as the process id. In the format
 * `segment ("/" segment)*` where `segment := [A-Za-z0-9_.+-]{1,32}`.
 * @param msg A one line readable description without escape sequences,
 * control characters, and malformed UTF-8 replaced with U+FFFD.
 * @param progress Percentage from 0 to 100, only for `Working` and `Blocked` states
 * @param kind What the user needs to do, only for `Blocked` states
 * @return An OSC 7501 sequence string that reports program state
 */
std::string makeProgramStatusReport(
    ProgramState state,
    std::string_view id,
    std::string_view msg = "",
    std::optional<uint64_t> progress = {},
    std::optional<ProgramStatusKind> kind = {});

/**
 * @param id The record to clear.
 * @return An OSC 7501 sequence that removes the program's status
 */
std::string makeProgramStatusClearReport(std::string_view id);

} // namespace nix
