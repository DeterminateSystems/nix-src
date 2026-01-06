#include <wasmtime.hh>

namespace nix {

// FIXME: cut&paste
template<typename T, typename E = Error>
T unwrap(wasmtime::Result<T, E> && res)
{
    if (res)
        return res.ok();
    throw Error(res.err().message());
}

// FIXME: cut&paste
static std::span<uint8_t> string2span(std::string_view s)
{
    return std::span<uint8_t>((uint8_t *) s.data(), s.size());
}

struct WasiDerivationBuilder : DerivationBuilderImpl
{
    WasiDerivationBuilder(
        LocalStore & store, std::unique_ptr<DerivationBuilderCallbacks> miscMethods, DerivationBuilderParams params)
        : DerivationBuilderImpl(store, std::move(miscMethods), std::move(params))
    {
        // experimentalFeatureSettings.require(Xp::WasiBuilders);
    }

    void execBuilder(const Strings & args, const Strings & envStrs) override
    {
        using namespace wasmtime;

        Engine engine;
        Linker linker(engine);
        unwrap(linker.define_wasi());

        WasiConfig wasiConfig;
        wasiConfig.inherit_stdin();
        wasiConfig.inherit_stdout();
        wasiConfig.inherit_stderr();
        wasiConfig.argv(std::vector(args.begin(), args.end()));
        {
            std::vector<std::pair<std::string, std::string>> env2;
            for (auto & [k, v] : env)
                env2.emplace_back(k, rewriteStrings(v, inputRewrites));
            wasiConfig.env(env2);
        }
        if (!wasiConfig.preopen_dir(
                store.config->realStoreDir.get(),
                store.storeDir,
                WASMTIME_WASI_DIR_PERMS_READ | WASMTIME_WASI_DIR_PERMS_WRITE,
                WASMTIME_WASI_FILE_PERMS_READ | WASMTIME_WASI_FILE_PERMS_WRITE))
            throw Error("cannot add store directory to WASI config");
        // FIXME: add temp dir

        auto module = unwrap(Module::compile(engine, string2span(readFile(realPathInHost(drv.builder)))));
        wasmtime::Store wasmStore(engine);
        unwrap(wasmStore.context().set_wasi(std::move(wasiConfig)));
        auto instance = unwrap(linker.instantiate(wasmStore, module));

        auto startName = "_start";
        auto ext = instance.get(wasmStore, startName);
        if (!ext)
            throw Error("WASM module '%s' does not export function '%s'", drv.builder, startName);
        auto fun = std::get_if<Func>(&*ext);
        if (!fun)
            throw Error("export '%s' of WASM module '%s' is not a function", startName, drv.builder);

        unwrap(fun->call(wasmStore.context(), {}));

        _exit(0);
    }
};

} // namespace nix
