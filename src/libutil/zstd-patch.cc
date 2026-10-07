#include "nix/util/zstd-patch.hh"
#include "nix/util/compression.hh"
#include "nix/util/signals.hh"

#include <zstd.h>

#include <algorithm>
#include <bit>

namespace nix {

static size_t checkZstd(size_t ret)
{
    if (ZSTD_isError(ret))
        throw CompressionError("zstd error: %s", ZSTD_getErrorName(ret));
    return ret;
}

static int maxWindowLog()
{
    return ZSTD_cParam_getBounds(ZSTD_c_windowLog).upperBound;
}

uint64_t maxZstdPatchWindow()
{
    return uint64_t(1) << maxWindowLog();
}

std::string makeZstdPatch(std::string_view base, std::string_view target, int level)
{
    std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> cctx{ZSTD_createCCtx(), ZSTD_freeCCtx};
    if (!cctx)
        throw CompressionError("unable to initialise zstd encoder");

    checkZstd(ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_compressionLevel, level));

    if (!base.empty()) {
        /* The window must cover both the base (prefix) and the
           target, otherwise the end of the target can't refer to the
           start of the base. */
        uint64_t total = base.size() + target.size();
        int windowLog = std::bit_width(total - 1);
        auto bounds = ZSTD_cParam_getBounds(ZSTD_c_windowLog);
        windowLog = std::clamp(windowLog, bounds.lowerBound, bounds.upperBound);
        checkZstd(ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_windowLog, windowLog));
        checkZstd(ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_enableLongDistanceMatching, 1));
        /* Use the multi-threaded compressor (even with a single
           worker), since the single-threaded one makes poor use of
           the prefix at high compression levels (e.g. a 13 MB patch
           vs 6 KB between two identical 65 MB inputs at level 19).
           Don't checkZstd(): if libzstd was built without
           ZSTD_MULTITHREAD this returns an error, but compression
           still works. */
        ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_nbWorkers, 1);
        checkZstd(ZSTD_CCtx_refPrefix(cctx.get(), base.data(), base.size()));
    }

    std::string out(ZSTD_compressBound(target.size()), '\0');
    auto n = checkZstd(ZSTD_compress2(cctx.get(), out.data(), out.size(), target.data(), target.size()));
    out.resize(n);
    return out;
}

void applyZstdPatch(std::string_view base, std::string_view patch, Sink & sink)
{
    std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> dctx{ZSTD_createDCtx(), ZSTD_freeDCtx};
    if (!dctx)
        throw CompressionError("unable to initialise zstd decoder");

    checkZstd(ZSTD_DCtx_setParameter(dctx.get(), ZSTD_d_windowLogMax, maxWindowLog()));
    if (!base.empty())
        checkZstd(ZSTD_DCtx_refPrefix(dctx.get(), base.data(), base.size()));

    std::vector<char> outbuf(ZSTD_DStreamOutSize());
    ZSTD_inBuffer in = {patch.data(), patch.size(), 0};

    while (true) {
        checkInterrupt();
        ZSTD_outBuffer out = {outbuf.data(), outbuf.size(), 0};
        auto ret = checkZstd(ZSTD_decompressStream(dctx.get(), &out, &in));
        if (out.pos > 0)
            sink({outbuf.data(), out.pos});
        if (ret == 0) {
            if (in.pos != in.size)
                throw CompressionError("trailing garbage after zstd patch");
            break;
        }
        if (in.pos == in.size && out.pos < out.size)
            throw CompressionError("truncated zstd patch");
    }
}

} // namespace nix
