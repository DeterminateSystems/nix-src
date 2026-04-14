#include "nix/cmd/command.hh"
#include "nix/util/file-system.hh"
#include "nix/util/signals.hh"
#include "nix/fetchers/git-utils.hh" // for Deleter
#include "nix/store/nar-info.hh"

#include <future>
#include <regex>

#include <arpa/inet.h>
#include <microhttpd.h>

using namespace nix;

using Response = std::unique_ptr<MHD_Response, Deleter<MHD_destroy_response>>;

struct CmdServe : StoreCommand
{
    uint16_t port = 8080;
    std::optional<int> priority;
    std::optional<std::filesystem::path> portFile;

    CmdServe()
    {
        addFlag({
            .longName = "port",
            .shortName = 'p',
            .description = "Port to listen on (default: 8080). Use 0 to dynamically allocate a free port.",
            .labels = {"port"},
            .handler = {&port},
        });
        addFlag({
            .longName = "port-file",
            .description = "Write the bound port number to this file.",
            .labels = {"path"},
            .handler = {[this](std::string s) { portFile = s; }},
        });
        addFlag({
            .longName = "priority",
            .description = "Priority of this cache (overrides the store's default).",
            .labels = {"priority"},
            .handler = {[this](std::string s) { priority = std::stoi(s); }},
        });
    }

    std::string description() override
    {
        return "serve a Nix store over the network";
    }

    MHD_Result
    handleRequest(Store & store, MHD_Connection * connection, const std::string & url, std::string_view method)
    try {
        std::string clientAddr = "unknown";
        if (auto * info = MHD_get_connection_info(connection, MHD_CONNECTION_INFO_CLIENT_ADDRESS)) {
            char buf[INET6_ADDRSTRLEN];
            auto * addr = info->client_addr;
            const void * src = addr->sa_family == AF_INET6 ? (void *) &((sockaddr_in6 *) addr)->sin6_addr
                                                           : (void *) &((sockaddr_in *) addr)->sin_addr;
            if (inet_ntop(addr->sa_family, src, buf, sizeof(buf)))
                clientAddr = buf;
        }

        notice("request: client=%s, method=%s, url=%s", clientAddr, method, url);

        Response response;

        auto notFound = [&] {
            static constexpr std::string_view body = "404 not found\n";
            response.reset(MHD_create_response_from_buffer(body.size(), (void *) body.data(), MHD_RESPMEM_PERSISTENT));
            return MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, response.get());
        };

        static const std::regex narInfoUrlRegex{R"(^/([0-9a-z]+)\.narinfo$)"};
        static const std::regex narUrlRegex{R"(^/nar/([0-9a-z]+)-([0-9a-z]+)\.nar$)"};

        if (method != MHD_HTTP_METHOD_GET && method != MHD_HTTP_METHOD_HEAD) {
            std::string_view body = "405 method not allowed\n";
            response.reset(MHD_create_response_from_buffer(body.size(), (void *) body.data(), MHD_RESPMEM_PERSISTENT));
            MHD_add_response_header(response.get(), "Allow", MHD_HTTP_METHOD_GET);
            return MHD_queue_response(connection, MHD_HTTP_METHOD_NOT_ALLOWED, response.get());
        }

        if (url == "/nix-cache-info") {
            auto body = std::make_unique<std::string>(
                        "StoreDir: " + store.storeDir + "\n"
                        "WantMassQuery: " + (store.config.wantMassQuery ? "1" : "0") + "\n"
                        "Priority: " + std::to_string(priority.value_or(store.config.priority)) + "\n");
            response.reset(MHD_create_response_from_buffer(body->size(), body->data(), MHD_RESPMEM_MUST_COPY));
            MHD_add_response_header(response.get(), "Content-Type", "text/x-nix-cache-info");

        } else if (std::smatch m; std::regex_match(url, m, narInfoUrlRegex)) {
            auto hashPart = m[1].str();
            auto path = store.queryPathFromHashPart(hashPart);
            if (!path)
                return notFound();

            auto info = store.queryPathInfo(*path);
            NarInfo ni(*info);
            ni.compression = "none";
            // FIXME: would be nicer to use just the NAR hash, but we can't look up NARs by NAR hash.
            ni.url = "nar/" + std::string(info->path.hashPart()) + "-"
                     + info->narHash.to_string(HashFormat::Nix32, false) + ".nar";
            ni.fileSize = info->narSize;
            auto body = ni.to_string(store);
            response.reset(MHD_create_response_from_buffer(body.size(), body.data(), MHD_RESPMEM_MUST_COPY));
            MHD_add_response_header(response.get(), "Content-Type", "text/x-nix-narinfo");

        } else if (std::smatch m; std::regex_match(url, m, narUrlRegex)) {
            auto hashPart = m[1].str();
            auto expectedNarHash = m[2].str();
            auto path = store.queryPathFromHashPart(hashPart);
            if (!path)
                return notFound();

            auto info = store.queryPathInfo(*path);
            if (info->narHash.to_string(HashFormat::Nix32, false) != expectedNarHash)
                return notFound();

            StringSink sink;
            store.narFromPath(*path, sink);
            response.reset(MHD_create_response_from_buffer(sink.s.size(), sink.s.data(), MHD_RESPMEM_MUST_COPY));
            MHD_add_response_header(response.get(), "Content-Type", "application/x-nix-nar");

        } else
            return notFound();

        return MHD_queue_response(connection, MHD_HTTP_OK, response.get());

    } catch (const Error & e) {
        auto body = fmt("500 Internal Server Error\n\nError: %s", e.message());
        Response response;
        response.reset(MHD_create_response_from_buffer(body.size(), body.data(), MHD_RESPMEM_MUST_COPY));
        MHD_add_response_header(response.get(), "Content-Type", "text/plain");
        return MHD_queue_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, response.get());
    }

    void run(ref<Store> store) override
    {
        struct Ctx
        {
            Store & store;
            CmdServe & cmd;
        };

        Ctx ctx{*store, *this};

        auto handler = [](void * cls,
                          MHD_Connection * connection,
                          const char * url,
                          const char * method,
                          const char * version,
                          const char * upload_data,
                          size_t * upload_data_size,
                          void ** con_cls) -> MHD_Result {
            auto & ctx = *static_cast<Ctx *>(cls);
            auto & store = ctx.store;
            auto & cmd = ctx.cmd;
            return cmd.handleRequest(store, connection, std::string(url), method);
        };

        auto * daemon = MHD_start_daemon(
            MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_DUAL_STACK,
            port,
            nullptr,
            nullptr,
            handler,
            &ctx,
            MHD_OPTION_END);

        if (!daemon)
            throw Error("failed to start HTTP daemon on port %d", port);

        auto * info = MHD_get_daemon_info(daemon, MHD_DAEMON_INFO_BIND_PORT);
        uint16_t boundPort = info ? info->port : port;
        notice("Listening on http://[::]:%d/", boundPort);

        if (portFile)
            writeFile(*portFile, std::to_string(boundPort) + "\n");

        /* Wait for Ctrl-C. */
        std::promise<void> interruptPromise;
        std::future<void> interruptFuture = interruptPromise.get_future();
        auto callback = createInterruptCallback([&]() { interruptPromise.set_value(); });
        interruptFuture.get();

        notice("Shutting down...");
        MHD_stop_daemon(daemon);
    }
};

static auto rCmdServe = registerCommand<CmdServe>("serve");
