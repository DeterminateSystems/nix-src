#include "nix/util/hdiff-patch.hh"
#include "nix/util/compression.hh"

#include <zstd.h>

#include <hdiffpatch/libHDiffPatch/HDiff/diff.h>
#include <hdiffpatch/libHDiffPatch/HPatch/patch.h>

/* Don't let the compression plugins print notices on stdout, which
   would end up in the output of e.g. `nix nario export`. */
#define IS_NOTICE_compress_canceled 0
#include <hdiffpatch/compress_plugin_demo.h>
#include <hdiffpatch/decompress_plugin_demo.h>

#include <exception>
#include <vector>

namespace nix {

/* The zstd dictionary size (log2) used for patches. */
static const int zstdDictBits = 24;

/* The minimum score for matches, i.e. `hdiffz -m-6`. */
static const int minSingleMatchScore = 6;

/* Don't trust the memory requirements stated in a patch beyond this. */
static const uint64_t maxStepMemSize = 64 * 1024 * 1024;

std::string makeHdiffPatch(std::string base, std::string target, int level)
{
    TCompressPlugin_zstd compressPlugin = zstdCompressPlugin;
    compressPlugin.compress_level = level;
    compressPlugin.dict_bits = zstdDictBits;
    compressPlugin.thread_num = 1;

    auto bytes = [](std::string & s) { return (unsigned char *) s.data(); };

    std::vector<unsigned char> out;
    try {
        create_single_compressed_diff_block(
            bytes(target),
            bytes(target) + target.size(),
            bytes(base),
            bytes(base) + base.size(),
            out,
            &compressPlugin.base,
            kDefaultPatchStepMemSize,
            kDefaultFastMatchBlockSize,
            minSingleMatchScore,
            /* isUseBigCacheMatch */ false,
            /* threadNum */ 1);
    } catch (std::exception & e) {
        throw CompressionError("computing HDiffPatch patch: %s", e.what());
    }

    return std::string((const char *) out.data(), out.size());
}

namespace {

/**
 * An HDiffPatch output stream that writes to a `Sink`. HDiffPatch
 * writes sequentially. Exceptions thrown by the sink are stored and
 * rethrown after HDiffPatch returns, since they can't propagate
 * through its C code.
 */
struct SinkOutputStream
{
    hpatch_TStreamOutput stream;
    Sink & sink;
    uint64_t written = 0;
    std::exception_ptr exception;

    SinkOutputStream(Sink & sink, uint64_t size)
        : sink(sink)
    {
        stream.streamImport = this;
        stream.streamSize = size;
        stream.read_writed = nullptr;
        stream.write = write;
    }

    static hpatch_BOOL write(
        const hpatch_TStreamOutput * stream,
        hpatch_StreamPos_t writeToPos,
        const unsigned char * data,
        const unsigned char * dataEnd)
    {
        auto & self = *(SinkOutputStream *) stream->streamImport;
        if (writeToPos != self.written)
            return hpatch_FALSE;
        try {
            self.sink({(const char *) data, (size_t) (dataEnd - data)});
        } catch (...) {
            self.exception = std::current_exception();
            return hpatch_FALSE;
        }
        self.written += dataEnd - data;
        return hpatch_TRUE;
    }
};

} // namespace

void applyHdiffPatch(std::string_view base, std::string_view patch, Sink & sink)
{
    auto patchData = (const unsigned char *) patch.data();
    auto baseData = (const unsigned char *) base.data();

    hpatch_singleCompressedDiffInfo info;
    if (!getSingleCompressedDiffInfo_mem(&info, patchData, patchData + patch.size()))
        throw CompressionError("invalid HDiffPatch patch");

    if (info.oldDataSize != base.size())
        throw CompressionError(
            "HDiffPatch patch expects a base of %d bytes, but the base has %d bytes", info.oldDataSize, base.size());

    auto dataSize = info.compressedSize ? info.compressedSize : info.uncompressedSize;
    if (info.diffDataPos > patch.size() || dataSize != patch.size() - info.diffDataPos)
        throw CompressionError("HDiffPatch patch has an invalid size");

    if (info.stepMemSize > maxStepMemSize)
        throw CompressionError("HDiffPatch patch requires too much memory");

    hpatch_TDecompress * decompressPlugin = nullptr;
    if (info.compressType[0]) {
        if (!zstdDecompressPlugin.is_can_open(info.compressType))
            throw CompressionError("HDiffPatch patch uses unsupported compression type '%s'", info.compressType);
        decompressPlugin = &zstdDecompressPlugin;
    }

    std::vector<unsigned char> cache(info.stepMemSize + hpatch_kStreamCacheSize * 3);

    hpatch_TStreamInput baseStream, patchStream;
    mem_as_hStreamInput(&baseStream, baseData, baseData + base.size());
    mem_as_hStreamInput(&patchStream, patchData, patchData + patch.size());

    SinkOutputStream out(sink, info.newDataSize);

    auto ok = patch_single_compressed_diff(
        &out.stream,
        &baseStream,
        &patchStream,
        info.diffDataPos,
        info.uncompressedSize,
        info.compressedSize,
        decompressPlugin,
        info.coverCount,
        (size_t) info.stepMemSize,
        cache.data(),
        cache.data() + cache.size(),
        /* coversListener */ nullptr,
        /* threadNum */ 1);

    if (out.exception)
        std::rethrow_exception(out.exception);

    if (!ok)
        throw CompressionError("applying HDiffPatch patch failed");

    if (out.written != info.newDataSize)
        throw CompressionError("HDiffPatch patch produced %d bytes instead of %d", out.written, info.newDataSize);
}

} // namespace nix
