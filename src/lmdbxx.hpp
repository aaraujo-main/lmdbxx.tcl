#pragma once

#include <lmdbxx/lmdb++.h>
#include <tcl.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace lmdbxx {

class dbi;

/**
 * Wraps a single `lmdb::env`, tracked process-wide by normalized path so
 * repeated `open_env_shared` calls for the same path share one instance.
 */
class env : public std::enable_shared_from_this<env> {
public:
    env(std::string path, unsigned int flags, mdb_mode_t mode);

    // tclxx's generic Tcl_Obj representation layer instantiates a
    // deep-copying code path for every wrapped type even though lmdbxx only
    // ever hands out shared (identity-preserving) handles for env/dbi; these
    // are never actually invoked at runtime, but must compile.
    env(const env& other);
    env(env&& other) noexcept;

    lmdb::env& handle() noexcept { return handle_; }
    const std::string& path() const noexcept { return path_; }
    unsigned int flags() const noexcept { return flags_; }
    mdb_mode_t mode() const noexcept { return mode_; }

    // Throws if a transaction has already started on this environment.
    void set_mapsize(std::size_t size);

    void mark_txn_started() noexcept { txn_started_.store(true, std::memory_order_relaxed); }
    bool txn_started() const noexcept { return txn_started_.load(std::memory_order_relaxed); }

    // Creates (first call) or reuses the cached default unnamed dbi for this env.
    std::shared_ptr<dbi> open_default_dbi();

private:
    std::string path_;
    unsigned int flags_;
    mdb_mode_t mode_;
    lmdb::env handle_;
    std::atomic<bool> txn_started_{false};
    std::mutex dbi_mutex_;
    std::weak_ptr<dbi> default_dbi_;
};

/**
 * Composition wrapper around `lmdb::dbi` plus the owning `lmdbxx::env`.
 * Deliberately not a subclass of `lmdb::dbi`: env ownership and dbi lifetime
 * are separate concerns.
 */
class dbi : public std::enable_shared_from_this<dbi> {
public:
    dbi(std::shared_ptr<env> envPtr, lmdb::dbi handle)
        : env_(std::move(envPtr)), handle_(handle) {}

    lmdb::dbi& handle() noexcept { return handle_; }
    const std::shared_ptr<env>& env_ptr() const noexcept { return env_; }

private:
    std::shared_ptr<env> env_;
    lmdb::dbi handle_;
};

// Process-wide registry keyed by normalized path; reuses live handles,
// rejects conflicting flags/mode for an already-open path.
std::shared_ptr<env> open_env_shared(const std::string& normalizedPath,
                                      unsigned int flags,
                                      mdb_mode_t mode);

} // namespace lmdbxx

extern "C" {
#ifdef _WIN32
__declspec(dllexport)
#endif
int Lmdbxx_Init(Tcl_Interp* interp);
}
