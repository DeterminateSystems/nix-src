#include "nix/expr/primops.hh"
#include "nix/expr/eval-inline.hh"

#include <wasmtime.hh>

using namespace wasmtime;

namespace nix {

// FIXME
SourcePath realisePath(
    EvalState & state,
    const PosIdx pos,
    Value & v,
    std::optional<SymlinkResolution> resolveSymlinks = SymlinkResolution::Full);

using ValueId = uint32_t;

template<typename T, typename E = Error>
T unwrap(Result<T, E> && res)
{
    if (res)
        return res.ok();
    throw Error(res.err().message());
}

static Engine & getEngine()
{
    static Engine engine;
    return engine;
}

static std::span<uint8_t> string2span(std::string_view s)
{
    return std::span<uint8_t>((uint8_t *) s.data(), s.size());
}

static std::string_view span2string(std::span<uint8_t> s)
{
    return std::string_view((char *) s.data(), s.size());
}

template<typename T>
static std::span<T> subspan(std::span<uint8_t> s, size_t len)
{
    assert(s.size() >= len * sizeof(T));
    return std::span((T *) s.data(), len);
}

template<typename R, typename... Args>
static void regFun_(Linker & linker, std::string_view name, std::function<R(Args...)> f)
{
    unwrap(linker.func_wrap("env", name, [f(std::move(f))](Args... args) -> Result<R, Trap> {
        try {
            return f(args...);
        } catch (Error & e) {
            return Trap(e.what());
        }
    }));
}

template<typename... Args>
static void regFun_(Linker & linker, std::string_view name, std::function<void(Args...)> f)
{
    unwrap(linker.func_wrap("env", name, [f(std::move(f))](Args... args) -> Result<std::monostate, Trap> {
        try {
            f(args...);
            return std::monostate();
        } catch (Error & e) {
            return Trap(e.what());
        }
    }));
}

template<typename F>
static void regFun(Linker & linker, std::string_view name, F f)
{
    regFun_(linker, name, std::function(f));
}

struct NixWasmContext
{
    EvalState & state;
    Engine & engine;
    SourcePath wasmPath;
    std::string functionName;
    Module module;
    wasmtime::Store wasmStore;
    Instance instance;
    Memory memory;

    ValueVector values;
    std::exception_ptr ex;

    NixWasmContext(EvalState & _state, SourcePath _wasmPath, std::string _functionName)
        : state(_state)
        , engine(getEngine())
        , wasmPath(_wasmPath)
        , functionName(_functionName)
        , module(unwrap(Module::compile(engine, string2span(wasmPath.readFile()))))
        , wasmStore(engine)
        , instance(({
            Linker linker(engine);

            regFun(linker, "panic", [this](Caller caller, uint32_t ptr, uint32_t len) {
                throw Error("WASM panic: %s", Uncolored(span2string(memory.data(caller).subspan(ptr, len))));
            });

            regFun(linker, "warn", [this](Caller caller, uint32_t ptr, uint32_t len) {
                nix::warn(
                    "'%s' function '%s': %s",
                    wasmPath,
                    functionName,
                    span2string(memory.data(caller).subspan(ptr, len)));
            });

            regFun(linker, "get_type", [this](Caller caller, ValueId valueId) -> uint32_t {
                auto & value = *values.at(valueId);
                state.forceValue(value, noPos);
                auto t = value.type();
                return t == nInt        ? 1
                       : t == nFloat    ? 2
                       : t == nBool     ? 3
                       : t == nString   ? 4
                       : t == nPath     ? 5
                       : t == nNull     ? 6
                       : t == nAttrs    ? 7
                       : t == nList     ? 8
                       : t == nFunction ? 9
                                        : []() -> int { throw Error("unsupported type"); }();
            });

            regFun(linker, "make_int", [this](Caller caller, int64_t n) -> ValueId {
                auto [valueId, value] = allocValue();
                value.mkInt(n);
                return valueId;
            });

            regFun(linker, "get_int", [this](ValueId valueId) -> int64_t {
                return state.forceInt(*values.at(valueId), noPos, "while evaluating a value from WASM").value;
            });

            regFun(linker, "make_float", [this](Caller caller, double x) -> ValueId {
                auto [valueId, value] = allocValue();
                value.mkFloat(x);
                return valueId;
            });

            regFun(linker, "get_float", [this](Caller caller, ValueId valueId) -> double {
                return state.forceFloat(*values.at(valueId), noPos, "while evaluating a value from WASM");
            });

            regFun(linker, "make_string", [this](Caller caller, uint32_t ptr, uint32_t len) -> ValueId {
                auto [valueId, value] = allocValue();
                value.mkString(span2string(memory.data(caller).subspan(ptr, len)), state.mem);
                return valueId;
            });

            regFun(
                linker,
                "copy_string",
                [this](Caller caller, ValueId valueId, uint32_t ptr, uint32_t maxLen) -> uint32_t {
                    auto s = state.forceString(*values.at(valueId), noPos, "while evaluating a value from WASM");
                    if (s.size() <= maxLen) {
                        auto buf = memory.data(caller).subspan(ptr, maxLen);
                        memcpy(buf.data(), s.data(), s.size());
                    }
                    return s.size();
                });

            regFun(linker, "make_bool", [this](Caller caller, int32_t b) -> ValueId {
                return addValue(state.getBool(b));
            });

            regFun(linker, "get_bool", [this](Caller caller, ValueId valueId) -> int32_t {
                return state.forceBool(*values.at(valueId), noPos, "while evaluating a value from WASM");
            });

            regFun(linker, "make_null", [this](Caller caller) -> ValueId { return addValue(&Value::vNull); });

            regFun(linker, "make_list", [this](Caller caller, uint32_t ptr, uint32_t len) -> ValueId {
                auto vs = subspan<ValueId>(memory.data(caller).subspan(ptr), len);

                auto [valueId, value] = allocValue();

                auto list = state.buildList(len);
                for (const auto & [n, v] : enumerate(list))
                    v = values.at(vs[n]); // FIXME: endianness
                value.mkList(list);

                return valueId;
            });

            regFun(
                linker, "copy_list", [this](Caller caller, ValueId valueId, uint32_t ptr, uint32_t maxLen) -> uint32_t {
                    auto & value = *values.at(valueId);
                    state.forceList(value, noPos, "while getting a list from WASM");

                    if (value.listSize() <= maxLen) {
                        auto out = subspan<ValueId>(memory.data(caller).subspan(ptr), value.listSize());

                        for (const auto & [n, elem] : enumerate(value.listView()))
                            out[n] = addValue(elem);
                    }

                    return value.listSize();
                });

            regFun(linker, "make_attrset", [this](Caller caller, uint32_t ptr, uint32_t len) -> ValueId {
                auto mem = memory.data(caller);

                struct Attr
                {
                    // FIXME: endianness
                    uint32_t attrNamePtr;
                    uint32_t attrNameLen;
                    ValueId value;
                };

                auto attrs = subspan<Attr>(mem.subspan(ptr), len);

                auto [valueId, value] = allocValue();
                auto builder = state.buildBindings(len);
                for (auto & attr : attrs)
                    builder.insert(
                        state.symbols.create(span2string(mem.subspan(attr.attrNamePtr, attr.attrNameLen))),
                        values.at(attr.value));
                value.mkAttrs(builder);

                return valueId;
            });

            regFun(
                linker,
                "copy_attrset",
                [this](Caller caller, ValueId valueId, uint32_t ptr, uint32_t maxLen) -> uint32_t {
                    auto & value = *values.at(valueId);
                    state.forceAttrs(value, noPos, "while copying an attrset into WASM");

                    if (value.attrs()->size() <= maxLen) {
                        // FIXME: endianness.
                        struct Attr
                        {
                            ValueId value;
                            uint32_t nameLen;
                        };

                        auto buf = subspan<Attr>(memory.data(caller).subspan(ptr), maxLen);

                        // FIXME: for determinism, we should return attributes in lexicographically sorted order.
                        for (const auto & [n, attr] : enumerate(*value.attrs())) {
                            buf[n].value = addValue(attr.value);
                            buf[n].nameLen = state.symbols[attr.name].size();
                        }
                    }

                    return value.attrs()->size();
                });

            regFun(
                linker,
                "copy_attrname",
                [this](Caller caller, ValueId valueId, uint32_t attrIdx, uint32_t ptr, uint32_t len) {
                    auto & value = *values.at(valueId);
                    state.forceAttrs(value, noPos, "while copying an attr name into WASM");

                    auto & attrs = *value.attrs();

                    assert((size_t) attrIdx < attrs.size());

                    std::string_view name = state.symbols[attrs[attrIdx].name];

                    assert((size_t) len == name.size());

                    memcpy(memory.data(caller).subspan(ptr, len).data(), name.data(), name.size());
                });

            regFun(
                linker, "call_function", [this](Caller caller, ValueId funId, uint32_t ptr, uint32_t len) -> ValueId {
                    auto & fun = *values.at(funId);
                    state.forceFunction(fun, noPos, "while calling a function from WASM");

                    ValueVector args;
                    for (auto argId : subspan<ValueId>(memory.data(caller).subspan(ptr), len))
                        args.push_back(values.at(argId));

                    auto [valueId, value] = allocValue();

                    state.callFunction(fun, args, value, noPos);

                    return valueId;
                });

            unwrap(linker.instantiate(wasmStore, module));
        }))
        , memory(std::get<Memory>(*instance.get(wasmStore, "memory")))
    {
    }

    ValueId addValue(Value * v)
    {
        auto id = values.size();
        values.emplace_back(v);
        return id;
    }

    std::pair<ValueId, Value &> allocValue()
    {
        auto v = state.allocValue();
        auto id = addValue(v);
        return {id, *v};
    }
};

void prim_wasm(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto wasmPath = realisePath(state, pos, *args[0]);
    std::string functionName =
        std::string(state.forceStringNoCtx(*args[1], pos, "while evaluating the second argument of `builtins.wasm`"));

    try {
        NixWasmContext nixCtx(state, wasmPath, functionName);

        auto init = std::get<Func>(*nixCtx.instance.get(nixCtx.wasmStore, "nix_wasm_init_v1"));
        unwrap(init.call(nixCtx.wasmStore, {}));

        auto run = std::get<Func>(*nixCtx.instance.get(nixCtx.wasmStore, nixCtx.functionName));

        debug("calling wasm module");

        v = *nixCtx.values.at(unwrap(run.call(nixCtx.wasmStore, {(int32_t) nixCtx.addValue(args[2])})).at(0).i32());
    } catch (Error & e) {
        e.addTrace(state.positions[pos], "while executing the WASM function '%s' from '%s'", functionName, wasmPath);
        throw;
    }
}

static RegisterPrimOp primop_fromTOML(
    {.name = "wasm",
     .args = {"wasm", "entry", "arg"},
     .doc = R"(
      Call a WASM function with the specified argument.
     )",
     .fun = prim_wasm});

} // namespace nix
