#include "nix/util/serialise.hh"
#include "nix/util/coroutine-gc.hh"
#include "nix/util/file-descriptor.hh"
#include "nix/util/signals.hh"
#include "nix/util/socket.hh"
#include "nix/util/util.hh"

#include <cstring>
#include <cerrno>
#include <limits>
#include <memory>

#include <boost/coroutine2/coroutine.hpp>
#include <boost/coroutine2/protected_fixedsize_stack.hpp>

#ifdef _WIN32
#  include <fileapi.h>
#else
#  include <poll.h>
#endif

namespace nix {

void SerialisationError::anchor() {}

void Sink::anchor() {}

void BufferedSink::anchor() {}

void FdSink::anchor() {}

void StringSink::anchor() {}

void TeeSink::anchor() {}

void FinishSink::anchor() {}

void LambdaSink::anchor() {}

void NullSink::anchor() {}

void FramedSink::anchor() {}

void Source::anchor() {}

void BufferedSource::anchor() {}

void RestartableSource::anchor() {}

void FdSource::anchor() {}

void TeeSource::anchor() {}

void StringSource::anchor() {}

void LambdaSource::anchor() {}

void FramedSource::anchor() {}

void LengthSource::anchor() {}

void EnsureRead::anchor() {}

void ChainSource::anchor() {}

void SizedSource::anchor() {}

void BufferedSink::operator()(std::string_view data)
{
    if (!buffer)
        buffer = decltype(buffer)(new char[bufSize]);

    while (!data.empty()) {
        /* Optimisation: bypass the buffer if the data exceeds the
           buffer size. */
        if (data.size() >= bufSize - bufPos) {
            flush();
            writeUnbuffered(data);
            break;
        }
        /* Otherwise, copy the bytes to the buffer.  Flush the buffer
           when it's full. */
        size_t n = data.size() > bufSize - bufPos ? bufSize - bufPos : data.size();
        memcpy(buffer.get() + bufPos, data.data(), n);
        data.remove_prefix(n);
        bufPos += n;
        if (bufPos == bufSize)
            flush();
    }
}

void BufferedSink::flush()
{
    if (bufPos == 0)
        return;
    size_t n = bufPos;
    bufPos = 0; // don't trigger the assert() in ~BufferedSink()
    writeUnbuffered({buffer.get(), n});
}

FdSink::~FdSink()
{
    try {
        flush();
    } catch (...) {
        ignoreExceptionInDestructor();
    }
}

void FdSink::writeUnbuffered(std::string_view data)
{
    written += data.size();
    try {
        writeFull(fd, data);
    } catch (SystemError & e) {
        _good = false;
        throw;
    }
}

bool FdSink::good()
{
    return _good;
}

void Source::operator()(char * data, size_t len)
{
    while (len) {
        size_t n = read(data, len);
        data += n;
        len -= n;
    }
}

void Source::operator()(std::string_view data)
{
    (*this)((char *) data.data(), data.size());
}

void Source::drainInto(Sink & sink)
{
    std::array<char, 8192> buf;
    while (true) {
        try {
            auto n = read(buf.data(), buf.size());
            sink({buf.data(), n});
        } catch (EndOfFile &) {
            break;
        }
    }
}

void Source::drainInto(Sink & sink, uint64_t len)
{
    std::array<char, 65536> buf;
    while (len) {
        checkInterrupt();
        // Until std::saturate_cast is available (C++26)
        auto lenTrunc = static_cast<size_t>(std::min<uint64_t>(len, std::numeric_limits<size_t>::max()));
        auto n = read(buf.data(), std::min(lenTrunc, buf.size()));
        sink({buf.data(), n});
        assert(n <= len);
        len -= n;
    }
}

std::string Source::drain()
{
    StringSink s;
    drainInto(s);
    return std::move(s.s);
}

void Source::skip(size_t len)
{
    std::array<char, 8192> buf;
    while (len) {
        auto n = read(buf.data(), std::min(len, buf.size()));
        assert(n <= len);
        len -= n;
    }
}

size_t BufferedSource::read(char * data, size_t len)
{
    if (!buffer)
        buffer = std::make_unique_for_overwrite<char[]>(bufSize);

    if (!bufPosIn)
        bufPosIn = readUnbuffered(buffer.get(), bufSize);

    /* Copy out the data in the buffer. */
    auto n = std::min(len, bufPosIn - bufPosOut);
    memcpy(data, buffer.get() + bufPosOut, n);
    bufPosOut += n;
    if (bufPosIn == bufPosOut)
        bufPosIn = bufPosOut = 0;
    return n;
}

std::string BufferedSource::readLine(bool eofOk, char terminator)
{
    if (!buffer)
        buffer = std::make_unique_for_overwrite<char[]>(bufSize);

    std::string line;
    while (true) {
        if (bufPosOut < bufPosIn) {
            auto * start = buffer.get() + bufPosOut;
            auto * end = buffer.get() + bufPosIn;
            if (auto * newline = static_cast<char *>(memchr(start, terminator, end - start))) {
                line.append(start, newline - start);
                bufPosOut = (newline - buffer.get()) + 1;
                if (bufPosOut == bufPosIn)
                    bufPosOut = bufPosIn = 0;
                return line;
            }

            line.append(start, end - start);
            bufPosOut = bufPosIn = 0;
        }

        auto handleEof = [&]() -> std::string {
            bufPosOut = bufPosIn = 0;
            if (eofOk)
                return line;
            throw EndOfFile("unexpected EOF reading a line");
        };

        size_t n = 0;
        try {
            n = readUnbuffered(buffer.get(), bufSize);
        } catch (EndOfFile & e) {
            return handleEof();
        }

        if (n == 0) {
            return handleEof();
        }

        bufPosIn = n;
        bufPosOut = 0;
    }
}

bool BufferedSource::hasData()
{
    return bufPosOut < bufPosIn;
}

size_t FdSource::readUnbuffered(char * data, size_t len)
{
    auto n = nix::read(fd, {reinterpret_cast<std::byte *>(data), len});
    if (n == 0) {
        _good = false;
        throw EndOfFile(std::string(*endOfFileError));
    }
    read += n;
    return n;
}

bool FdSource::good()
{
    return _good;
}

bool FdSource::hasData()
{
    if (BufferedSource::hasData())
        return true;

    while (true) {
#ifdef _WIN32
        /* Windows' fd_set is a bounded handle array, so FD_SET can't
           overflow; on Unix use poll() since fd may exceed FD_SETSIZE. */
        fd_set fds;
        FD_ZERO(&fds);
        Socket sock = toSocket(fd);
        FD_SET(sock, &fds);

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;

        auto n = select(sock + 1, &fds, nullptr, nullptr, &timeout);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            throw SysError("polling file descriptor");
        }
        return FD_ISSET(sock, &fds);
#else
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        auto n = poll(&pfd, 1, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            throw SysError("polling file descriptor");
        }
        return n > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
#endif
    }
}

void FdSource::restart()
{
    if (!isSeekable)
        throw Error("can't seek to the start of a file");
    buffer.reset();
    read = bufPosIn = bufPosOut = 0;
    if (lseek(fd, 0, SEEK_SET) == -1)
        throw SysError("seeking to the start of a file");
}

void FdSource::skip(size_t len)
{
    /* Discard data in the buffer. */
    if (len && buffer && bufPosIn - bufPosOut) {
        if (len >= bufPosIn - bufPosOut) {
            len -= bufPosIn - bufPosOut;
            bufPosIn = bufPosOut = 0;
        } else {
            bufPosOut += len;
            len = 0;
        }
    }

#ifndef _WIN32
    /* If we can, seek forward in the file to skip the rest. */
    if (isSeekable && len) {
        if (len > static_cast<size_t>(std::numeric_limits<off_t>::max()))
            throw Error("cannot skip %d bytes: exceeds maximum file offset", len);
        if (lseek(fd, len, SEEK_CUR) == -1) {
            if (errno == ESPIPE)
                isSeekable = false;
            else
                throw SysError("seeking forward in file");
        } else {
            read += len;
            return;
        }
    }
#endif

    /* Otherwise, skip by reading. */
    if (len)
        BufferedSource::skip(len);
}

size_t StringSource::read(char * data, size_t len)
{
    if (pos == s.size())
        throw EndOfFile("end of string reached");
    size_t n = s.copy(data, len, pos);
    pos += n;
    return n;
}

void StringSource::skip(size_t len)
{
    const size_t remain = s.size() - pos;
    if (len > remain) {
        pos = s.size();
        throw EndOfFile("end of string reached");
    }
    pos += len;
}

/* 512KiB is a conservative estimate for deeply nested NARs, which are limited
   to 64 levels. We also tend to allocate rather large buffers on the stack, so
   we should leave plenty of headroom. Note that no evaluation is supposed to
   happen on sourceToSink/sinkToSource coroutine stacks (they are too small for
   that), though the GC hooks below do make them scannable. */
static constexpr size_t defaultCoroutineStackSize = 512 * 1024;

void * (*coroStackRegister)(void * base, size_t size) = nullptr;
void (*coroStackUnregister)(void * cookie) = nullptr;
void * (*coroSwitchTo)(void * cookie, void * callerSp) = nullptr;
void (*coroSwitchBack)(void * prevHandle) = nullptr;
void (*coroMarkSuspended)(void * cookie, void * sp) = nullptr;
void (*coroMarkActive)(void * cookie) = nullptr;

namespace {

/**
 * A stack allocator that reports the coroutine stacks to the GC hooks
 * (see `coroutine-gc.hh`), storing the resulting cookie in the owning
 * object (which must outlive the coroutine).
 */
struct GCTrackedStackAllocator
{
    void ** cookie;

    boost::context::stack_context allocate()
    {
        auto sctx = boost::coroutines2::protected_fixedsize_stack(defaultCoroutineStackSize).allocate();
        *cookie = coroStackRegister ? coroStackRegister(sctx.sp, sctx.size) : nullptr;
        return sctx;
    }

    void deallocate(boost::context::stack_context & sctx)
    {
        if (*cookie) {
            coroStackUnregister(*cookie);
            *cookie = nullptr;
        }
        boost::coroutines2::protected_fixedsize_stack(defaultCoroutineStackSize).deallocate(sctx);
    }
};

/**
 * Notify the GC hooks that the current thread is about to switch onto
 * (and, on destruction, back off) the coroutine stack identified by
 * `cookie`. `callerSp` must be the frame address of the function
 * performing the switch. Construct just before resuming a coroutine
 * (this includes destroying a suspended one, which unwinds on its own
 * stack); the switch back happens when the coroutine yields or
 * finishes.
 */
struct CoroutineGuard
{
    void * prev = nullptr;
    bool active = false;

    CoroutineGuard(void * cookie, void * callerSp)
    {
        if (coroSwitchTo) {
            prev = coroSwitchTo(cookie, callerSp);
            active = true;
        }
    }

    ~CoroutineGuard()
    {
        if (active)
            coroSwitchBack(prev);
    }
};

} // namespace

std::unique_ptr<FinishSink> sourceToSink(fun<void(Source &)> reader)
{
    struct SourceToSink : FinishSink
    {
        typedef boost::coroutines2::coroutine<bool> coro_t;

        fun<void(Source &)> reader;
        std::optional<coro_t::push_type> coro;
        void * stackCookie = nullptr;

        SourceToSink(fun<void(Source &)> reader)
            : reader(reader)
        {
        }

        ~SourceToSink()
        {
            /* Destroying a suspended coroutine unwinds it on its own
               stack, so this too is a switch onto the coroutine
               stack. */
            CoroutineGuard guard{stackCookie, __builtin_frame_address(0)};
            coro.reset();
        }

        std::string_view cur;

        void operator()(std::string_view in) override
        {
            if (in.empty())
                return;
            cur = in;

            CoroutineGuard guard{stackCookie, __builtin_frame_address(0)};

            if (!coro) {
                coro = coro_t::push_type(GCTrackedStackAllocator{&stackCookie}, [&](coro_t::pull_type & yield) {
                    LambdaSource source([&](char * out, size_t out_len) {
                        if (cur.empty()) {
                            if (coroMarkSuspended)
                                coroMarkSuspended(stackCookie, (char *) __builtin_frame_address(0) - 512);
                            yield();
                            if (coroMarkActive)
                                coroMarkActive(stackCookie);
                            if (yield.get())
                                throw EndOfFile("coroutine has finished");
                        }

                        size_t n = cur.copy(out, out_len);
                        cur.remove_prefix(n);
                        return n;
                    });
                    reader(source);
                });
            }

            if (!*coro) {
                unreachable();
            }

            if (!cur.empty()) {
                (*coro)(false);
            }
        }

        void finish() override
        {
            if (coro && *coro) {
                CoroutineGuard guard{stackCookie, __builtin_frame_address(0)};
                (*coro)(true);
            }
        }
    };

    return std::make_unique<SourceToSink>(reader);
}

std::unique_ptr<Source> sinkToSource(fun<void(Sink &)> writer, fun<void()> eof)
{
    struct SinkToSource : Source
    {
        typedef boost::coroutines2::coroutine<std::string_view> coro_t;

        fun<void(Sink &)> writer;
        fun<void()> eof;
        std::optional<coro_t::pull_type> coro;
        void * stackCookie = nullptr;

        SinkToSource(fun<void(Sink &)> writer, fun<void()> eof)
            : writer(writer)
            , eof(eof)
        {
        }

        ~SinkToSource()
        {
            /* See `SourceToSink::~SourceToSink()`. */
            CoroutineGuard guard{stackCookie, __builtin_frame_address(0)};
            coro.reset();
        }

        std::string_view cur;

        size_t read(char * data, size_t len) override
        {
            CoroutineGuard guard{stackCookie, __builtin_frame_address(0)};

            bool hasCoro = coro.has_value();
            if (!hasCoro) {
                coro = coro_t::pull_type(GCTrackedStackAllocator{&stackCookie}, [&](coro_t::push_type & yield) {
                    /* Feed the consumer in chunks, instead of on each write
                       to avoid excessive context switching. parseDump does
                       lots of small writes to the sink, which we should
                       accumulate. */
                    struct CoroBufferedSink : BufferedSink
                    {
                        coro_t::push_type & yield;
                        void * stackCookie;

                        void writeUnbuffered(std::string_view data) override
                        {
                            if (coroMarkSuspended)
                                coroMarkSuspended(stackCookie, (char *) __builtin_frame_address(0) - 512);
                            yield(data);
                            if (coroMarkActive)
                                coroMarkActive(stackCookie);
                        }

                        CoroBufferedSink(coro_t::push_type & yield, void * stackCookie)
                            : yield(yield)
                            , stackCookie(stackCookie)
                        {
                        }
                    };

                    CoroBufferedSink sink(yield, stackCookie);
                    writer(sink);
                    sink.flush();
                });
            }

            if (cur.empty()) {
                if (hasCoro && *coro) {
                    (*coro)();
                }
                if (*coro) {
                    cur = coro->get();
                } else {
                    coro.reset();
                    eof();
                    unreachable();
                }
            }

            size_t n = cur.copy(data, len);
            cur.remove_prefix(n);

            /* This is necessary to ensure that the coroutine gets resumed
               after the consumer has finished reading the Source. Otherwise the
               coroutine is always abandoned (i.e. it is always destroyed when
               suspended). */
            if (cur.empty() && coro && *coro) {
                (*coro)();
                if (*coro)
                    cur = coro->get();
            }

            return n;
        }
    };

    return std::make_unique<SinkToSource>(writer, eof);
}

void writePadding(size_t len, Sink & sink)
{
    if (len % 8) {
        char zero[8];
        memset(zero, 0, sizeof(zero));
        sink({zero, 8 - (len % 8)});
    }
}

void writeString(std::string_view data, Sink & sink)
{
    sink << data.size();
    sink(data);
    writePadding(data.size(), sink);
}

Sink & operator<<(Sink & sink, std::string_view s)
{
    writeString(s, sink);
    return sink;
}

template<class T>
void writeStrings(const T & ss, Sink & sink)
{
    sink << ss.size();
    for (auto & i : ss)
        sink << i;
}

Sink & operator<<(Sink & sink, const Strings & s)
{
    writeStrings(s, sink);
    return sink;
}

Sink & operator<<(Sink & sink, const StringSet & s)
{
    writeStrings(s, sink);
    return sink;
}

Sink & operator<<(Sink & sink, const Error & ex)
{
    auto & info = ex.info();
    sink << "Error" << info.level << "Error" // removed
         << info.msg.str() << 0              // FIXME: info.errPos
         << info.traces.size();
    for (auto & trace : info.traces) {
        sink << 0; // FIXME: trace.pos
        sink << trace.hint.str();
    }
    return sink;
}

void readPadding(size_t len, Source & source)
{
    if (len % 8) {
        uint64_t zero = 0;
        size_t n = 8 - (len % 8);
        source(reinterpret_cast<char *>(&zero), n);
        if (zero)
            throw SerialisationError("non-zero padding");
    }
}

size_t readString(char * buf, size_t max, Source & source)
{
    auto len = readNum<size_t>(source);
    if (len > max)
        throw SerialisationError("string is too long");
    source(buf, len);
    readPadding(len, source);
    return len;
}

std::string readString(Source & source, size_t max)
{
    auto len = readNum<size_t>(source);
    if (len > max)
        throw SerialisationError("string is too long");
    std::string res(len, 0);
    source(res.data(), len);
    readPadding(len, source);
    return res;
}

Source & operator>>(Source & in, std::string & s)
{
    s = readString(in);
    return in;
}

template<class T>
T readStrings(Source & source)
{
    auto count = readNum<size_t>(source);
    T ss;
    while (count--)
        ss.insert(ss.end(), readString(source));
    return ss;
}

template Strings readStrings(Source & source);
template StringSet readStrings(Source & source);

Error readError(Source & source)
{
    auto type = readString(source);
    if (type != "Error")
        throw SerialisationError("unexpected error type '%s'", type);
    auto level = (Verbosity) readInt(source);
    [[maybe_unused]] auto name = readString(source); // removed
    auto msg = readString(source);
    ErrorInfo info{
        .level = level,
        .msg = HintFmt(msg),
    };
    auto havePos = readNum<size_t>(source);
    if (havePos != 0)
        throw SerialisationError("deserializing error positions is not supported");
    auto nrTraces = readNum<size_t>(source);
    for (size_t i = 0; i < nrTraces; ++i) {
        havePos = readNum<size_t>(source);
        if (havePos != 0)
            throw SerialisationError("deserializing error positions is not supported");
        info.traces.push_back(Trace{.hint = HintFmt(readString(source))});
    }
    return Error(std::move(info));
}

void StringSink::operator()(std::string_view data)
{
    s.append(data);
}

size_t ChainSource::read(char * data, size_t len)
{
    if (useSecond) {
        return source2.read(data, len);
    } else {
        try {
            return source1.read(data, len);
        } catch (EndOfFile &) {
            useSecond = true;
            return this->read(data, len);
        }
    }
}

} // namespace nix
