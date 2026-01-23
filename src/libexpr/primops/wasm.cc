#include "nix/expr/primops.hh"
#include "nix/expr/eval-inline.hh"

#include <wasmtime.hh>
#include <boost/unordered/concurrent_flat_map.hpp>

using namespace wasmtime;

namespace nix {

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
    static Engine engine = []() {
        wasmtime::Config config;
        config.pooling_allocation_strategy(PoolAllocationConfig());
        config.memory_init_cow(true);
        return Engine(std::move(config));
    }();
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
    if (s.size() < len * sizeof(T))
        throw Error("Wasm memory access out of bounds");
    return std::span((T *) s.data(), len);
}

// FIXME: move to wasmtime C++ wrapper.
class InstancePre
{
    WASMTIME_OWN_WRAPPER(InstancePre, wasmtime_instance_pre);

public:
    TrapResult<Instance> instantiate(wasmtime::Store::Context cx)
    {
        wasmtime_instance_t instance;
        wasm_trap_t * trap = nullptr;
        auto * error = wasmtime_instance_pre_instantiate(ptr.get(), cx.capi(), &instance, &trap);
        if (error != nullptr) {
            return TrapError(wasmtime::Error(error));
        }
        if (trap != nullptr) {
            return TrapError(Trap(trap));
        }
        return Instance(instance);
    }
};

TrapResult<InstancePre> instantiate_pre(Linker & linker, const Module & m)
{
    wasmtime_instance_pre_t * instance_pre;
    auto * error = wasmtime_linker_instantiate_pre(linker.capi(), m.capi(), &instance_pre);
    if (error != nullptr) {
        return TrapError(wasmtime::Error(error));
    }
    return InstancePre(instance_pre);
}

void regFuns(Linker & linker);

struct NixWasmInstancePre
{
    Engine & engine;
    SourcePath wasmPath;
    InstancePre instancePre;

    NixWasmInstancePre(SourcePath _wasmPath)
        : engine(getEngine())
        , wasmPath(_wasmPath)
        , instancePre(({
            Linker linker(engine);
            regFuns(linker);
            unwrap(instantiate_pre(linker, unwrap(Module::compile(engine, string2span(wasmPath.readFile())))));
        }))
    {
    }
};

struct NixWasmInstance
{
    EvalState & state;
    ref<NixWasmInstancePre> pre;
    wasmtime::Store wasmStore;
    wasmtime::Store::Context wasmCtx;
    Instance instance;
    Memory memory_;

    ValueVector values;
    std::exception_ptr ex;

    std::optional<std::string> functionName;

    NixWasmInstance(EvalState & _state, ref<NixWasmInstancePre> _pre)
        : state(_state)
        , pre(_pre)
        , wasmStore(pre->engine)
        , wasmCtx(wasmStore)
        , instance(unwrap(pre->instancePre.instantiate(wasmCtx)))
        , memory_(getExport<Memory>("memory"))
    {
        wasmCtx.set_data(this);

        /* Reserve value ID 0 so it can be used in functions like get_attr() to denote a missing attribute. */
        values.push_back(nullptr);
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

    Value & getValue(ValueId id)
    {
        if (id >= values.size())
            throw Error("invalid ValueId %d", id);
        return *values[id];
    }

    template<typename T>
    T getExport(std::string_view name)
    {
        auto ext = instance.get(wasmCtx, name);
        if (!ext)
            throw Error("Wasm module '%s' does not export '%s'", pre->wasmPath, name);
        auto res = std::get_if<T>(&*ext);
        if (!res)
            throw Error("export '%s' of Wasm module '%s' does not have the right type", name, pre->wasmPath);
        return *res;
    }

    std::vector<Val> runFunction(std::string_view name, const std::vector<Val> & args)
    {
        functionName = name;
        return unwrap(getExport<Func>(name).call(wasmCtx, args));
    }

    auto memory()
    {
        return memory_.data(wasmCtx);
    }

    std::monostate panic(uint32_t ptr, uint32_t len)
    {
        throw Error("Wasm panic: %s", Uncolored(span2string(memory().subspan(ptr, len))));
    }

    std::monostate warn(uint32_t ptr, uint32_t len)
    {
        nix::warn(
            "'%s' function '%s': %s",
            pre->wasmPath,
            functionName.value_or("<unknown>"),
            span2string(memory().subspan(ptr, len)));
        return {};
    }

    uint32_t get_type(ValueId valueId)
    {
        auto & value = getValue(valueId);
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
    }

    ValueId make_int(int64_t n)
    {
        auto [valueId, value] = allocValue();
        value.mkInt(n);
        return valueId;
    }

    int64_t get_int(ValueId valueId)
    {
        return state.forceInt(getValue(valueId), noPos, "while evaluating a value from Wasm").value;
    }

    ValueId make_float(double x)
    {
        auto [valueId, value] = allocValue();
        value.mkFloat(x);
        return valueId;
    }

    double get_float(ValueId valueId)
    {
        return state.forceFloat(getValue(valueId), noPos, "while evaluating a value from Wasm");
    }

    ValueId make_string(uint32_t ptr, uint32_t len)
    {
        auto [valueId, value] = allocValue();
        value.mkString(span2string(memory().subspan(ptr, len)), state.mem);
        return valueId;
    }

    uint32_t copy_string(ValueId valueId, uint32_t ptr, uint32_t maxLen)
    {
        auto s = state.forceString(getValue(valueId), noPos, "while evaluating a value from Wasm");
        if (s.size() <= maxLen) {
            auto buf = memory().subspan(ptr, maxLen);
            memcpy(buf.data(), s.data(), s.size());
        }
        return s.size();
    }

    ValueId make_path(ValueId baseId, uint32_t ptr, uint32_t len)
    {
        auto & baseValue = getValue(baseId);
        state.forceValue(baseValue, noPos);
        if (baseValue.type() != nPath)
            throw Error("make_path expects a path value");
        auto base = baseValue.path();

        auto [valueId, value] = allocValue();
        value.mkPath({base.accessor, CanonPath(span2string(memory().subspan(ptr, len)), base.path)}, state.mem);
        return valueId;
    }

    uint32_t copy_path(ValueId valueId, uint32_t ptr, uint32_t maxLen)
    {
        auto & v = getValue(valueId);
        state.forceValue(v, noPos);
        if (v.type() != nPath)
            throw Error("copy_path expects a path value");
        auto path = v.path().path;
        auto s = path.abs();
        if (s.size() <= maxLen) {
            auto buf = memory().subspan(ptr, maxLen);
            memcpy(buf.data(), s.data(), s.size());
        }
        return s.size();
    }

    ValueId make_bool(int32_t b)
    {
        return addValue(state.getBool(b));
    }

    int32_t get_bool(ValueId valueId)
    {
        return state.forceBool(getValue(valueId), noPos, "while evaluating a value from Wasm");
    }

    ValueId make_null()
    {
        return addValue(&Value::vNull);
    }

    ValueId make_list(uint32_t ptr, uint32_t len)
    {
        auto vs = subspan<ValueId>(memory().subspan(ptr), len);

        auto [valueId, value] = allocValue();

        auto list = state.buildList(len);
        for (const auto & [n, v] : enumerate(list))
            v = &getValue(vs[n]); // FIXME: endianness
        value.mkList(list);

        return valueId;
    }

    uint32_t copy_list(ValueId valueId, uint32_t ptr, uint32_t maxLen)
    {
        auto & value = getValue(valueId);
        state.forceList(value, noPos, "while getting a list from Wasm");

        if (value.listSize() <= maxLen) {
            auto out = subspan<ValueId>(memory().subspan(ptr), value.listSize());

            for (const auto & [n, elem] : enumerate(value.listView()))
                out[n] = addValue(elem);
        }

        return value.listSize();
    }

    ValueId make_attrset(uint32_t ptr, uint32_t len)
    {
        auto mem = memory();

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
                &getValue(attr.value));
        value.mkAttrs(builder);

        return valueId;
    }

    uint32_t copy_attrset(ValueId valueId, uint32_t ptr, uint32_t maxLen)
    {
        auto & value = getValue(valueId);
        state.forceAttrs(value, noPos, "while copying an attrset into Wasm");

        if (value.attrs()->size() <= maxLen) {
            // FIXME: endianness.
            struct Attr
            {
                ValueId value;
                uint32_t nameLen;
            };

            auto buf = subspan<Attr>(memory().subspan(ptr), maxLen);

            // FIXME: for determinism, we should return attributes in lexicographically sorted order.
            for (const auto & [n, attr] : enumerate(*value.attrs())) {
                buf[n].value = addValue(attr.value);
                buf[n].nameLen = state.symbols[attr.name].size();
            }
        }

        return value.attrs()->size();
    }

    std::monostate copy_attrname(ValueId valueId, uint32_t attrIdx, uint32_t ptr, uint32_t len)
    {
        auto & value = getValue(valueId);
        state.forceAttrs(value, noPos, "while copying an attr name into Wasm");

        auto & attrs = *value.attrs();

        if ((size_t) attrIdx >= attrs.size())
            throw Error("copy_attrname: attribute index out of bounds");

        std::string_view name = state.symbols[attrs[attrIdx].name];

        if ((size_t) len != name.size())
            throw Error("copy_attrname: buffer length does not match attribute name length");

        memcpy(memory().subspan(ptr, len).data(), name.data(), name.size());

        return {};
    }

    ValueId get_attr(ValueId valueId, uint32_t ptr, uint32_t len)
    {
        auto attrName = span2string(memory().subspan(ptr, len));

        auto & value = getValue(valueId);
        state.forceAttrs(value, noPos, "while getting an attribute from Wasm");

        auto attr = value.attrs()->get(state.symbols.create(attrName));

        return attr ? addValue(attr->value) : 0;
    }

    ValueId call_function(ValueId funId, uint32_t ptr, uint32_t len)
    {
        auto & fun = getValue(funId);
        state.forceFunction(fun, noPos, "while calling a function from Wasm");

        ValueVector args;
        for (auto argId : subspan<ValueId>(memory().subspan(ptr), len))
            args.push_back(&getValue(argId));

        auto [valueId, value] = allocValue();

        state.callFunction(fun, args, value, noPos);

        return valueId;
    }

    ValueId make_app(ValueId funId, uint32_t ptr, uint32_t len)
    {
        if (!len)
            return funId;

        auto args = subspan<ValueId>(memory().subspan(ptr), len);

        auto res = &getValue(funId);

        while (!args.empty()) {
            auto arg = &getValue(args[0]);
            auto tmp = state.allocValue();
            tmp->mkApp(res, {arg});
            res = tmp;
            args = args.subspan(1);
        }

        return addValue(res);
    }

    /**
     * Read the contents of a file into Wasm memory. This is like calling `builtins.readFile`, except that it can handle
     * binary files that cannot be represented as Nix strings.
     */
    uint32_t read_file(ValueId pathId, uint32_t ptr, uint32_t len)
    {
        auto & pathValue = getValue(pathId);
        state.forceValue(pathValue, noPos);
        if (pathValue.type() != nPath)
            throw Error("read_file expects a path value");

        auto path = pathValue.path();
        auto contents = path.readFile();

        if (contents.size() > std::numeric_limits<uint32_t>::max())
            throw Error("file '%s' is too large to process in Wasm (size: %d)", path, contents.size());

        // FIXME: this is an inefficient interface since it may cause the file to be read twice.
        if (contents.size() <= len) {
            auto buf = memory().subspan(ptr, len);
            memcpy(buf.data(), contents.data(), contents.size());
        }

        return contents.size();
    }
};

template<typename R, typename... Args>
static void regFun(Linker & linker, std::string_view name, R (NixWasmInstance::*f)(Args...))
{
    unwrap(linker.func_wrap("env", name, [f](Caller caller, Args... args) -> Result<R, Trap> {
        try {
            auto instance = std::any_cast<NixWasmInstance *>(caller.context().get_data());
            return (*instance.*f)(args...);
        } catch (std::exception & e) {
            return Trap(e.what());
        } catch (...) {
            return Trap("unknown exception");
        }
    }));
}

void regFuns(Linker & linker)
{
    regFun(linker, "panic", &NixWasmInstance::panic);
    regFun(linker, "warn", &NixWasmInstance::warn);
    regFun(linker, "get_type", &NixWasmInstance::get_type);
    regFun(linker, "make_int", &NixWasmInstance::make_int);
    regFun(linker, "get_int", &NixWasmInstance::get_int);
    regFun(linker, "make_float", &NixWasmInstance::make_float);
    regFun(linker, "get_float", &NixWasmInstance::get_float);
    regFun(linker, "make_string", &NixWasmInstance::make_string);
    regFun(linker, "copy_string", &NixWasmInstance::copy_string);
    regFun(linker, "make_path", &NixWasmInstance::make_path);
    regFun(linker, "copy_path", &NixWasmInstance::copy_path);
    regFun(linker, "make_bool", &NixWasmInstance::make_bool);
    regFun(linker, "get_bool", &NixWasmInstance::get_bool);
    regFun(linker, "make_null", &NixWasmInstance::make_null);
    regFun(linker, "make_list", &NixWasmInstance::make_list);
    regFun(linker, "copy_list", &NixWasmInstance::copy_list);
    regFun(linker, "make_attrset", &NixWasmInstance::make_attrset);
    regFun(linker, "copy_attrset", &NixWasmInstance::copy_attrset);
    regFun(linker, "copy_attrname", &NixWasmInstance::copy_attrname);
    regFun(linker, "get_attr", &NixWasmInstance::get_attr);
    regFun(linker, "call_function", &NixWasmInstance::call_function);
    regFun(linker, "make_app", &NixWasmInstance::make_app);
    regFun(linker, "read_file", &NixWasmInstance::read_file);
}

void prim_wasm(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto wasmPath = state.realisePath(pos, *args[0]);
    std::string functionName =
        std::string(state.forceStringNoCtx(*args[1], pos, "while evaluating the second argument of `builtins.wasm`"));

    try {
        // FIXME: make this a weak Boehm GC pointer so that it can be freed during GC.
        // FIXME: move to EvalState?
        // Note: InstancePre in Rust is Send+Sync so it should be safe to share between threads.
        static boost::concurrent_flat_map<SourcePath, std::shared_ptr<NixWasmInstancePre>> instancesPre;

        std::shared_ptr<NixWasmInstancePre> instancePre;

        instancesPre.try_emplace_and_cvisit(
            wasmPath,
            nullptr,
            [&](auto & i) { instancePre = i.second = std::make_shared<NixWasmInstancePre>(wasmPath); },
            [&](auto & i) { instancePre = i.second; });

        debug("calling wasm module");

        NixWasmInstance instance{state, ref(instancePre)};

        // FIXME: use the "start" function if present.
        instance.runFunction("nix_wasm_init_v1", {});

        auto res = instance.runFunction(functionName, {(int32_t) instance.addValue(args[2])});
        if (res.size() != 1)
            throw Error("Wasm function '%s' from '%s' did not return exactly one value", functionName, wasmPath);
        if (res[0].kind() != ValKind::I32)
            throw Error("Wasm function '%s' from '%s' did not return an i32 value", functionName, wasmPath);
        auto & vRes = instance.getValue(res[0].i32());
        state.forceValue(vRes, pos);
        v = vRes;
    } catch (Error & e) {
        e.addTrace(state.positions[pos], "while executing the Wasm function '%s' from '%s'", functionName, wasmPath);
        throw;
    }
}

static RegisterPrimOp primop_wasm(
    {.name = "wasm",
     .args = {"wasm", "entry", "arg"},
     .doc = R"(
      Call a Wasm function with the specified argument.
     )",
     .fun = prim_wasm,
     .experimentalFeature = Xp::WasmBuiltin});

} // namespace nix
