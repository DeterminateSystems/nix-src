#include "nix/expr/primops.hh"
#include "nix/expr/eval-inline.hh"

#include <wasmedge/wasmedge.h>

namespace nix {

// FIXME
SourcePath realisePath(
    EvalState & state,
    const PosIdx pos,
    Value & v,
    std::optional<SymlinkResolution> resolveSymlinks = SymlinkResolution::Full);

// FIXME
char * allocString(size_t size);

using ValueId = uint32_t;

struct NixWasmContext
{
    EvalState & state;
    SourcePath wasmPath;
    std::string functionName;
    std::vector<RootValue> values;
    std::exception_ptr ex;

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

enum struct WasmUserError : uint16_t {
    Exception = 1,
};

struct FunCtx
{
    NixWasmContext & nixCtx;
    std::string funName;
    void (*fun)(
        NixWasmContext & nixCtx,
        const WasmEdge_CallingFrameContext * callFrameCtx,
        const WasmEdge_Value * in,
        WasmEdge_Value * out);
};

static WasmEdge_Result wrapHostFun(
    void * _funCtx, const WasmEdge_CallingFrameContext * callFrameCtx, const WasmEdge_Value * in, WasmEdge_Value * out)
{
    auto & funCtx(*(FunCtx *) _funCtx);
    try {
        funCtx.fun(funCtx.nixCtx, callFrameCtx, in, out);
        return WasmEdge_Result_Success;
    } catch (Error & e) {
        e.addTrace(funCtx.nixCtx.state.positions[noPos], "while calling the WASM host function '%s'", funCtx.funName);
        funCtx.nixCtx.ex = std::current_exception();
        return WasmEdge_ResultGen(WasmEdge_ErrCategory_UserLevelError, (uint32_t) WasmUserError::Exception);
    } catch (...) {
        funCtx.nixCtx.ex = std::current_exception();
        return WasmEdge_ResultGen(WasmEdge_ErrCategory_UserLevelError, (uint32_t) WasmUserError::Exception);
    }
}

static void wasm_warn(
    NixWasmContext & nixCtx,
    const WasmEdge_CallingFrameContext * callFrameCtx,
    const WasmEdge_Value * in,
    WasmEdge_Value * out)
{
    int32_t ptr = WasmEdge_ValueGetI32(in[0]);
    int32_t len = WasmEdge_ValueGetI32(in[1]);

    auto memCtx = WasmEdge_CallingFrameGetMemoryInstance(callFrameCtx, 0);

    std::vector<uint8_t> buf;
    buf.resize(len);
    WasmEdge_Result res = WasmEdge_MemoryInstanceGetData(memCtx, buf.data(), ptr, len);
    if (!WasmEdge_ResultOK(res))
        throw Error("unable to copy bytes from WASM memory: %s", WasmEdge_ResultGetMessage(res));

    warn(
        "'%s' function '%s': %s",
        nixCtx.wasmPath,
        nixCtx.functionName,
        std::string_view((char *) buf.data(), buf.size()));
}

static void wasm_get_type(
    NixWasmContext & nixCtx,
    const WasmEdge_CallingFrameContext * callFrameCtx,
    const WasmEdge_Value * in,
    WasmEdge_Value * out)
{
    ValueId valueId = WasmEdge_ValueGetI32(in[0]);
    auto & value = **nixCtx.values.at(valueId);
    nixCtx.state.forceValue(value, noPos);
    auto t = value.type();
    out[0] = WasmEdge_ValueGenI64(
        t == nInt        ? 1
        : t == nFloat    ? 2
        : t == nBool     ? 3
        : t == nString   ? 4
        : t == nPath     ? 5
        : t == nNull     ? 6
        : t == nAttrs    ? 7
        : t == nList     ? 8
        : t == nFunction ? 9
                         : []() -> int { throw Error("unsupported type"); }());
}

static void wasm_make_int(
    NixWasmContext & nixCtx,
    const WasmEdge_CallingFrameContext * callFrameCtx,
    const WasmEdge_Value * in,
    WasmEdge_Value * out)
{
    auto n = WasmEdge_ValueGetI64(in[0]);
    auto [valueId, value] = nixCtx.allocValue();
    value.mkInt(n);
    out[0] = WasmEdge_ValueGenI32(valueId);
}

static void wasm_get_int(
    NixWasmContext & nixCtx,
    const WasmEdge_CallingFrameContext * callFrameCtx,
    const WasmEdge_Value * in,
    WasmEdge_Value * out)
{
    ValueId valueId = WasmEdge_ValueGetI32(in[0]);
    auto & value = **nixCtx.values.at(valueId);
    auto n = nixCtx.state.forceInt(value, noPos, "while evaluating a value from WASM");
    out[0] = WasmEdge_ValueGenI64(n.value);
}

static void wasm_make_float(
    NixWasmContext & nixCtx,
    const WasmEdge_CallingFrameContext * callFrameCtx,
    const WasmEdge_Value * in,
    WasmEdge_Value * out)
{
    auto n = WasmEdge_ValueGetF64(in[0]);
    auto [valueId, value] = nixCtx.allocValue();
    value.mkFloat(n);
    out[0] = WasmEdge_ValueGenI32(valueId);
}

static void wasm_get_float(
    NixWasmContext & nixCtx,
    const WasmEdge_CallingFrameContext * callFrameCtx,
    const WasmEdge_Value * in,
    WasmEdge_Value * out)
{
    ValueId valueId = WasmEdge_ValueGetI32(in[0]);
    auto & value = **nixCtx.values.at(valueId);
    auto n = nixCtx.state.forceFloat(value, noPos, "while evaluating a value from WASM");
    out[0] = WasmEdge_ValueGenF64(n);
}

static void wasm_make_string(
    NixWasmContext & nixCtx,
    const WasmEdge_CallingFrameContext * callFrameCtx,
    const WasmEdge_Value * in,
    WasmEdge_Value * out)
{
    int32_t ptr = WasmEdge_ValueGetI32(in[0]);
    int32_t len = WasmEdge_ValueGetI32(in[1]);

    auto memCtx = WasmEdge_CallingFrameGetMemoryInstance(callFrameCtx, 0);

    auto s = allocString(len + 1);
    s[len] = 0;

    WasmEdge_Result res = WasmEdge_MemoryInstanceGetData(memCtx, (uint8_t *) s, ptr, len);
    if (!WasmEdge_ResultOK(res))
        throw Error("unable to copy bytes from WASM memory: %s", WasmEdge_ResultGetMessage(res));

    auto [valueId, value] = nixCtx.allocValue();
    value.mkString(s);

    out[0] = WasmEdge_ValueGenI32(valueId);
}

static void wasm_get_string_length(
    NixWasmContext & nixCtx,
    const WasmEdge_CallingFrameContext * callFrameCtx,
    const WasmEdge_Value * in,
    WasmEdge_Value * out)
{
    ValueId valueId = WasmEdge_ValueGetI32(in[0]);
    auto & value = nixCtx.values.at(valueId);
    auto s = nixCtx.state.forceString(**value, noPos, "while getting a string length from WASM");
    out[0] = WasmEdge_ValueGenI64(s.size());
}

static void wasm_copy_string(
    NixWasmContext & nixCtx,
    const WasmEdge_CallingFrameContext * callFrameCtx,
    const WasmEdge_Value * in,
    WasmEdge_Value * out)
{
    ValueId valueId = WasmEdge_ValueGetI32(in[0]);
    int32_t ptr = WasmEdge_ValueGetI32(in[1]);
    int32_t len = WasmEdge_ValueGetI32(in[2]);

    auto & value = **nixCtx.values.at(valueId);

    auto s = nixCtx.state.forceString(value, noPos, "while evaluating a value from WASM");

    assert((size_t) len == s.size());

    auto memCtx = WasmEdge_CallingFrameGetMemoryInstance(callFrameCtx, 0);

    auto res = WasmEdge_MemoryInstanceSetData(memCtx, (const uint8_t *) s.data(), ptr, len);
    if (!WasmEdge_ResultOK(res))
        throw Error("unable to copy bytes to WASM memory: %s", WasmEdge_ResultGetMessage(res));
}

static void wasm_make_bool(
    NixWasmContext & nixCtx,
    const WasmEdge_CallingFrameContext * callFrameCtx,
    const WasmEdge_Value * in,
    WasmEdge_Value * out)
{
    bool b = WasmEdge_ValueGetI32(in[0]);
    out[0] = WasmEdge_ValueGenI32(nixCtx.addValue(nixCtx.state.getBool(b)));
}

static void wasm_get_bool(
    NixWasmContext & nixCtx,
    const WasmEdge_CallingFrameContext * callFrameCtx,
    const WasmEdge_Value * in,
    WasmEdge_Value * out)
{
    ValueId valueId = WasmEdge_ValueGetI32(in[0]);
    auto & value = **nixCtx.values.at(valueId);
    out[0] = WasmEdge_ValueGenI32(nixCtx.state.forceBool(value, noPos, "while evaluating a value from WASM") ? 1 : 0);
}

static void wasm_make_null(
    NixWasmContext & nixCtx,
    const WasmEdge_CallingFrameContext * callFrameCtx,
    const WasmEdge_Value * in,
    WasmEdge_Value * out)
{
    out[0] = WasmEdge_ValueGenI32(nixCtx.addValue(&nixCtx.state.vNull));
}

static void wasm_make_list(
    NixWasmContext & nixCtx,
    const WasmEdge_CallingFrameContext * callFrameCtx,
    const WasmEdge_Value * in,
    WasmEdge_Value * out)
{
    int32_t ptr = WasmEdge_ValueGetI32(in[0]);
    int32_t len = WasmEdge_ValueGetI32(in[1]);

    std::vector<ValueId> vs(len);

    auto memCtx = WasmEdge_CallingFrameGetMemoryInstance(callFrameCtx, 0);

    if (len) {
        WasmEdge_Result res = WasmEdge_MemoryInstanceGetData(memCtx, (uint8_t *) vs.data(), ptr, len * sizeof(ValueId));
        if (!WasmEdge_ResultOK(res))
            throw Error("unable to copy bytes from WASM memory: %s", WasmEdge_ResultGetMessage(res));
    }

    auto [valueId, value] = nixCtx.allocValue();
    auto list = nixCtx.state.buildList(len);
    for (const auto & [n, v] : enumerate(list))
        v = *nixCtx.values.at(vs[n]);
    value.mkList(list);

    out[0] = WasmEdge_ValueGenI32(valueId);
}

static void wasm_get_list_length(
    NixWasmContext & nixCtx,
    const WasmEdge_CallingFrameContext * callFrameCtx,
    const WasmEdge_Value * in,
    WasmEdge_Value * out)
{
    ValueId valueId = WasmEdge_ValueGetI32(in[0]);
    auto & value = **nixCtx.values.at(valueId);
    nixCtx.state.forceList(value, noPos, "while getting a list length from WASM");
    out[0] = WasmEdge_ValueGenI64(value.listSize());
}

static void wasm_copy_list(
    NixWasmContext & nixCtx,
    const WasmEdge_CallingFrameContext * callFrameCtx,
    const WasmEdge_Value * in,
    WasmEdge_Value * out)
{
    ValueId valueId = WasmEdge_ValueGetI32(in[0]);
    int32_t ptr = WasmEdge_ValueGetI32(in[1]);
    int32_t len = WasmEdge_ValueGetI32(in[2]);

    if (!len) return;

    auto & value = **nixCtx.values.at(valueId);
    nixCtx.state.forceList(value, noPos, "while copying a list into WASM");

    assert((size_t) len == value.listSize());

    std::vector<ValueId> vs;
    for (auto elem : value.listView())
        // FIXME: endianness.
        vs.push_back(nixCtx.addValue(elem));

    auto memCtx = WasmEdge_CallingFrameGetMemoryInstance(callFrameCtx, 0);

    auto res = WasmEdge_MemoryInstanceSetData(memCtx, (const uint8_t *) vs.data(), ptr, vs.size() * sizeof(ValueId));
    if (!WasmEdge_ResultOK(res))
        throw Error("unable to copy bytes to WASM memory: %s", WasmEdge_ResultGetMessage(res));
}

static void wasm_make_attrset(
    NixWasmContext & nixCtx,
    const WasmEdge_CallingFrameContext * callFrameCtx,
    const WasmEdge_Value * in,
    WasmEdge_Value * out)
{
    int32_t ptr = WasmEdge_ValueGetI32(in[0]);
    int32_t len = WasmEdge_ValueGetI32(in[1]);

    auto memCtx = WasmEdge_CallingFrameGetMemoryInstance(callFrameCtx, 0);

    struct Attr
    {
        uint32_t attrNamePtr;
        uint32_t attrNameLen;
        ValueId value;
    };

    std::vector<Attr> attrs(len);

    if (len) {
        WasmEdge_Result res = WasmEdge_MemoryInstanceGetData(memCtx, (uint8_t *) attrs.data(), ptr, len * sizeof(Attr));
        if (!WasmEdge_ResultOK(res))
            throw Error("unable to copy bytes from WASM memory: %s", WasmEdge_ResultGetMessage(res));
    }

    auto [valueId, value] = nixCtx.allocValue();
    auto builder = nixCtx.state.buildBindings(len);
    for (auto & attr : attrs) {
        std::string name;
        name.resize(attr.attrNameLen, 0);
        WasmEdge_Result res =
            WasmEdge_MemoryInstanceGetData(memCtx, (uint8_t *) name.data(), attr.attrNamePtr, attr.attrNameLen);
        if (!WasmEdge_ResultOK(res))
            throw Error("unable to copy bytes from WASM memory: %s", WasmEdge_ResultGetMessage(res));
        builder.insert(nixCtx.state.symbols.create(name), *nixCtx.values.at(attr.value));
    }
    value.mkAttrs(builder);

    out[0] = WasmEdge_ValueGenI32(valueId);
}

static void wasm_get_attrset_length(
    NixWasmContext & nixCtx,
    const WasmEdge_CallingFrameContext * callFrameCtx,
    const WasmEdge_Value * in,
    WasmEdge_Value * out)
{
    ValueId valueId = WasmEdge_ValueGetI32(in[0]);
    auto & value = **nixCtx.values.at(valueId);
    nixCtx.state.forceAttrs(value, noPos, "while getting an attrset length from WASM");
    out[0] = WasmEdge_ValueGenI64(value.attrs()->size());
}

static void wasm_copy_attrset(
    NixWasmContext & nixCtx,
    const WasmEdge_CallingFrameContext * callFrameCtx,
    const WasmEdge_Value * in,
    WasmEdge_Value * out)
{
    ValueId valueId = WasmEdge_ValueGetI32(in[0]);
    int32_t ptr = WasmEdge_ValueGetI32(in[1]);
    int32_t len = WasmEdge_ValueGetI32(in[2]);

    auto & value = **nixCtx.values.at(valueId);
    nixCtx.state.forceAttrs(value, noPos, "while copying an attrset into WASM");

    assert((size_t) len == value.attrs()->size());

    // FIXME: endianness.
    struct Attr
    {
        ValueId value;
        uint32_t nameLen;
    };

    std::vector<Attr> buf;
    for (auto & attr : *value.attrs())
        buf.push_back(
            {.value = nixCtx.addValue(attr.value), .nameLen = (uint32_t) nixCtx.state.symbols[attr.name].size()});

    auto memCtx = WasmEdge_CallingFrameGetMemoryInstance(callFrameCtx, 0);

    auto res = WasmEdge_MemoryInstanceSetData(memCtx, (const uint8_t *) buf.data(), ptr, len * sizeof(Attr));
    if (!WasmEdge_ResultOK(res))
        throw Error("unable to copy bytes to WASM memory: %s", WasmEdge_ResultGetMessage(res));
}

static void wasm_copy_attrname(
    NixWasmContext & nixCtx,
    const WasmEdge_CallingFrameContext * callFrameCtx,
    const WasmEdge_Value * in,
    WasmEdge_Value * out)
{
    ValueId valueId = WasmEdge_ValueGetI32(in[0]);
    ValueId attrIdx = WasmEdge_ValueGetI32(in[1]);
    int32_t ptr = WasmEdge_ValueGetI32(in[2]);
    int32_t len = WasmEdge_ValueGetI32(in[3]);

    auto & value = **nixCtx.values.at(valueId);
    nixCtx.state.forceAttrs(value, noPos, "while copying an attr name into WASM");

    auto & attrs = *value.attrs();

    assert((size_t) attrIdx < attrs.size());

    std::string_view name = nixCtx.state.symbols[attrs[attrIdx].name];

    assert((size_t) len == name.size());

    auto memCtx = WasmEdge_CallingFrameGetMemoryInstance(callFrameCtx, 0);

    auto res = WasmEdge_MemoryInstanceSetData(memCtx, (const uint8_t *) name.data(), ptr, len);
    if (!WasmEdge_ResultOK(res))
        throw Error("unable to copy bytes to WASM memory: %s", WasmEdge_ResultGetMessage(res));
}

void prim_wasm(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    NixWasmContext nixCtx{
        .state = state,
        .wasmPath = realisePath(state, pos, *args[0]),
        .functionName = std::string(
            state.forceStringNoCtx(*args[1], pos, "while evaluating the second argument of `builtins.wasm`")),
    };
    auto wasmFile = nixCtx.wasmPath.readFile();

    WasmEdge_ConfigureContext * confCtx = WasmEdge_ConfigureCreate();
    Finally d0([&]() { WasmEdge_ConfigureDelete(confCtx); });

    // Disable native (AOT) execution since it's a huge security hole.
    WasmEdge_ConfigureSetForceInterpreter(confCtx, true);

    // Create the VM.
    WasmEdge_VMContext * vmCtx = WasmEdge_VMCreate(confCtx, nullptr);
    Finally d1([&]() { WasmEdge_VMDelete(vmCtx); });

    // Register host functions.
    auto exportName = WasmEdge_StringCreateByCString("env");
    auto modCtx = WasmEdge_ModuleInstanceCreate(exportName);

    auto regFun = [&](const char * name,
                      auto fun,
                      const std::vector<WasmEdge_ValType> & params,
                      const std::vector<WasmEdge_ValType> & returns) {
        auto hostFunType = WasmEdge_FunctionTypeCreate(params.data(), params.size(), returns.data(), returns.size());
        Finally hostFunTypeDel([&]() { WasmEdge_FunctionTypeDelete(hostFunType); });
        auto funCtx = new FunCtx(nixCtx, name, fun);
        auto hostFun = WasmEdge_FunctionInstanceCreate(hostFunType, wrapHostFun, funCtx, 0);
        auto hostFunName = WasmEdge_StringCreateByCString(name);
        Finally hostFunNameDel([&]() { WasmEdge_StringDelete(hostFunName); });
        WasmEdge_ModuleInstanceAddFunction(modCtx, hostFunName, hostFun);
    };

    auto tNixValue = WasmEdge_ValTypeGenI32();
    auto tPtr = WasmEdge_ValTypeGenI32();

    regFun("warn", wasm_warn, {tPtr, WasmEdge_ValTypeGenI32()}, {});
    regFun("get_type", wasm_get_type, {tNixValue}, {WasmEdge_ValTypeGenI32()});
    regFun("make_int", wasm_make_int, {WasmEdge_ValTypeGenI64()}, {tNixValue});
    regFun("get_int", wasm_get_int, {tNixValue}, {WasmEdge_ValTypeGenI64()});
    regFun("make_float", wasm_make_float, {WasmEdge_ValTypeGenF64()}, {tNixValue});
    regFun("get_float", wasm_get_float, {tNixValue}, {WasmEdge_ValTypeGenF64()});
    regFun("make_string", wasm_make_string, {tPtr, WasmEdge_ValTypeGenI32()}, {tNixValue});
    regFun("get_string_length", wasm_get_string_length, {tNixValue}, {WasmEdge_ValTypeGenI32()});
    regFun("copy_string", wasm_copy_string, {tNixValue, tPtr, WasmEdge_ValTypeGenI32()}, {});
    regFun("make_bool", wasm_make_bool, {WasmEdge_ValTypeGenI32()}, {tNixValue});
    regFun("get_bool", wasm_get_bool, {tNixValue}, {WasmEdge_ValTypeGenI32()});
    regFun("make_null", wasm_make_null, {}, {tNixValue});
    regFun("make_list", wasm_make_list, {tPtr, WasmEdge_ValTypeGenI32()}, {tNixValue});
    regFun("get_list_length", wasm_get_list_length, {tNixValue}, {WasmEdge_ValTypeGenI32()});
    regFun("copy_list", wasm_copy_list, {tNixValue, tPtr, WasmEdge_ValTypeGenI32()}, {});
    regFun("make_attrset", wasm_make_attrset, {tPtr, WasmEdge_ValTypeGenI32()}, {tNixValue});
    regFun("get_attrset_length", wasm_get_attrset_length, {tNixValue}, {WasmEdge_ValTypeGenI32()});
    regFun("copy_attrset", wasm_copy_attrset, {tNixValue, tPtr, WasmEdge_ValTypeGenI32()}, {});
    regFun(
        "copy_attrname", wasm_copy_attrname, {tNixValue, WasmEdge_ValTypeGenI32(), tPtr, WasmEdge_ValTypeGenI32()}, {});

    WasmEdge_VMRegisterModuleFromImport(vmCtx, modCtx);

    try {
        // Call the WASM function.
        auto argId = nixCtx.addValue(args[2]);
        WasmEdge_Value paramList[1] = {WasmEdge_ValueGenI32(argId)};
        WasmEdge_Value returnList[1];
        WasmEdge_String funcName = WasmEdge_StringCreateByCString(nixCtx.functionName.c_str());

        // FIXME: redirect wasmedge log messages
        debug("calling wasm module");
        WasmEdge_Result res = WasmEdge_VMRunWasmFromBuffer(
            vmCtx, (const uint8_t *) wasmFile.data(), wasmFile.size(), funcName, paramList, 1, returnList, 1);

        if (WasmEdge_ResultOK(res))
            v = **nixCtx.values.at(WasmEdge_ValueGetI32(returnList[0]));
        else if (
            WasmEdge_ResultGetCategory(res) == WasmEdge_ErrCategory_UserLevelError
            && WasmEdge_ResultGetCode(res) == (uint32_t) WasmUserError::Exception)
            std::rethrow_exception(nixCtx.ex);
        else
            throw Error("WASM execution error: %s", WasmEdge_ResultGetMessage(res));
    } catch (Error & e) {
        e.addTrace(
            state.positions[pos],
            "while executing the WASM function '%s' from '%s'",
            nixCtx.functionName,
            nixCtx.wasmPath);
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
