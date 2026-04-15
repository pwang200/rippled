#include <xrpl/basics/Log.h>
#include <xrpl/tx/wasm/WasmiVM.h>

#include <memory>

#ifdef _DEBUG
// #define DEBUG_OUTPUT 1
#endif
// #define SHOW_CALL_TIME 1

namespace xrpl {

namespace {

void
print_wasm_error(std::string_view msg, wasm_trap_t* trap, beast::Journal jlog)
{
#ifdef DEBUG_OUTPUT
    auto& j = std::cerr;
#else
    auto j = jlog.warn();
    if (jlog.active(beast::severities::kWarning))
#endif
    {
        wasm_byte_vec_t error_message WASM_EMPTY_VEC;

        if (trap != nullptr)
            wasm_trap_message(trap, &error_message);

        if (error_message.size != 0u)
        {
            j << "WASMI Error: " << msg << ", "
              << std::string_view(error_message.data, error_message.size - 1);
        }
        else
        {
            j << "WASMI Error: " << msg;
        }

        if (error_message.size != 0u)
            wasm_byte_vec_delete(&error_message);
    }

    if (trap != nullptr)
        wasm_trap_delete(trap);

#ifdef DEBUG_OUTPUT
    j << std::endl;
#endif
}
// LCOV_EXCL_STOP

}  // namespace

// Runtime wrapper for wasmi. Used in two phases:
//   1) Pre-instantiation (iw_ == nullptr): only the store exists, so host
//      callbacks fired by a WASM start section can still produce real traps
//      via the store. Memory is unavailable at this point.
//   2) Post-instantiation (iw_ != nullptr): full access to the instance's
//      memory and gas.
struct WasmiRuntimeWrapper : public WasmRuntimeWrapper
{
    wasm_store_t* store_;
    InstanceWrapper* iw_;
    beast::Journal j_;

    WasmiRuntimeWrapper(wasm_store_t* store, beast::Journal j)
        : store_(store), iw_(nullptr), j_(j)
    {
    }

    WasmiRuntimeWrapper(InstanceWrapper& iw, beast::Journal j)
        : store_(iw.store_), iw_(&iw), j_(j)
    {
    }

    virtual wmem
    getMem() override
    {
        return iw_ ? iw_->getMem() : wmem{};
    }

    virtual std::int64_t
    getGas() override
    {
        if (iw_)
            return iw_->getGas();
        std::uint64_t gas = 0;
        wasm_store_get_fuel(store_, &gas);
        return static_cast<std::int64_t>(gas);
    }

    virtual std::int64_t
    setGas(std::int64_t gas) override
    {
        if (iw_)
            return iw_->setGas(gas);
        if (gas < 0)
            gas = std::numeric_limits<decltype(gas)>::max();
        if (wasmi_error_t* err =
                wasm_store_set_fuel(store_, static_cast<std::uint64_t>(gas)))
        {
            // LCOV_EXCL_START
            wasmi_error_delete(err);
            return -1;
            // LCOV_EXCL_STOP
        }
        return gas;
    }

    virtual void*
    newTrap(std::string const& msg) override
    {
        return WasmiEngine::newTrap(store_, msg);
    }
};

InstancePtr
InstanceWrapper::init(
    StorePtr& s,
    ModulePtr& m,
    WasmExternVec& expt,
    WasmExternVec const& imports,
    beast::Journal j)
{
    wasm_trap_t* trap = nullptr;
    InstancePtr mi = InstancePtr(
        wasm_instance_new(s.get(), m.get(), imports.get(), &trap), &wasm_instance_delete);

    if (!mi || (trap != nullptr))
    {
        print_wasm_error("can't create instance", trap, j);
        throw std::runtime_error("can't create instance");
    }
    wasm_instance_exports(mi.get(), expt.get());
    return mi;
}

InstanceWrapper::InstanceWrapper() : instance_(nullptr, &wasm_instance_delete)
{
}

// LCOV_EXCL_START
InstanceWrapper::InstanceWrapper(InstanceWrapper&& o) : instance_(nullptr, &wasm_instance_delete)
{
    *this = std::move(o);
}
// LCOV_EXCL_STOP

InstanceWrapper::InstanceWrapper(
    StorePtr& s,
    ModulePtr& m,
    WasmExternVec const& imports,
    beast::Journal j)
    : store_(s.get()), instance_(init(s, m, exports_, imports, j)), j_(j)
{
}

InstanceWrapper&
InstanceWrapper::operator=(InstanceWrapper&& o)
{
    if (this == &o)
        return *this;  // LCOV_EXCL_LINE

    store_ = o.store_;
    o.store_ = nullptr;
    exports_ = std::move(o.exports_);
    memIdx_ = o.memIdx_;
    o.memIdx_ = -1;
    instance_ = std::move(o.instance_);

    j_ = o.j_;

    return *this;
}

InstanceWrapper::
operator bool() const
{
    return static_cast<bool>(instance_);
}

FuncInfo
InstanceWrapper::getFunc(std::string_view funcName, WasmExporttypeVec const& exportTypes) const
{
    wasm_func_t const* f = nullptr;
    wasm_functype_t const* ft = nullptr;

    if (!instance_)
        throw std::runtime_error("no instance");  // LCOV_EXCL_LINE

    if (exportTypes.empty())
        throw std::runtime_error("no export");  // LCOV_EXCL_LINE
    if (exportTypes.size() != exports_.size())
        throw std::runtime_error("invalid export");  // LCOV_EXCL_LINE

    for (unsigned i = 0; i < exportTypes.size(); ++i)
    {
        auto const* expType(exportTypes[i]);

        wasm_name_t const* name = wasm_exporttype_name(expType);
        wasm_externtype_t const* exnType = wasm_exporttype_type(expType);
        if (wasm_externtype_kind(exnType) == WASM_EXTERN_FUNC)
        {
            if (funcName != std::string_view(name->data, name->size))
                continue;

            auto const* exn(exports_[i]);
            if (wasm_extern_kind(exn) != WASM_EXTERN_FUNC)
                throw std::runtime_error("invalid export");  // LCOV_EXCL_LINE

            ft = wasm_externtype_as_functype_const(exnType);
            f = wasm_extern_as_func_const(exn);
            break;
        }
    }

    if ((f == nullptr) || (ft == nullptr))
        throw std::runtime_error("can't find function <" + std::string(funcName) + ">");

    return {f, ft};
}

wmem
InstanceWrapper::getMem() const
{
    if (memIdx_ >= 0)
    {
        auto* e(exports_[memIdx_]);
        wasm_memory_t* mem = wasm_extern_as_memory(e);
        return {reinterpret_cast<std::uint8_t*>(wasm_memory_data(mem)), wasm_memory_data_size(mem)};
    }

    wasm_memory_t* mem = nullptr;
    for (int i = 0; i < exports_.size(); ++i)
    {
        auto* e(exports_[i]);
        if (wasm_extern_kind(e) == WASM_EXTERN_MEMORY)
        {
            memIdx_ = i;
            mem = wasm_extern_as_memory(e);
            break;
        }
    }

    if (mem == nullptr)
        return {};  // LCOV_EXCL_LINE

    return {reinterpret_cast<std::uint8_t*>(wasm_memory_data(mem)), wasm_memory_data_size(mem)};
}

std::int64_t
InstanceWrapper::getGas() const
{
    if (store_ == nullptr)
        return -1;  // LCOV_EXCL_LINE
    std::uint64_t gas = 0;
    wasm_store_get_fuel(store_, &gas);
    return static_cast<std::int64_t>(gas);
}

std::int64_t
InstanceWrapper::setGas(std::int64_t gas) const
{
    if (store_ == nullptr)
        return -1;  // LCOV_EXCL_LINE

    if (gas < 0)
        gas = std::numeric_limits<decltype(gas)>::max();
    wasmi_error_t* err = wasm_store_set_fuel(store_, static_cast<std::uint64_t>(gas));
    if (err != nullptr)
    {
        // LCOV_EXCL_START
        print_wasm_error("Can't set instance gas", nullptr, j_);
        wasmi_error_delete(err);
        return -1;
        // LCOV_EXCL_STOP
    }

    return gas;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////

ModulePtr
ModuleWrapper::init(StorePtr& s, Bytes const& wasmBin, beast::Journal j)
{
    wasm_byte_vec_t const code{wasmBin.size(), (char*)(wasmBin.data())};
    ModulePtr m = ModulePtr(wasm_module_new(s.get(), &code), &wasm_module_delete);
    if (!m)
        throw std::runtime_error("can't create module");

    return m;
}

// LCOV_EXCL_START
ModuleWrapper::ModuleWrapper() : module_(nullptr, &wasm_module_delete)
{
}

ModuleWrapper::ModuleWrapper(ModuleWrapper&& o) : module_(nullptr, &wasm_module_delete)
{
    *this = std::move(o);
}
// LCOV_EXCL_STOP

ModuleWrapper::ModuleWrapper(
    StorePtr& s,
    Bytes const& wasmBin,
    bool instantiate,
    ImportVec const& imports,
    beast::Journal j)
    : module_(init(s, wasmBin, j)), j_(j)
{
    wasm_module_exports(module_.get(), exportTypes_.get());
    auto wimports = buildImports(s, imports);
    if (instantiate)
    {
        addInstance(s, wimports);
    }
}

// LCOV_EXCL_START
ModuleWrapper&
ModuleWrapper::operator=(ModuleWrapper&& o)
{
    if (this == &o)
        return *this;

    module_ = std::move(o.module_);
    instanceWrap_ = std::move(o.instanceWrap_);
    exportTypes_ = std::move(o.exportTypes_);
    j_ = o.j_;

    return *this;
}

ModuleWrapper::
operator bool() const
{
    return instanceWrap_;
}

// LCOV_EXCL_STOP

static WasmValtypeVec
makeImpParams(WasmImportFunc const& imp)
{
    auto const paramSize = imp.params.size();
    if (paramSize == 0u)
        return {};

    WasmValtypeVec v(paramSize);

    for (unsigned i = 0; i < paramSize; ++i)
    {
        auto const vt = imp.params[i];
        switch (vt)
        {
            case WT_I32:
                v[i] = wasm_valtype_new_i32();
                break;
            case WT_I64:
                v[i] = wasm_valtype_new_i64();
                break;
                // LCOV_EXCL_START
            default:
                throw std::runtime_error("invalid import type");
                // LCOV_EXCL_STOP
        }
    }
    return v;
}

static WasmValtypeVec
makeImpReturn(WasmImportFunc const& imp)
{
    if (!imp.result)
        return {};  // LCOV_EXCL_LINE

    WasmValtypeVec v(1);
    switch (*imp.result)
    {
        case WT_I32:
            v[0] = wasm_valtype_new_i32();
            break;
            // LCOV_EXCL_START
        case WT_I64:
            v[0] = wasm_valtype_new_i64();
            break;
        default:
            throw std::runtime_error("invalid return type");
            // LCOV_EXCL_STOP
    }
    return v;
}

WasmExternVec
ModuleWrapper::buildImports(StorePtr& s, ImportVec const& imports) const
{
    WasmImporttypeVec importTypes;
    wasm_module_imports(module_.get(), importTypes.get());

    if (importTypes.empty())
        return {};
    if (imports.empty())
        throw std::runtime_error("Missing imports");

    WasmExternVec wimports(importTypes.size());

    unsigned impCnt = 0;
    for (unsigned i = 0; i < importTypes.size(); ++i)
    {
        wasm_importtype_t const* importType = importTypes[i];

        // wasm_name_t const* mn = wasm_importtype_module(importtype);
        // auto modName = std::string_view(mn->data, mn->num_elems);
        wasm_name_t const* fn = wasm_importtype_name(importType);
        auto fieldName = std::string_view(fn->data, fn->size);

        wasm_externkind_t const itype = wasm_externtype_kind(wasm_importtype_type(importType));
        if ((itype) != WASM_EXTERN_FUNC)
        {
            throw std::runtime_error(
                "Invalid import type " + std::to_string(itype));  // LCOV_EXCL_LINE
        }

        // for multi-module support
        // if ((W_ENV != modName) && (W_HOST_LIB != modName))
        //     continue;

        bool impSet = false;
        for (auto const& obj : imports)
        {
            auto const& imp = obj.second;
            if (imp.name != fieldName)
                continue;

            WasmValtypeVec params(makeImpParams(imp));
            WasmValtypeVec results(makeImpReturn(imp));

            std::unique_ptr<wasm_functype_t, decltype(&wasm_functype_delete)> const ftype(
                wasm_functype_new(params.get(), results.get()), &wasm_functype_delete);

            params.release();
            results.release();

            wasm_func_t* func = wasm_func_new_with_env(
                s.get(),
                ftype.get(),
                reinterpret_cast<wasm_func_callback_with_env_t>(imp.wrap),
                (void*)&obj,
                nullptr);
            if (func == nullptr)
            {
                // LCOV_EXCL_START
                throw std::runtime_error("can't create import function " + imp.name);
                // LCOV_EXCL_STOP
            }

            wimports[i] = wasm_func_as_extern(func);
            ++impCnt;
            impSet = true;

            break;
        }

        if (!impSet)
        {
            print_wasm_error("Import not found: " + std::string(fieldName), nullptr, j_);
        }
    }

    if (impCnt != importTypes.size())
    {
        print_wasm_error(
            std::string("Imports not finished: ") + std::to_string(impCnt) + "/" +
                std::to_string(importTypes.size()),
            nullptr,
            j_);
        throw std::runtime_error("Missing imports");
    }

    return wimports;
}

FuncInfo
ModuleWrapper::getFunc(std::string_view funcName) const
{
    return instanceWrap_.getFunc(funcName, exportTypes_);
}

wasm_functype_t*
ModuleWrapper::getFuncType(std::string_view funcName) const
{
    for (size_t i = 0; i < exportTypes_.size(); i++)
    {
        auto const* exp_type(exportTypes_[i]);
        wasm_name_t const* name = wasm_exporttype_name(exp_type);
        wasm_externtype_t const* exn_type = wasm_exporttype_type(exp_type);
        if (wasm_externtype_kind(exn_type) == WASM_EXTERN_FUNC &&
            funcName == std::string_view(name->data, name->size))
        {
            return wasm_externtype_as_functype(const_cast<wasm_externtype_t*>(exn_type));
        }
    }

    throw std::runtime_error("can't find function <" + std::string(funcName) + ">");
}

wmem
ModuleWrapper::getMem() const
{
    return instanceWrap_.getMem();
}

InstanceWrapper&
ModuleWrapper::getInstance(int)
{
    return instanceWrap_;
}

int
ModuleWrapper::addInstance(StorePtr& s, WasmExternVec const& imports)
{
    instanceWrap_ = {s, module_, imports, j_};
    return 0;
}

// int
// my_module_t::delInstance(int i)
// {
//     if (i >= mod_inst.size())
//         return -1;
//     if (!mod_inst[i])
//         mod_inst[i] = my_mod_inst_t();
//     return i;
// }

std::int64_t
ModuleWrapper::getGas() const
{
    return instanceWrap_ ? instanceWrap_.getGas() : -1;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////

// void
// WasmiEngine::clearModules()
// {
//     modules.clear();
//     store.reset();  // to free the memory before creating new store
//     store = {wasm_store_new(engine.get()), &wasm_store_delete};
// }

std::unique_ptr<wasm_engine_t, decltype(&wasm_engine_delete)>
WasmiEngine::init()
{
    wasm_config_t* config = wasm_config_new();
    if (config == nullptr)
    {
        return std::unique_ptr<wasm_engine_t, decltype(&wasm_engine_delete)>{
            nullptr, &wasm_engine_delete};  // LCOV_EXCL_LINE
    }
    wasmi_config_consume_fuel_set(config, true);
    wasmi_config_ignore_custom_sections_set(config, true);
    wasmi_config_wasm_mutable_globals_set(config, false);
    wasmi_config_wasm_multi_value_set(config, false);
    wasmi_config_wasm_sign_extension_set(config, false);
    wasmi_config_wasm_saturating_float_to_int_set(config, false);
    wasmi_config_wasm_bulk_memory_set(config, false);
    wasmi_config_wasm_reference_types_set(config, false);
    wasmi_config_wasm_tail_call_set(config, false);
    wasmi_config_wasm_extended_const_set(config, false);
    wasmi_config_floats_set(config, false);
    wasmi_config_wasm_multi_memory_set(config, false);
    wasmi_config_wasm_custom_page_sizes_set(config, false);
    wasmi_config_wasm_memory64_set(config, false);
    wasmi_config_wasm_wide_arithmetic_set(config, false);

    return std::unique_ptr<wasm_engine_t, decltype(&wasm_engine_delete)>(
        wasm_engine_new_with_config(config), &wasm_engine_delete);
}

WasmiEngine::WasmiEngine() : engine_(init())
{
}

StorePtr
WasmiEngine::createStore(int64_t gas, beast::Journal j)
{
    StorePtr store = {
        wasm_store_new_with_memory_max_pages(engine_.get(), MAX_PAGES),
        &wasm_store_delete};

    if (gas < 0)
        gas = std::numeric_limits<decltype(gas)>::max();
    wasmi_error_t* err =
        wasm_store_set_fuel(store.get(), static_cast<std::uint64_t>(gas));
    if (err != nullptr)
    {
        // LCOV_EXCL_START
        print_wasm_error("Error setting gas", nullptr, j);
        wasmi_error_delete(err);
        throw std::runtime_error("can't set gas");
        // LCOV_EXCL_STOP
    }

    return store;
}

// int
// WasmiEngine::addInstance()
// {
//     return module->addInstance(store.get());
// }

std::vector<wasm_val_t>
WasmiEngine::convertParams(std::vector<WasmParam> const& params)
{
    std::vector<wasm_val_t> v;
    v.reserve(params.size());
    for (auto const& p : params)
    {
        switch (p.type)
        {
            case WT_I32:
                v.push_back(WASM_I32_VAL(p.of.i32));
                break;
            // LCOV_EXCL_START
            case WT_I64:
                v.push_back(WASM_I64_VAL(p.of.i64));
                break;
            default:
                throw std::runtime_error("unknown parameter type: " + std::to_string(p.type));
                break;
                // LCOV_EXCL_STOP
        }
    }

    return v;
}

int
WasmiEngine::compareParamTypes(wasm_valtype_vec_t const* ftp, std::vector<wasm_val_t> const& p)
{
    if (ftp->size != p.size())
        return std::min(ftp->size, p.size());

    for (unsigned i = 0; i < ftp->size; ++i)
    {
        auto const t1 = wasm_valtype_kind(ftp->data[i]);
        auto const t2 = p[i].kind;
        if (t1 != t2)
            return i;
    }

    return -1;
}

// LCOV_EXCL_START
void
WasmiEngine::add_param(std::vector<wasm_val_t>& in, int32_t p)
{
    in.emplace_back();
    auto& el(in.back());
    memset(&el, 0, sizeof(el));
    el = WASM_I32_VAL(p);  // WASM_I32;
}

// LCOV_EXCL_STOP

void
WasmiEngine::add_param(std::vector<wasm_val_t>& in, int64_t p)
{
    in.emplace_back();
    auto& el(in.back());
    el = WASM_I64_VAL(p);
}

#ifdef SHOW_CALL_TIME
static inline uint64_t
usecs()
{
    uint64_t x = std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::high_resolution_clock::now().time_since_epoch())
                     .count();
    return x;
}
#endif

template <int NR, class... Types>
WasmiResult
WasmiEngine::callFunc(FuncInfo const& f, std::vector<wasm_val_t>& in, beast::Journal j)
{
    WasmiResult ret(NR);
    wasm_val_vec_t const inv =
        in.empty() ? wasm_val_vec_t WASM_EMPTY_VEC : wasm_val_vec_t{in.size(), in.data()};

#ifdef SHOW_CALL_TIME
    auto const start = usecs();
#endif

    wasm_trap_t* trap = wasm_func_call(f.first, &inv, ret.r.get());

#ifdef SHOW_CALL_TIME
    auto const finish = usecs();
    auto const delta_ms = (finish - start) / 1000;
    std::cout << "wasm_func_call: " << delta_ms << "ms" << std::endl;
#endif

    if (trap)
    {
        ret.f = true;
        print_wasm_error("failure to call func", trap, j);
    }

    return ret;
}

template <int NR, class... Types>
WasmiResult
WasmiEngine::callFunc(
    FuncInfo const& f,
    std::vector<wasm_val_t>& in,
    std::int32_t p,
    Types&&... args)
{
    add_param(in, p);
    return callFunc<NR>(f, in, std::forward<Types>(args)...);
}

template <int NR, class... Types>
WasmiResult
WasmiEngine::callFunc(
    FuncInfo const& f,
    std::vector<wasm_val_t>& in,
    std::int64_t p,
    Types&&... args)
{
    add_param(in, p);
    return callFunc<NR>(f, in, std::forward<Types>(args)...);
}

template <int NR, class... Types>
WasmiResult
WasmiEngine::callFunc(
    FuncInfo const& f,
    std::vector<wasm_val_t>& in,
    Bytes const& p,
    Types&&... args)
{
    return callFunc<NR>(f, in, p.data(), p.size(), std::forward<Types>(args)...);
}

static inline void
checkImports(ImportVec const& imports, HostFunctions* hfs)
{
    for (auto const& obj : imports)
    {
        if (hfs != obj.first)
            Throw<std::runtime_error>("Imports hf unsync");
    }
}

Expected<WasmResult<int32_t>, TER>
WasmiEngine::run(
    Bytes const& wasmCode,
    HostFunctions& hfs,
    int64_t gas,
    std::string_view funcName,
    std::vector<WasmParam> const& params,
    ImportVec const& imports,
    beast::Journal j)
{
    if (gas <= 0)
        return Unexpected<TER>(temBAD_AMOUNT);

    try
    {
        checkImports(imports, &hfs);

        if (wasmCode.empty())
            throw std::runtime_error("empty module");
        if (!hfs.checkSelf())
            throw std::runtime_error("hfs isn't clean");

        // Store and module are local to this call — no shared mutable state,
        // no mutex needed.
        StorePtr store = createStore(gas, j);

        // Install a pre-instantiation runtime on hfs BEFORE instantiating, so
        // that if the WASM module has a start section and that start section
        // calls a host import, checkGas can produce a real trap using the
        // store. Without this the import callback would fire with no runtime
        // set, and there would be no way to trap correctly.
        auto clear = [](HostFunctions* p) { p->setRT(nullptr); };
        std::unique_ptr<HostFunctions, decltype(clear)> const clearGuard(
            &hfs, clear);
        WasmiRuntimeWrapper preRT(store.get(), j);
        hfs.setRT(&preRT);

        // Compile and instantiate (may execute the WASM start section).
        auto moduleWrap = std::make_unique<ModuleWrapper>(
            store, wasmCode, true, imports, j);

        if (!moduleWrap || !moduleWrap->instanceWrap_)
            throw std::runtime_error("no instance");  // LCOV_EXCL_LINE

        // Swap to the full runtime (with instance/memory access) for the
        // actual function call.
        WasmiRuntimeWrapper iw(moduleWrap->getInstance(), j);
        hfs.setRT(&iw);

        // Call main
        auto const f = moduleWrap->getFunc(!funcName.empty() ? funcName : "_start");
        auto const* ftp = wasm_functype_params(f.second);

        // not const because passed directly to VM function (which accept non
        // const)
        auto p = convertParams(params);

        if (int const comp = compareParamTypes(ftp, p); comp >= 0)
            throw std::runtime_error("invalid parameter type #" + std::to_string(comp));

        auto const res = callFunc<1>(f, p, j);

        if (res.f)
        {
            throw std::runtime_error("<" + std::string(funcName) + "> failure");
        }

        if (res.r.empty())
        {
            throw std::runtime_error(
                "<" + std::string(funcName) + "> return nothing");  // LCOV_EXCL_LINE
        }

        if (res.r[0].kind != WASM_I32)
        {
            throw std::runtime_error(
                "<" + std::string(funcName) +
                "> return type mismatch, ret: " + std::to_string(static_cast<int>(res.r[0].kind)));
        }

        if (gas == -1)
            gas = std::numeric_limits<decltype(gas)>::max();
        WasmResult<int32_t> const ret{res.r[0].of.i32, gas - moduleWrap->getGas()};

        return ret;
    }
    catch (std::exception const& e)
    {
        print_wasm_error(std::string("exception: ") + e.what(), nullptr, j);
    }
    // LCOV_EXCL_START
    catch (...)
    {
        print_wasm_error(std::string("exception: unknown"), nullptr, j);
    }
    // LCOV_EXCL_STOP
    return Unexpected<TER>(tecFAILED_PROCESSING);
}

NotTEC
WasmiEngine::check(
    Bytes const& wasmCode,
    HostFunctions& hfs,
    std::string_view funcName,
    std::vector<WasmParam> const& params,
    ImportVec const& imports,
    beast::Journal j)
{
    try
    {
        checkImports(imports, &hfs);

        if (wasmCode.empty())
            throw std::runtime_error("empty module");

        // Compile only, no instantiation — so no WASM start section runs and
        // no host callbacks fire. The store must outlive moduleWrap because
        // wasmi module/type objects reference it internally; the local binding
        // keeps it alive for the duration of this call even though we don't
        // otherwise touch it here.
        StorePtr store = createStore(-1, j);
        auto moduleWrap = std::make_unique<ModuleWrapper>(
            store, wasmCode, false, imports, j);

        if (!moduleWrap)
            throw std::runtime_error("no module");  // LCOV_EXCL_LINE

        // Looking for a func and compare parameter types
        auto const f = moduleWrap->getFuncType(!funcName.empty() ? funcName : "_start");
        auto const* ftp = wasm_functype_params(f);
        auto const p = convertParams(params);

        if (int const comp = compareParamTypes(ftp, p); comp >= 0)
            throw std::runtime_error("invalid parameter type #" + std::to_string(comp));

        return tesSUCCESS;
    }
    catch (std::exception const& e)
    {
        print_wasm_error(std::string("exception: ") + e.what(), nullptr, j);
    }
    // LCOV_EXCL_START
    catch (...)
    {
        print_wasm_error(std::string("exception: unknown"), nullptr, j);
    }
    // LCOV_EXCL_STOP

    return temBAD_WASM;
}

wasm_trap_t*
WasmiEngine::newTrap(wasm_store_t* store, std::string const& txt)
{
    static char empty[1] = {0};
    wasm_message_t msg = {1, empty};

    if (!txt.empty())
        wasm_name_new(&msg, txt.size() + 1, txt.c_str());  // include 0

    wasm_trap_t* trap = wasm_trap_new(store, &msg);  // NOLINT

    if (!txt.empty())
        wasm_byte_vec_delete(&msg);

    return trap;
}

}  // namespace xrpl
