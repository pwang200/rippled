#pragma once

#include <xrpl/tx/wasm/WasmVM.h>

#include <wasm.h>
#include <wasmi.h>

namespace xrpl {

template <class T, void (*Create)(T*, size_t), void (*Destroy)(T*)>
class WasmVec
{
    using TD = std::remove_pointer_t<decltype(T::data)>;
    T vec_;

public:
    WasmVec(size_t s = 0) : vec_ WASM_EMPTY_VEC
    {
        if (s > 0)
            Create(&vec_, s);  // zeroes memory
    }

    ~WasmVec()
    {
        clear();
    }

    WasmVec(WasmVec const&) = delete;
    WasmVec&
    operator=(WasmVec const&) = delete;

    WasmVec(WasmVec&& other) noexcept : vec_ WASM_EMPTY_VEC
    {
        *this = std::move(other);
    }

    WasmVec&
    operator=(WasmVec&& other) noexcept
    {
        if (this != &other)
        {
            clear();
            vec_ = other.vec_;
            other.vec_ = WASM_EMPTY_VEC;
        }
        return *this;
    }

    void
    clear()
    {
        Destroy(&vec_);  // call destructor for every elements too
        vec_ = WASM_EMPTY_VEC;
    }

    T
    release()
    {
        T result = vec_;
        vec_ = WASM_EMPTY_VEC;
        return result;
    }

    T*
    get()
    {
        return &vec_;
    }

    T const*
    get() const
    {
        return &vec_;
    }

    TD&
    operator[](size_t i)
    {
        if (i >= vec_.size)
            Throw<std::runtime_error>("Out of bound");
        return vec_.data[i];
    }

    TD const&
    operator[](size_t i) const
    {
        if (i >= vec_.size)
            Throw<std::runtime_error>("Out of bound");
        return vec_.data[i];
    }

    size_t
    size() const
    {
        return vec_.size;
    }

    bool
    empty() const
    {
        return vec_.size == 0u;
    }
};

using WasmValtypeVec =
    WasmVec<wasm_valtype_vec_t, &wasm_valtype_vec_new_uninitialized, &wasm_valtype_vec_delete>;
using WasmValVec = WasmVec<wasm_val_vec_t, &wasm_val_vec_new_uninitialized, &wasm_val_vec_delete>;
using WasmExternVec =
    WasmVec<wasm_extern_vec_t, &wasm_extern_vec_new_uninitialized, &wasm_extern_vec_delete>;
using WasmExporttypeVec = WasmVec<
    wasm_exporttype_vec_t,
    &wasm_exporttype_vec_new_uninitialized,
    &wasm_exporttype_vec_delete>;
using WasmImporttypeVec = WasmVec<
    wasm_importtype_vec_t,
    &wasm_importtype_vec_new_uninitialized,
    &wasm_importtype_vec_delete>;

struct WasmiResult
{
    WasmValVec r;
    bool f{false};  // failure flag

    WasmiResult(unsigned N = 0) : r(N)
    {
    }

    ~WasmiResult() = default;
    WasmiResult(WasmiResult&& o) = default;
    WasmiResult&
    operator=(WasmiResult&& o) = default;
};

using ModulePtr = std::unique_ptr<wasm_module_t, decltype(&wasm_module_delete)>;
using InstancePtr = std::unique_ptr<wasm_instance_t, decltype(&wasm_instance_delete)>;
using EnginePtr = std::unique_ptr<wasm_engine_t, decltype(&wasm_engine_delete)>;
using StorePtr = std::unique_ptr<wasm_store_t, decltype(&wasm_store_delete)>;

using FuncInfo = std::pair<wasm_func_t const*, wasm_functype_t const*>;

struct InstanceWrapper
{
    wasm_store_t* store_ = nullptr;
    WasmExternVec exports_;
    mutable int memIdx_ = -1;
    InstancePtr instance_;
    beast::Journal j_ = beast::Journal(beast::Journal::getNullSink());

private:
    static InstancePtr
    init(
        StorePtr& s,
        ModulePtr& m,
        WasmExternVec& expt,
        WasmExternVec const& imports,
        beast::Journal j);

public:
    InstanceWrapper();

    InstanceWrapper(InstanceWrapper&& o);

    InstanceWrapper&
    operator=(InstanceWrapper&& o);

    InstanceWrapper(StorePtr& s, ModulePtr& m, WasmExternVec const& imports, beast::Journal j);

    ~InstanceWrapper() = default;

    operator bool() const;

    FuncInfo
    getFunc(std::string_view funcName, WasmExporttypeVec const& exportTypes) const;

    wmem
    getMem() const;

    std::int64_t
    getGas() const;

    std::int64_t
    setGas(std::int64_t) const;
};

struct ModuleWrapper
{
    ModulePtr module_;
    InstanceWrapper instanceWrap_;
    WasmExporttypeVec exportTypes_;
    beast::Journal j_ = beast::Journal(beast::Journal::getNullSink());

private:
    static ModulePtr
    init(StorePtr& s, Bytes const& wasmBin, beast::Journal j);

public:
    ModuleWrapper();
    ModuleWrapper(ModuleWrapper&& o);
    ModuleWrapper&
    operator=(ModuleWrapper&& o);
    ModuleWrapper(
        StorePtr& s,
        Bytes const& wasmBin,
        bool instantiate,
        ImportVec const& imports,
        beast::Journal j);
    ~ModuleWrapper() = default;

    operator bool() const;

    FuncInfo
    getFunc(std::string_view funcName) const;

    wasm_functype_t*
    getFuncType(std::string_view funcName) const;

    wmem
    getMem() const;

    InstanceWrapper&
    getInstance(int i = 0);

    int
    addInstance(StorePtr& s, WasmExternVec const& imports);

    std::int64_t
    getGas() const;

private:
    WasmExternVec
    buildImports(StorePtr& s, ImportVec const& imports) const;
};

class WasmiEngine
{
    EnginePtr engine_;

public:
    WasmiEngine();
    ~WasmiEngine() = default;

    static EnginePtr
    init();

    // Thread-safety: multiple threads may call run() or check() concurrently
    // as long as each call owns its own HostFunctions instance. The engine
    // mutates hfs internally (via setRT), so sharing the same HostFunctions
    // across concurrent calls is not supported. The shared wasmi engine_
    // handles its own internal synchronization.
    Expected<WasmResult<int32_t>, TER>
    run(Bytes const& wasmCode,
        HostFunctions& hfs,
        int64_t gas,
        std::string_view funcName,
        std::vector<WasmParam> const& params,
        ImportVec const& imports,
        beast::Journal j);

    // See run() for thread-safety notes.
    NotTEC
    check(
        Bytes const& wasmCode,
        HostFunctions& hfs,
        std::string_view funcName,
        std::vector<WasmParam> const& params,
        ImportVec const& imports,
        beast::Journal j);

    // Host functions helper functionality
    static wasm_trap_t*
    newTrap(wasm_store_t* store, std::string const& msg);

private:
    StorePtr
    createStore(int64_t gas, beast::Journal j);

    static std::vector<wasm_val_t>
    convertParams(std::vector<WasmParam> const& params);

    static int
    compareParamTypes(wasm_valtype_vec_t const* ftp, std::vector<wasm_val_t> const& p);

    static void
    add_param(std::vector<wasm_val_t>& in, int32_t p);
    static void
    add_param(std::vector<wasm_val_t>& in, int64_t p);

    template <int NR, class... Types>
    static WasmiResult
    callFunc(FuncInfo const& f, std::vector<wasm_val_t>& in, beast::Journal j);

    template <int NR, class... Types>
    static WasmiResult
    callFunc(FuncInfo const& f, std::vector<wasm_val_t>& in, std::int32_t p, Types&&... args);

    template <int NR, class... Types>
    static WasmiResult
    callFunc(FuncInfo const& f, std::vector<wasm_val_t>& in, std::int64_t p, Types&&... args);

    template <int NR, class... Types>
    static WasmiResult
    callFunc(FuncInfo const& f, std::vector<wasm_val_t>& in, Bytes const& p, Types&&... args);
};

}  // namespace xrpl
