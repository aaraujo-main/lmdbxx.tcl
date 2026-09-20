#include "lmdbxx.hpp"

#include <tclxx.hpp>

#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace lmdbxx {

namespace {

// mdb_env_open never creates directories itself: without MDB_NOSUBDIR the
// path names a directory that must already exist (it will hold data.mdb /
// lock.mdb); with MDB_NOSUBDIR the path names a file whose parent directory
// must already exist. Create whichever is missing so open_env_shared can be
// used against a fresh scratch path.
void EnsureEnvPathExists(const std::string& path, unsigned int flags) {
    std::error_code ec;
    if (flags & MDB_NOSUBDIR) {
        auto parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
        }
    } else {
        std::filesystem::create_directories(path, ec);
    }
}

} // namespace

env::env(std::string path, unsigned int flags, mdb_mode_t mode)
    : path_(std::move(path)), flags_(flags), mode_(mode), handle_(lmdb::env::create()) {
    EnsureEnvPathExists(path_, flags_);
    handle_.open(path_.c_str(), flags_, mode_);
}

env::env(const env& other)
    : path_(other.path_), flags_(other.flags_), mode_(other.mode_), handle_(lmdb::env::create()) {
}

env::env(env&& other) noexcept
    : path_(std::move(other.path_)),
      flags_(other.flags_),
      mode_(other.mode_),
      handle_(std::move(other.handle_)) {
}

void env::set_mapsize(std::size_t size) {
    if (txn_started_.load(std::memory_order_relaxed)) {
        throw std::runtime_error(
            "lmdbxx: env_set_mapsize: cannot resize map after a transaction has already "
            "started on this environment");
    }
    handle_.set_mapsize(size);
}

std::shared_ptr<dbi> env::open_default_dbi() {
    std::lock_guard<std::mutex> lock(dbi_mutex_);
    if (auto cached = default_dbi_.lock()) {
        return cached;
    }
    lmdb::txn txn = lmdb::txn::begin(handle_, nullptr, 0);
    lmdb::dbi rawDbi = lmdb::dbi::open(txn, nullptr, MDB_CREATE);
    txn.commit();
    txn_started_.store(true, std::memory_order_relaxed);
    auto created = std::make_shared<dbi>(shared_from_this(), rawDbi);
    default_dbi_ = created;
    return created;
}

namespace {

struct RegistryEntry {
    std::weak_ptr<env> handle;
    unsigned int flags;
    mdb_mode_t mode;
};

std::mutex g_registryMutex;
std::unordered_map<std::string, RegistryEntry> g_registry;

} // namespace

std::shared_ptr<env> open_env_shared(const std::string& path, unsigned int flags, mdb_mode_t mode) {
    std::lock_guard<std::mutex> lock(g_registryMutex);
    auto it = g_registry.find(path);
    if (it != g_registry.end()) {
        if (auto existing = it->second.handle.lock()) {
            if (existing->flags() != flags || existing->mode() != mode) {
                throw std::runtime_error(
                    "lmdbxx: open_env_shared: environment already open for path '" + path +
                    "' with different flags/mode");
            }
            return existing;
        }
        g_registry.erase(it);
    }
    auto created = std::make_shared<env>(path, flags, mode);
    g_registry[path] = RegistryEntry{created, flags, mode};
    return created;
}

} // namespace lmdbxx

////////////////////////////////////////////////////////////////////////////////
// Tcl command layer
////////////////////////////////////////////////////////////////////////////////

namespace {

// ---- thread-local transaction context ----------------------------------

struct TxnContext {
    std::shared_ptr<lmdbxx::dbi> dbi;
    lmdb::txn* txn = nullptr;
    bool write = false;
};

thread_local TxnContext g_ctx;

bool RequireReadContext(Tcl_Interp* interp) {
    if (g_ctx.txn == nullptr) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(
            "lmdbxx: no active transaction (must be called inside eval_read or eval_write)", -1));
        return false;
    }
    return true;
}

bool RequireWriteContext(Tcl_Interp* interp) {
    if (g_ctx.txn == nullptr || !g_ctx.write) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(
            "lmdbxx: write operation requires an active eval_write transaction", -1));
        return false;
    }
    return true;
}

// ---- argument parsing helpers -------------------------------------------

bool GetUInt(Tcl_Interp* interp, Tcl_Obj* obj, unsigned int& out) {
    Tcl_WideInt wide = 0;
    if (Tcl_GetWideIntFromObj(interp, obj, &wide) != TCL_OK) {
        return false;
    }
    if (wide < 0 || wide > static_cast<Tcl_WideInt>((std::numeric_limits<unsigned int>::max)())) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("lmdbxx: value out of range for unsigned int", -1));
        return false;
    }
    out = static_cast<unsigned int>(wide);
    return true;
}

bool GetSizeT(Tcl_Interp* interp, Tcl_Obj* obj, std::size_t& out) {
    Tcl_WideInt wide = 0;
    if (Tcl_GetWideIntFromObj(interp, obj, &wide) != TCL_OK) {
        return false;
    }
    if (wide < 0) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("lmdbxx: value must not be negative", -1));
        return false;
    }
    out = static_cast<std::size_t>(wide);
    return true;
}

bool NormalizePath(Tcl_Interp* interp, Tcl_Obj* pathObj, std::string& out) {
    try {
        Tcl_Obj* obj = pathObj;
        Tcl_IncrRefCount(obj);
        tclxx::ObjGuard guard(obj);

        if (Tcl_FSGetPathType(obj) != TCL_PATH_ABSOLUTE) {
            Tcl_Obj* cwd = Tcl_FSGetCwd(interp);
            if (!cwd) {
                throw std::runtime_error("lmdbxx: unable to determine current working directory");
            }
            Tcl_IncrRefCount(cwd);
            Tcl_Obj* joined = Tcl_FSJoinToPath(cwd, 1, &obj);
            Tcl_DecrRefCount(cwd);
            if (!joined) {
                throw std::runtime_error("lmdbxx: unable to resolve relative path");
            }
            Tcl_IncrRefCount(joined);
            guard.reset(joined);
            obj = joined;
        }

        Tcl_Obj* normalized = Tcl_FSGetNormalizedPath(interp, obj);
        if (!normalized) {
            throw std::runtime_error("lmdbxx: unable to normalize path");
        }
        int len = 0;
        const char* s = Tcl_GetStringFromObj(normalized, &len);
        if (static_cast<int>(std::strlen(s)) != len) {
            throw std::runtime_error("lmdbxx: path must not contain embedded NUL bytes");
        }
        out.assign(s, static_cast<std::size_t>(len));
        return true;
    } catch (const std::exception& e) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(e.what(), -1));
        return false;
    }
}

template <typename T>
T* ResolveHandle(Tcl_Interp* interp, Tcl_Obj* varNameObj, const char* what) {
    // eval_read/eval_write take a *variable name* (not a dereferenced value)
    // and always resolve it in the global namespace, so helper procs can
    // pass the name through without needing `global` or re-binding a local
    // variable to the same handle.
    Tcl_Obj* valueObj =
        Tcl_ObjGetVar2(interp, varNameObj, nullptr, TCL_GLOBAL_ONLY | TCL_LEAVE_ERR_MSG);
    if (!valueObj) {
        return nullptr;
    }
    T* ptr = nullptr;
    try {
        ptr = tclxx::obj_cast::to<T*>(interp, valueObj);
    } catch (const std::exception& e) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(e.what(), -1));
        return nullptr;
    }
    if (!ptr) {
        std::string msg = std::string("lmdbxx: not a valid ") + what + " handle";
        Tcl_SetObjResult(interp, Tcl_NewStringObj(msg.c_str(), -1));
        return nullptr;
    }
    return ptr;
}

// Unlike ResolveHandle (which treats its Tcl_Obj argument as a *variable
// name* to be looked up in the caller's scope, matching eval_read/eval_write's
// documented semantics), this treats the Tcl_Obj argument as the handle
// *value* itself (already dereferenced by the Tcl parser, e.g. `$myEnv`).
template <typename T>
T* ResolveHandleValue(Tcl_Interp* interp, Tcl_Obj* valueObj, const char* what) {
    T* ptr = nullptr;
    try {
        ptr = tclxx::obj_cast::to<T*>(interp, valueObj);
    } catch (const std::exception& e) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(e.what(), -1));
        return nullptr;
    }
    if (!ptr) {
        std::string msg = std::string("lmdbxx: not a valid ") + what + " handle";
        Tcl_SetObjResult(interp, Tcl_NewStringObj(msg.c_str(), -1));
        return nullptr;
    }
    return ptr;
}

// ---- commands -------------------------------------------------------------

int OpenEnvSharedCmd(ClientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    if (objc != 4) {
        Tcl_WrongNumArgs(interp, 1, objv, "path flags mode");
        return TCL_ERROR;
    }
    std::string path;
    if (!NormalizePath(interp, objv[1], path)) {
        return TCL_ERROR;
    }
    unsigned int flags = 0;
    if (!GetUInt(interp, objv[2], flags)) {
        return TCL_ERROR;
    }
    unsigned int modeRaw = 0;
    if (!GetUInt(interp, objv[3], modeRaw)) {
        return TCL_ERROR;
    }

    try {
        auto envPtr = lmdbxx::open_env_shared(path, flags, static_cast<mdb_mode_t>(modeRaw));
        Tcl_SetObjResult(
            interp, tclxx::obj_cast::from_shared<tclxx::detail::ownership::shared>(envPtr));
        return TCL_OK;
    } catch (const std::exception& e) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(e.what(), -1));
        return TCL_ERROR;
    }
}

int EnvSetMapsizeCmd(ClientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "envVar mapsize");
        return TCL_ERROR;
    }
    std::size_t mapsize = 0;
    if (!GetSizeT(interp, objv[2], mapsize)) {
        return TCL_ERROR;
    }
    lmdbxx::env* envPtr = ResolveHandleValue<lmdbxx::env>(interp, objv[1], "environment");
    if (!envPtr) {
        return TCL_ERROR;
    }
    try {
        envPtr->set_mapsize(mapsize);
        return TCL_OK;
    } catch (const std::exception& e) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(e.what(), -1));
        return TCL_ERROR;
    }
}

int OpenDbSharedCmd(ClientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "envVar");
        return TCL_ERROR;
    }
    if (g_ctx.txn != nullptr) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(
            "lmdbxx: open_db_shared cannot be called while a transaction is active on this thread",
            -1));
        return TCL_ERROR;
    }
    lmdbxx::env* envPtr = ResolveHandleValue<lmdbxx::env>(interp, objv[1], "environment");
    if (!envPtr) {
        return TCL_ERROR;
    }
    try {
        auto dbiPtr = envPtr->open_default_dbi();
        Tcl_SetObjResult(
            interp, tclxx::obj_cast::from_shared<tclxx::detail::ownership::shared>(dbiPtr));
        return TCL_OK;
    } catch (const std::exception& e) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(e.what(), -1));
        return TCL_ERROR;
    }
}

int EvalTxnImpl(Tcl_Interp* interp, int objc, Tcl_Obj* const objv[], bool write) {
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "dbVar body");
        return TCL_ERROR;
    }
    if (g_ctx.txn != nullptr) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(
            "lmdbxx: a transaction is already active on this thread (nested eval_read/eval_write "
            "is not allowed)",
            -1));
        return TCL_ERROR;
    }
    lmdbxx::dbi* dbiPtr = ResolveHandle<lmdbxx::dbi>(interp, objv[1], "database");
    if (!dbiPtr) {
        return TCL_ERROR;
    }

    std::shared_ptr<lmdbxx::dbi> dbiShared = dbiPtr->shared_from_this();
    std::shared_ptr<lmdbxx::env> envShared = dbiShared->env_ptr();

    std::unique_ptr<lmdb::txn> txnPtr;
    try {
        txnPtr = std::make_unique<lmdb::txn>(
            lmdb::txn::begin(envShared->handle(), nullptr, write ? 0u : static_cast<unsigned int>(MDB_RDONLY)));
    } catch (const std::exception& e) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(e.what(), -1));
        return TCL_ERROR;
    }
    envShared->mark_txn_started();

    g_ctx.dbi = dbiShared;
    g_ctx.txn = txnPtr.get();
    g_ctx.write = write;

    int rc = Tcl_EvalObjEx(interp, objv[2], 0);

    g_ctx.dbi.reset();
    g_ctx.txn = nullptr;
    g_ctx.write = false;

    if (write && rc == TCL_OK) {
        try {
            txnPtr->commit();
        } catch (const std::exception& e) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj(e.what(), -1));
            return TCL_ERROR;
        }
    } else {
        txnPtr->abort();
    }
    return rc;
}

int EvalWriteCmd(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    return EvalTxnImpl(interp, objc, objv, true);
}

int EvalReadCmd(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    return EvalTxnImpl(interp, objc, objv, false);
}

int GetCmd(ClientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "key");
        return TCL_ERROR;
    }
    if (!RequireReadContext(interp)) {
        return TCL_ERROR;
    }
    int keyLen = 0;
    unsigned char* keyBytes = Tcl_GetByteArrayFromObj(objv[1], &keyLen);
    std::string_view keyView(reinterpret_cast<const char*>(keyBytes), static_cast<std::size_t>(keyLen));
    std::string_view dataView;
    try {
        bool found = g_ctx.dbi->handle().get(g_ctx.txn->handle(), keyView, dataView);
        if (!found) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("lmdbxx: get: key not found (MDB_NOTFOUND)", -1));
            return TCL_ERROR;
        }
    } catch (const std::exception& e) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(e.what(), -1));
        return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, Tcl_NewByteArrayObj(
        reinterpret_cast<const unsigned char*>(dataView.data()), static_cast<int>(dataView.size())));
    return TCL_OK;
}

int ExistsCmd(ClientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "key");
        return TCL_ERROR;
    }
    if (!RequireReadContext(interp)) {
        return TCL_ERROR;
    }
    int keyLen = 0;
    unsigned char* keyBytes = Tcl_GetByteArrayFromObj(objv[1], &keyLen);
    std::string_view keyView(reinterpret_cast<const char*>(keyBytes), static_cast<std::size_t>(keyLen));
    std::string_view dataView;
    try {
        bool found = g_ctx.dbi->handle().get(g_ctx.txn->handle(), keyView, dataView);
        Tcl_SetObjResult(interp, Tcl_NewBooleanObj(found ? 1 : 0));
        return TCL_OK;
    } catch (const std::exception& e) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(e.what(), -1));
        return TCL_ERROR;
    }
}

int PutCmd(ClientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    if (objc != 3 && objc != 4) {
        Tcl_WrongNumArgs(interp, 1, objv, "key value ?flags?");
        return TCL_ERROR;
    }
    if (!RequireWriteContext(interp)) {
        return TCL_ERROR;
    }
    unsigned int flags = 0;
    if (objc == 4 && !GetUInt(interp, objv[3], flags)) {
        return TCL_ERROR;
    }
    int keyLen = 0, valLen = 0;
    unsigned char* keyBytes = Tcl_GetByteArrayFromObj(objv[1], &keyLen);
    unsigned char* valBytes = Tcl_GetByteArrayFromObj(objv[2], &valLen);
    std::string_view keyView(reinterpret_cast<const char*>(keyBytes), static_cast<std::size_t>(keyLen));
    std::string_view valView(reinterpret_cast<const char*>(valBytes), static_cast<std::size_t>(valLen));
    try {
        bool stored = g_ctx.dbi->handle().put(g_ctx.txn->handle(), keyView, valView, flags);
        if (!stored) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("lmdbxx: put: key already exists (MDB_KEYEXIST)", -1));
            return TCL_ERROR;
        }
    } catch (const std::exception& e) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(e.what(), -1));
        return TCL_ERROR;
    }
    return TCL_OK;
}

int DelCmd(ClientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "key");
        return TCL_ERROR;
    }
    if (!RequireWriteContext(interp)) {
        return TCL_ERROR;
    }
    int keyLen = 0;
    unsigned char* keyBytes = Tcl_GetByteArrayFromObj(objv[1], &keyLen);
    std::string_view keyView(reinterpret_cast<const char*>(keyBytes), static_cast<std::size_t>(keyLen));
    try {
        bool removed = g_ctx.dbi->handle().del(g_ctx.txn->handle(), keyView);
        if (!removed) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("lmdbxx: del: key not found (MDB_NOTFOUND)", -1));
            return TCL_ERROR;
        }
    } catch (const std::exception& e) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(e.what(), -1));
        return TCL_ERROR;
    }
    return TCL_OK;
}

int MputCmd(ClientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    if (objc != 2 && objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "keyValueList ?flags?");
        return TCL_ERROR;
    }
    if (!RequireWriteContext(interp)) {
        return TCL_ERROR;
    }
    int itemCount = 0;
    Tcl_Obj** items = nullptr;
    if (Tcl_ListObjGetElements(interp, objv[1], &itemCount, &items) != TCL_OK) {
        return TCL_ERROR;
    }
    if (itemCount % 2 != 0) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(
            "lmdbxx: mput: key/value list must contain an even number of elements", -1));
        return TCL_ERROR;
    }
    unsigned int flags = 0;
    if (objc == 3 && !GetUInt(interp, objv[2], flags)) {
        return TCL_ERROR;
    }
    try {
        for (int i = 0; i < itemCount; i += 2) {
            int keyLen = 0;
            int valueLen = 0;
            unsigned char* keyBytes = Tcl_GetByteArrayFromObj(items[i], &keyLen);
            unsigned char* valueBytes = Tcl_GetByteArrayFromObj(items[i + 1], &valueLen);
            std::string_view key(reinterpret_cast<const char*>(keyBytes), static_cast<std::size_t>(keyLen));
            std::string_view value(reinterpret_cast<const char*>(valueBytes), static_cast<std::size_t>(valueLen));
            if (!g_ctx.dbi->handle().put(g_ctx.txn->handle(), key, value, flags)) {
                std::string message = "lmdbxx: mput: key already exists at index " +
                    std::to_string(i / 2) + " (MDB_KEYEXIST)";
                Tcl_SetObjResult(interp, Tcl_NewStringObj(message.c_str(), -1));
                return TCL_ERROR;
            }
        }
    } catch (const std::exception& e) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(e.what(), -1));
        return TCL_ERROR;
    }
    return TCL_OK;
}

int MgetCmd(ClientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "keyList");
        return TCL_ERROR;
    }
    if (!RequireReadContext(interp)) {
        return TCL_ERROR;
    }
    int keyCount = 0;
    Tcl_Obj** keys = nullptr;
    if (Tcl_ListObjGetElements(interp, objv[1], &keyCount, &keys) != TCL_OK) {
        return TCL_ERROR;
    }
    Tcl_Obj* result = Tcl_NewListObj(0, nullptr);
    try {
        for (int i = 0; i < keyCount; ++i) {
            int keyLen = 0;
            unsigned char* keyBytes = Tcl_GetByteArrayFromObj(keys[i], &keyLen);
            std::string_view key(reinterpret_cast<const char*>(keyBytes), static_cast<std::size_t>(keyLen));
            std::string_view value;
            if (!g_ctx.dbi->handle().get(g_ctx.txn->handle(), key, value)) {
                continue;
            }
            Tcl_ListObjAppendElement(interp, result, Tcl_NewByteArrayObj(keyBytes, keyLen));
            Tcl_ListObjAppendElement(interp, result, Tcl_NewByteArrayObj(
                reinterpret_cast<const unsigned char*>(value.data()), static_cast<int>(value.size())));
        }
    } catch (const std::exception& e) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(e.what(), -1));
        return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, result);
    return TCL_OK;
}

int LgetCmd(ClientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "keyList");
        return TCL_ERROR;
    }
    if (!RequireReadContext(interp)) {
        return TCL_ERROR;
    }
    int keyCount = 0;
    Tcl_Obj** keys = nullptr;
    if (Tcl_ListObjGetElements(interp, objv[1], &keyCount, &keys) != TCL_OK) {
        return TCL_ERROR;
    }
    Tcl_Obj* result = Tcl_NewListObj(0, nullptr);
    try {
        for (int i = 0; i < keyCount; ++i) {
            int keyLen = 0;
            unsigned char* keyBytes = Tcl_GetByteArrayFromObj(keys[i], &keyLen);
            std::string_view key(reinterpret_cast<const char*>(keyBytes), static_cast<std::size_t>(keyLen));
            std::string_view value;
            if (g_ctx.dbi->handle().get(g_ctx.txn->handle(), key, value)) {
                Tcl_ListObjAppendElement(interp, result, Tcl_NewByteArrayObj(
                    reinterpret_cast<const unsigned char*>(value.data()), static_cast<int>(value.size())));
            } else {
                Tcl_ListObjAppendElement(interp, result, Tcl_NewByteArrayObj(nullptr, 0));
            }
        }
    } catch (const std::exception& e) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(e.what(), -1));
        return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, result);
    return TCL_OK;
}

int MdelCmd(ClientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "keyList");
        return TCL_ERROR;
    }
    if (!RequireWriteContext(interp)) {
        return TCL_ERROR;
    }
    int keyCount = 0;
    Tcl_Obj** keys = nullptr;
    if (Tcl_ListObjGetElements(interp, objv[1], &keyCount, &keys) != TCL_OK) {
        return TCL_ERROR;
    }
    try {
        for (int i = 0; i < keyCount; ++i) {
            int keyLen = 0;
            unsigned char* keyBytes = Tcl_GetByteArrayFromObj(keys[i], &keyLen);
            std::string_view key(reinterpret_cast<const char*>(keyBytes), static_cast<std::size_t>(keyLen));
            if (!g_ctx.dbi->handle().del(g_ctx.txn->handle(), key)) {
                std::string message = "lmdbxx: mdel: key not found at index " +
                    std::to_string(i) + " (MDB_NOTFOUND)";
                Tcl_SetObjResult(interp, Tcl_NewStringObj(message.c_str(), -1));
                return TCL_ERROR;
            }
        }
    } catch (const std::exception& e) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(e.what(), -1));
        return TCL_ERROR;
    }
    return TCL_OK;
}

// ---- iterators --------------------------------------------------------

enum class IterDir { Forward, Reverse };

bool ParseVarSpec(Tcl_Interp* interp, Tcl_Obj* specObj, Tcl_Obj** keyVarOut, Tcl_Obj** dataVarOut) {
    int objc = 0;
    Tcl_Obj** objv = nullptr;
    if (Tcl_ListObjGetElements(interp, specObj, &objc, &objv) != TCL_OK) {
        return false;
    }
    if (objc != 2) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(
            "lmdbxx: variable spec must be a two-element list {keyVar dataVar}", -1));
        return false;
    }
    int n1 = 0, n2 = 0;
    const char* s1 = Tcl_GetStringFromObj(objv[0], &n1);
    const char* s2 = Tcl_GetStringFromObj(objv[1], &n2);
    if (n1 == 0 || n2 == 0) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(
            "lmdbxx: variable names in spec must not be empty", -1));
        return false;
    }
    if (n1 == n2 && std::memcmp(s1, s2, static_cast<std::size_t>(n1)) == 0) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(
            "lmdbxx: keyVar and dataVar must be distinct names", -1));
        return false;
    }
    *keyVarOut = objv[0];
    *dataVarOut = objv[1];
    return true;
}

// Sets loop vars, evaluates body. Returns TCL_OK to keep iterating (also on
// TCL_CONTINUE/TCL_BREAK, with shouldBreak set for the latter); any other
// code is propagated unchanged to the caller.
int RunIteration(Tcl_Interp* interp, Tcl_Obj* keyVarObj, Tcl_Obj* dataVarObj, std::string_view key,
                  std::string_view data, Tcl_Obj* bodyObj, bool& shouldBreak) {
    Tcl_Obj* keyObj = Tcl_NewByteArrayObj(
        reinterpret_cast<const unsigned char*>(key.data()), static_cast<int>(key.size()));
    Tcl_Obj* dataObj = Tcl_NewByteArrayObj(
        reinterpret_cast<const unsigned char*>(data.data()), static_cast<int>(data.size()));
    if (!Tcl_ObjSetVar2(interp, keyVarObj, nullptr, keyObj, TCL_LEAVE_ERR_MSG)) {
        return TCL_ERROR;
    }
    if (!Tcl_ObjSetVar2(interp, dataVarObj, nullptr, dataObj, TCL_LEAVE_ERR_MSG)) {
        return TCL_ERROR;
    }
    int rc = Tcl_EvalObjEx(interp, bodyObj, 0);
    if (rc == TCL_CONTINUE) {
        Tcl_ResetResult(interp);
        return TCL_OK;
    }
    if (rc == TCL_BREAK) {
        Tcl_ResetResult(interp);
        shouldBreak = true;
        return TCL_OK;
    }
    return rc;
}

int IterateRange(Tcl_Interp* interp, Tcl_Obj* keyVarObj, Tcl_Obj* dataVarObj, std::string_view startKey,
                  bool hasStart, std::string_view endKey, bool hasEnd, Tcl_Obj* bodyObj, IterDir dir) {
    try {
        lmdb::cursor cur = lmdb::cursor::open(g_ctx.txn->handle(), g_ctx.dbi->handle().handle());

        std::string_view key;
        std::string_view data;
        bool positioned;

        if (dir == IterDir::Forward) {
            if (hasStart) {
                key = startKey;
                positioned = cur.get(key, data, MDB_SET_RANGE);
            } else {
                positioned = cur.get(key, data, MDB_FIRST);
            }
        } else {
            if (hasEnd) {
                std::string_view seek = endKey;
                positioned = cur.get(seek, data, MDB_SET_RANGE);
                if (positioned) {
                    key = seek;
                    if (key.compare(endKey) > 0) {
                        positioned = cur.get(key, data, MDB_PREV);
                    }
                } else {
                    positioned = cur.get(key, data, MDB_LAST);
                }
            } else {
                positioned = cur.get(key, data, MDB_LAST);
            }
        }

        bool shouldBreak = false;
        while (positioned) {
            if (dir == IterDir::Forward && hasEnd && key.compare(endKey) > 0) {
                break;
            }
            if (dir == IterDir::Reverse && hasStart && key.compare(startKey) < 0) {
                break;
            }

            int rc = RunIteration(interp, keyVarObj, dataVarObj, key, data, bodyObj, shouldBreak);
            if (rc != TCL_OK) {
                return rc;
            }
            if (shouldBreak) {
                break;
            }

            positioned = cur.get(key, data, dir == IterDir::Forward ? MDB_NEXT : MDB_PREV);
        }
        return TCL_OK;
    } catch (const std::exception& e) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(e.what(), -1));
        return TCL_ERROR;
    }
}

int ForeachCmdImpl(Tcl_Interp* interp, int objc, Tcl_Obj* const objv[], IterDir dir) {
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "{keyVar dataVar} body");
        return TCL_ERROR;
    }
    if (!RequireReadContext(interp)) {
        return TCL_ERROR;
    }
    Tcl_Obj *keyVarObj = nullptr, *dataVarObj = nullptr;
    if (!ParseVarSpec(interp, objv[1], &keyVarObj, &dataVarObj)) {
        return TCL_ERROR;
    }
    return IterateRange(interp, keyVarObj, dataVarObj, std::string_view{}, false, std::string_view{}, false,
                         objv[2], dir);
}

int ForeachCmd(ClientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    return ForeachCmdImpl(interp, objc, objv, IterDir::Forward);
}

int ForeachReverseCmd(ClientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    return ForeachCmdImpl(interp, objc, objv, IterDir::Reverse);
}

int ForrangeCmdImpl(Tcl_Interp* interp, int objc, Tcl_Obj* const objv[], IterDir dir) {
    if (objc != 5) {
        Tcl_WrongNumArgs(interp, 1, objv, "{keyVar dataVar} startKey endKey body");
        return TCL_ERROR;
    }
    if (!RequireReadContext(interp)) {
        return TCL_ERROR;
    }
    Tcl_Obj *keyVarObj = nullptr, *dataVarObj = nullptr;
    if (!ParseVarSpec(interp, objv[1], &keyVarObj, &dataVarObj)) {
        return TCL_ERROR;
    }
    int startLen = 0, endLen = 0;
    unsigned char* startBytes = Tcl_GetByteArrayFromObj(objv[2], &startLen);
    unsigned char* endBytes = Tcl_GetByteArrayFromObj(objv[3], &endLen);
    std::string_view startKey(reinterpret_cast<const char*>(startBytes), static_cast<std::size_t>(startLen));
    std::string_view endKey(reinterpret_cast<const char*>(endBytes), static_cast<std::size_t>(endLen));
    return IterateRange(interp, keyVarObj, dataVarObj, startKey, startLen != 0, endKey, endLen != 0, objv[4],
                         dir);
}

int ForrangeCmd(ClientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    return ForrangeCmdImpl(interp, objc, objv, IterDir::Forward);
}

int ForrangeReverseCmd(ClientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    return ForrangeCmdImpl(interp, objc, objv, IterDir::Reverse);
}

bool SetConstant(Tcl_Interp* interp, const char* name, unsigned int value) {
    std::string variableName = std::string("::lmdbxx::") + name;
    return Tcl_SetVar2Ex(interp, variableName.c_str(), nullptr,
                         Tcl_NewWideIntObj(static_cast<Tcl_WideInt>(value)),
                         TCL_GLOBAL_ONLY) != nullptr;
}

bool SetLmdbConstants(Tcl_Interp* interp) {
    struct Constant {
        const char* name;
        unsigned int value;
    };
    static const Constant constants[] = {
        {"MDB_FIXEDMAP", MDB_FIXEDMAP},
        {"MDB_ENCRYPT", MDB_ENCRYPT},
        {"MDB_NOSUBDIR", MDB_NOSUBDIR},
        {"MDB_NOSYNC", MDB_NOSYNC},
        {"MDB_RDONLY", MDB_RDONLY},
        {"MDB_NOMETASYNC", MDB_NOMETASYNC},
        {"MDB_WRITEMAP", MDB_WRITEMAP},
        {"MDB_MAPASYNC", MDB_MAPASYNC},
        {"MDB_NOTLS", MDB_NOTLS},
        {"MDB_NOLOCK", MDB_NOLOCK},
        {"MDB_NORDAHEAD", MDB_NORDAHEAD},
        {"MDB_NOMEMINIT", MDB_NOMEMINIT},
        {"MDB_PREVSNAPSHOT", MDB_PREVSNAPSHOT},
        {"MDB_REMAP_CHUNKS", MDB_REMAP_CHUNKS},
        {"MDB_REVERSEKEY", MDB_REVERSEKEY},
        {"MDB_DUPSORT", MDB_DUPSORT},
        {"MDB_INTEGERKEY", MDB_INTEGERKEY},
        {"MDB_DUPFIXED", MDB_DUPFIXED},
        {"MDB_INTEGERDUP", MDB_INTEGERDUP},
        {"MDB_REVERSEDUP", MDB_REVERSEDUP},
        {"MDB_CREATE", MDB_CREATE},
        {"MDB_NOOVERWRITE", MDB_NOOVERWRITE},
        {"MDB_NODUPDATA", MDB_NODUPDATA},
        {"MDB_CURRENT", MDB_CURRENT},
        {"MDB_RESERVE", MDB_RESERVE},
        {"MDB_APPEND", MDB_APPEND},
        {"MDB_APPENDDUP", MDB_APPENDDUP},
        {"MDB_MULTIPLE", MDB_MULTIPLE},
    };
    for (const auto& constant : constants) {
        if (!SetConstant(interp, constant.name, constant.value)) {
            return false;
        }
    }
    return true;
}

} // namespace

extern "C" int Lmdbxx_Init(Tcl_Interp* interp) {
    if (Tcl_InitStubs(interp, "8.6", 0) == nullptr) {
        return TCL_ERROR;
    }
    if (Tcl_EvalEx(interp, "namespace eval ::lmdbxx {}", -1, TCL_EVAL_DIRECT) != TCL_OK ||
        !SetLmdbConstants(interp)) {
        return TCL_ERROR;
    }

    static const struct {
        const char* name;
        Tcl_ObjCmdProc* proc;
    } commands[] = {
        {"lmdbxx::open_env_shared", OpenEnvSharedCmd},
        {"lmdbxx::env_set_mapsize", EnvSetMapsizeCmd},
        {"lmdbxx::open_db_shared", OpenDbSharedCmd},
        {"lmdbxx::eval_write", EvalWriteCmd},
        {"lmdbxx::eval_read", EvalReadCmd},
        {"lmdbxx::get", GetCmd},
        {"lmdbxx::exists", ExistsCmd},
        {"lmdbxx::put", PutCmd},
        {"lmdbxx::del", DelCmd},
        {"lmdbxx::mput", MputCmd},
        {"lmdbxx::mget", MgetCmd},
        {"lmdbxx::mdel", MdelCmd},
        {"lmdbxx::lget", LgetCmd},
        {"lmdbxx::foreach", ForeachCmd},
        {"lmdbxx::foreach_reverse", ForeachReverseCmd},
        {"lmdbxx::forrange", ForrangeCmd},
        {"lmdbxx::forrange_reverse", ForrangeReverseCmd},
    };

    for (const auto& command : commands) {
        Tcl_CreateObjCommand(interp, command.name, command.proc, nullptr, nullptr);
    }

    return Tcl_PkgProvideEx(interp, "lmdbxx", "1.0.0", nullptr);
}
