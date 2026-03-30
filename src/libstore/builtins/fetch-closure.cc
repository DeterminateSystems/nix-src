#include "nix/store/builtins.hh"
#include "nix/store/parsed-derivations.hh"
#include "nix/store/store-open.hh"
#include "nix/store/realisation.hh"
#include "nix/store/make-content-addressed.hh"
#include "nix/store/nar-info-disk-cache.hh"
#include "nix/store/nar-info.hh"
#include "nix/store/filetransfer.hh"
#include "nix/store/globals.hh"
#include "nix/store/store-dir-config.hh"
#include "nix/util/url.hh"
#include "nix/util/archive.hh"
#include "nix/util/compression.hh"
#include "nix/util/file-system.hh"
#include "nix/util/util.hh"

#include <nlohmann/json.hpp>
#include <filesystem>

namespace nix {

static void builtinFetchClosure(const BuiltinBuilderContext & ctx)
{
    experimentalFeatureSettings.require(Xp::FetchClosure);

    auto out = get(ctx.drv.outputs, "out");
    if (!out)
        throw Error("'builtin:fetch-closure' requires an 'out' output");

    if (!ctx.drv.structuredAttrs)
        throw Error("'builtin:fetch-closure' must have '__structuredAttrs = true'");

    auto & attrs = ctx.drv.structuredAttrs->structuredAttrs;

    // Parse attributes
    std::optional<std::string> fromStoreUrl;
    std::optional<StorePath> fromPath;
    std::optional<StorePath> toPath;
    bool inputAddressed = false;

    if (auto it = attrs.find("fromStore"); it != attrs.end() && it->second.is_string()) {
        fromStoreUrl = it->second.get<std::string>();
    }

    if (auto it = attrs.find("fromPath"); it != attrs.end() && it->second.is_string()) {
        auto pathStr = it->second.get<std::string>();
        // Extract basename if full path provided
        auto lastSlash = pathStr.rfind('/');
        fromPath = StorePath(lastSlash != std::string::npos ? pathStr.substr(lastSlash + 1) : pathStr);
    }

    if (auto it = attrs.find("toPath"); it != attrs.end() && it->second.is_string()) {
        auto pathStr = it->second.get<std::string>();
        // Extract basename if full path provided
        auto lastSlash = pathStr.rfind('/');
        toPath = StorePath(lastSlash != std::string::npos ? pathStr.substr(lastSlash + 1) : pathStr);
    }

    if (auto it = attrs.find("inputAddressed"); it != attrs.end() && it->second.is_boolean()) {
        inputAddressed = it->second.get<bool>();
    }

    if (!fromStoreUrl)
        throw Error("'builtin:fetch-closure' requires 'fromStore' attribute");

    if (!fromPath)
        throw Error("'builtin:fetch-closure' requires 'fromPath' attribute");

    // Validate URL
    auto parsedURL = parseURL(*fromStoreUrl, /*lenient=*/true);

    if (parsedURL.scheme != "http" && parsedURL.scheme != "https")
        throw Error("'builtin:fetch-closure' only supports http:// and https:// stores");

    if (!parsedURL.query.empty())
        throw Error("'builtin:fetch-closure' does not support URL query parameters (in '%s')", *fromStoreUrl);

    std::cerr << fmt("fetching closure '%s' from '%s'...\n",
        fromPath->to_string(), *fromStoreUrl);

    // Open the remote store to get its storeDir
    auto fromStore = openStore(*fromStoreUrl);
    auto remoteStoreDir = fromStore->storeDir;

    // Extract local store directory from derivation output path
    auto derivationOutputPath = ctx.outputs.at("out");
    auto lastSlash = derivationOutputPath.rfind('/');
    if (lastSlash == std::string::npos)
        throw Error("invalid output path '%s'", derivationOutputPath);
    Path localStoreDir = derivationOutputPath.substr(0, lastSlash);

    // Verify store directories match
    if (remoteStoreDir != localStoreDir)
        throw Error("store directory mismatch: remote store uses '%s' but local store uses '%s'",
            remoteStoreDir, localStoreDir);

    // Create a fresh FileTransfer since we're in a forked process
    auto fileTransfer = makeFileTransfer();

    // Download and parse .narinfo to get metadata
    auto narInfoUrl = parsedURL.to_string();
    if (!hasSuffix(narInfoUrl, "/")) narInfoUrl += "/";
    narInfoUrl += fromPath->hashPart() + ".narinfo";

    StoreDirConfig storeDirConfig{.storeDir = remoteStoreDir};

    std::shared_ptr<NarInfo> narInfo;
    try {
        FileTransferRequest request(VerbatimURL{narInfoUrl});
        auto result = fileTransfer->download(request);
        narInfo = std::make_shared<NarInfo>(storeDirConfig, result.data, narInfoUrl);

        // Verify the path matches
        if (narInfo->path != *fromPath)
            throw Error("NAR info path mismatch: expected '%s', got '%s'",
                fromPath->to_string(), narInfo->path.to_string());
    } catch (FileTransferError & e) {
        throw Error("failed to fetch NAR info for '%s' from '%s': %s",
            fromPath->to_string(), *fromStoreUrl, e.what());
    }

    // Validate content-addressed vs input-addressed
    bool isCA = narInfo->ca.has_value();

    if (inputAddressed && isCA)
        throw Error("The store object at '%s' is content-addressed, but 'inputAddressed' is set to 'true'",
            fromPath->to_string());

    if (!inputAddressed && !isCA)
        throw Error("The store object at '%s' is input-addressed, but 'inputAddressed' is not set.\n\n"
            "Add 'inputAddressed = true;' if you intend to fetch an input-addressed store path.",
            fromPath->to_string());

    // Derive the output path from fromPath (or toPath if rewriting)
    auto outputStorePath = toPath ? *toPath : *fromPath;
    std::string outputPath = localStoreDir + "/" + outputStorePath.to_string();

    // Download and unpack NAR
    auto narUrl = parsedURL.to_string();
    if (!hasSuffix(narUrl, "/")) narUrl += "/";
    narUrl += narInfo->url;

    // If rewriting, we need to unpack to a temporary location first
    Path unpackPath = outputPath;
    std::optional<Path> tempPath;

    if (toPath) {
        // Create temporary directory for unpacking
        tempPath = outputPath + ".tmp";
        unpackPath = *tempPath;
    }

    auto source = sinkToSource([&](Sink & sink) {
        auto decompressor = makeDecompressionSink(narInfo->compression, sink);
        FileTransferRequest request(VerbatimURL{narUrl});
        fileTransfer->download(std::move(request), *decompressor);
        decompressor->finish();
    });

    restorePath(unpackPath, *source);

    // Rewrite store path references if toPath is provided
    if (toPath) {
        std::cerr << fmt("rewriting references from '%s' to '%s'...\n",
            fromPath->to_string(), toPath->to_string());

        StringMap rewrites;
        rewrites[std::string(fromPath->to_string())] = std::string(toPath->to_string());

        // Recursively rewrite all files
        std::function<void(const Path &)> rewritePath = [&](const Path & path) {
            for (auto & entry : std::filesystem::directory_iterator(path)) {
                auto entryPath = entry.path().string();
                if (entry.is_directory() && !entry.is_symlink()) {
                    rewritePath(entryPath);
                } else if (entry.is_regular_file()) {
                    auto content = readFile(entryPath);
                    auto newContent = rewriteStrings(content, rewrites);
                    if (newContent != content)
                        writeFile(entryPath, newContent);
                }
            }
        };

        rewritePath(unpackPath);

        // Move to final output path
        std::filesystem::rename(unpackPath, outputPath);
    }
}

static RegisterBuiltinBuilder registerFetchClosure("fetch-closure", builtinFetchClosure);

} // namespace nix
