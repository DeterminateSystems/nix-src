#pragma once
///@file

#include "nix/util/file-descriptor.hh"

#include <string>
#include <thread>

namespace nix {

/**
 * A thread that copies the output of a builder from the master side
 * of its pseudoterminal to a pipe.
 *
 * When the last file descriptor for the slave side of a
 * pseudoterminal is closed, macOS waits at most 0.6 s for the master
 * side to be read and then discards the output that is still
 * queued. The worker can take much longer than that to get around to
 * reading the output of a builder (e.g. when it's starting other
 * builds or registering the outputs of another build), so if it read
 * the master side itself, the last output of a builder that exits in
 * the meantime would be missing from the build log. This thread does
 * nothing else, so it reads the master side in time. The pipe keeps
 * its contents until the worker reads them.
 *
 * The pipe gets EOF when the master side does, i.e. when the slave
 * side has been closed by all processes that had it open.
 */
struct PtyRelay
{
    /**
     * @param master The master side of the pseudoterminal.
     *
     * @param slaveName The file name of its slave side.
     */
    PtyRelay(AutoCloseFD master, std::string slaveName);

    /**
     * Stop the thread. Output that hasn't been read from the
     * pseudoterminal yet is lost.
     */
    ~PtyRelay();

    /**
     * The read side of the pipe.
     */
    AutoCloseFD out;

private:

    /**
     * The thread stops when this is closed.
     */
    AutoCloseFD cancel;

    std::thread thread;
};

} // namespace nix
