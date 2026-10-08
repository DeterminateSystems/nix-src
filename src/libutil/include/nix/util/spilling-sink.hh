#pragma once
///@file

#include "nix/util/serialise.hh"

namespace nix {

/**
 * A sink that accumulates data in memory up to `maxMemory` bytes, and
 * spills to an anonymous temporary file once that limit is exceeded.
 * This is useful when the size of some data must be known before it
 * can be written, without using unbounded memory.
 */
struct SpillingStringSink : Sink
{
    explicit SpillingStringSink(size_t maxMemory)
        : maxMemory(maxMemory)
    {
    }

    void operator()(std::string_view data) override;

    /**
     * Total number of bytes written.
     */
    uint64_t size() const
    {
        return written;
    }

    /**
     * Whether the data has been spilled to disk.
     */
    bool isSpilled() const
    {
        return (bool) fd;
    }

    /**
     * Return a source that reads back everything written so far. No
     * more data may be written after calling this. The source must
     * not outlive this sink.
     */
    std::unique_ptr<Source> getSource();

private:
    size_t maxMemory;
    uint64_t written = 0;
    bool finished = false;
    StringSink memory;
    AutoCloseFD fd;
    std::optional<FdSink> fileSink;
};

} // namespace nix
