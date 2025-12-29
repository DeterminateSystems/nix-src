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
    throw Error("wasmtime failure: %s", res.err().message());
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

    std::vector<RootValue> values;
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

            unwrap(linker.func_wrap("env", "warn", [this](Caller caller, uint32_t ptr, uint32_t len) {
                nix::warn(
                    "'%s' function '%s': %s",
                    wasmPath,
                    functionName,
                    span2string(memory.data(caller).subspan(ptr, len)));
            }));

            unwrap(linker.func_wrap("env", "get_type", [this](Caller caller, ValueId valueId) -> uint32_t {
                auto & value = **values.at(valueId);
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
            }));

            unwrap(linker.func_wrap("env", "make_int", [this](Caller caller, int64_t n) -> ValueId {
                auto [valueId, value] = allocValue();
                value.mkInt(n);
                return valueId;
            }));

            unwrap(linker.func_wrap("env", "get_int", [this](Caller caller, ValueId valueId) -> int64_t {
                return state.forceInt(**values.at(valueId), noPos, "while evaluating a value from WASM").value;
            }));

            unwrap(linker.func_wrap("env", "make_float", [this](Caller caller, double x) -> ValueId {
                auto [valueId, value] = allocValue();
                value.mkFloat(x);
                return valueId;
            }));

            unwrap(linker.func_wrap("env", "get_float", [this](Caller caller, ValueId valueId) -> double {
                return state.forceFloat(**values.at(valueId), noPos, "while evaluating a value from WASM");
            }));

            unwrap(linker.func_wrap("env", "make_string", [this](Caller caller, uint32_t ptr, uint32_t len) -> ValueId {
                auto [valueId, value] = allocValue();
                value.mkString(span2string(memory.data(caller).subspan(ptr, len)), state.mem);
                return valueId;
            }));

            unwrap(linker.func_wrap("env", "get_string_length", [this](Caller caller, ValueId valueId) -> uint32_t {
                return state.forceString(**values.at(valueId), noPos, "while getting a string length from WASM").size();
            }));

            unwrap(linker.func_wrap(
                "env", "copy_string", [this](Caller caller, ValueId valueId, uint32_t ptr, uint32_t len) {
                    auto s = state.forceString(**values.at(valueId), noPos, "while evaluating a value from WASM");
                    auto buf = memory.data(caller).subspan(ptr, len);
                    assert(buf.size() == s.size());
                    memcpy(buf.data(), s.data(), s.size());
                }));

            unwrap(linker.func_wrap("env", "make_bool", [this](Caller caller, int32_t b) -> ValueId {
                return addValue(state.getBool(b));
            }));

            unwrap(linker.func_wrap("env", "get_bool", [this](Caller caller, ValueId valueId) -> int32_t {
                return state.forceBool(**values.at(valueId), noPos, "while evaluating a value from WASM");
            }));

            unwrap(linker.func_wrap(
                "env", "make_null", [this](Caller caller) -> ValueId { return addValue(&Value::vNull); }));

            unwrap(linker.func_wrap("env", "make_list", [this](Caller caller, uint32_t ptr, uint32_t len) -> ValueId {
                auto vs = subspan<ValueId>(memory.data(caller).subspan(ptr), len);

                auto [valueId, value] = allocValue();

                auto list = state.buildList(len);
                for (const auto & [n, v] : enumerate(list))
                    v = *values.at(vs[n]); // FIXME: endianness
                value.mkList(list);

                return valueId;
            }));

            unwrap(linker.func_wrap("env", "get_list_length", [this](Caller caller, ValueId valueId) -> uint32_t {
                auto & value = **values.at(valueId);
                state.forceList(value, noPos, "while getting a list length from WASM");
                return value.listSize();
            }));

            unwrap(linker.func_wrap(
                "env", "copy_list", [this](Caller caller, ValueId valueId, uint32_t ptr, uint32_t len) {
                    auto & value = **values.at(valueId);
                    state.forceList(value, noPos, "while getting a list length from WASM");

                    assert((size_t) len == value.listSize());

                    auto out = subspan<ValueId>(memory.data(caller).subspan(ptr), len);

                    for (const auto & [n, elem] : enumerate(value.listView()))
                        out[n] = addValue(elem);
                }));

            unwrap(
                linker.func_wrap("env", "make_attrset", [this](Caller caller, uint32_t ptr, uint32_t len) -> ValueId {
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
                            *values.at(attr.value));
                    value.mkAttrs(builder);

                    return valueId;
                }));

            unwrap(linker.func_wrap("env", "get_attrset_length", [this](Caller caller, ValueId valueId) -> int32_t {
                auto & value = **values.at(valueId);
                state.forceAttrs(value, noPos, "while getting an attrset length from WASM");
                return value.attrs()->size();
            }));

            unwrap(linker.func_wrap(
                "env", "copy_attrset", [this](Caller caller, ValueId valueId, uint32_t ptr, uint32_t len) {
                    auto & value = **values.at(valueId);
                    state.forceAttrs(value, noPos, "while copying an attrset into WASM");

                    assert((size_t) len == value.attrs()->size());

                    // FIXME: endianness.
                    struct Attr
                    {
                        ValueId value;
                        uint32_t nameLen;
                    };

                    auto buf = subspan<Attr>(memory.data(caller).subspan(ptr), len);

                    for (const auto & [n, attr] : enumerate(*value.attrs())) {
                        buf[n].value = addValue(attr.value);
                        buf[n].nameLen = state.symbols[attr.name].size();
                    }
                }));

            unwrap(linker.func_wrap(
                "env",
                "copy_attrname",
                [this](Caller caller, ValueId valueId, uint32_t attrIdx, uint32_t ptr, uint32_t len) {
                    auto & value = **values.at(valueId);
                    state.forceAttrs(value, noPos, "while copying an attr name into WASM");

                    auto & attrs = *value.attrs();

                    assert((size_t) attrIdx < attrs.size());

                    std::string_view name = state.symbols[attrs[attrIdx].name];

                    assert((size_t) len == name.size());

                    memcpy(memory.data(caller).subspan(ptr, len).data(), name.data(), name.size());
                }));

            unwrap(linker.instantiate(wasmStore, module));
        }))
        , memory(std::get<Memory>(*instance.get(wasmStore, "memory")))
    {
    }

    ValueId addValue(Value * v)
    {
        auto id = values.size();
        values.emplace_back(allocRootValue(v));
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

        auto run = std::get<Func>(*nixCtx.instance.get(nixCtx.wasmStore, nixCtx.functionName));

        debug("calling wasm module");

        v = **nixCtx.values.at(unwrap(run.call(nixCtx.wasmStore, {(int32_t) nixCtx.addValue(args[2])})).at(0).i32());
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
