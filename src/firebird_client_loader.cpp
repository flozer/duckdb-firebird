#include "firebird_client_loader.hpp"

#include "duckdb/common/exception.hpp"

#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #include <dlfcn.h>
#endif

namespace duckdb {

#ifdef _WIN32
using LibHandle = HMODULE;
static LibHandle PlatformOpen(const char *name) {
    return LoadLibraryA(name);
}
static void *PlatformSym(LibHandle handle, const char *sym) {
    return reinterpret_cast<void *>(GetProcAddress(handle, sym));
}
#else
using LibHandle = void *;
static LibHandle PlatformOpen(const char *name) {
    return dlopen(name, RTLD_NOW | RTLD_GLOBAL);
}
static void *PlatformSym(LibHandle handle, const char *sym) {
    return dlsym(handle, sym);
}
#endif

// Platform-specific default candidates, ordered by what users
// typically have installed. On Linux libfbclient.so.2 is the
// canonical SONAME that distros symlink; libfbclient.so without a
// version suffix is the dev-package symlink. Windows ships
// fbclient.dll (MS runtime) and fbclient_ms.dll (older MSVC).
static std::vector<std::string> DefaultLibraryCandidates() {
#ifdef _WIN32
    return {"fbclient.dll", "fbclient_ms.dll"};
#elif defined(__APPLE__)
    return {"libfbclient.dylib"};
#else
    return {"libfbclient.so.2", "libfbclient.so"};
#endif
}

static void BindSymbols(FirebirdClientApi &api,
                         LibHandle handle,
                         const std::string &source) {
#define BIND(sym)                                                              \
    do {                                                                        \
        api.sym = reinterpret_cast<decltype(api.sym)>(                          \
            PlatformSym(handle, #sym));                                         \
        if (!api.sym) {                                                         \
            throw IOException(                                                  \
                "Firebird client library loaded from '" + source +              \
                "' is missing the '" #sym "' symbol. The library looks too "    \
                "old or is not a Firebird build.");                             \
        }                                                                       \
    } while (0)

    BIND(isc_attach_database);
    BIND(isc_detach_database);
    BIND(isc_start_transaction);
    BIND(isc_rollback_transaction);
    BIND(isc_dsql_allocate_statement);
    BIND(isc_dsql_prepare);
    BIND(isc_dsql_describe);
    BIND(isc_dsql_describe_bind);
    BIND(isc_dsql_execute);
    BIND(isc_dsql_fetch);
    BIND(isc_dsql_free_statement);
    BIND(isc_open_blob2);
    BIND(isc_close_blob);
    BIND(isc_get_segment);
    BIND(isc_sqlcode);
    BIND(fb_interpret);
#undef BIND
}

const FirebirdClientApi &fbapi() {
    static std::mutex load_lock;
    static bool loaded = false;
    static FirebirdClientApi api{};

    std::lock_guard<std::mutex> g(load_lock);
    if (loaded) {
        return api;
    }

    std::vector<std::string> tried;

    // 1. DUCKDB_FIREBIRD_CLIENT_LIBRARY explicit override. When set
    //    non-empty, it is authoritative: a failed load throws instead
    //    of silently falling back to the platform defaults, so the
    //    user gets a deterministic error pointing at the path they
    //    actually supplied.
    if (const char *env = std::getenv("DUCKDB_FIREBIRD_CLIENT_LIBRARY")) {
        if (env[0] != '\0') {
            if (auto h = PlatformOpen(env)) {
                BindSymbols(api, h, env);
                loaded = true;
                return api;
            }
            throw IOException(
                std::string("DUCKDB_FIREBIRD_CLIENT_LIBRARY is set to '") +
                env + "' but that file could not be loaded. Check the path "
                "and that the binary matches this process architecture.");
        }
    }

    // 2. Platform defaults, in install-likelihood order.
    for (const auto &name : DefaultLibraryCandidates()) {
        tried.push_back(name);
        if (auto h = PlatformOpen(name.c_str())) {
            BindSymbols(api, h, name);
            loaded = true;
            return api;
        }
    }

    std::string msg =
        "Firebird client library not found. Install the Firebird client "
        "(libfbclient on Linux/macOS, fbclient.dll on Windows) or set "
        "DUCKDB_FIREBIRD_CLIENT_LIBRARY=/path/to/library. Tried: ";
    bool first = true;
    for (const auto &name : tried) {
        if (!first) msg += ", ";
        msg += name;
        first = false;
    }
    throw IOException(msg);
}

} // namespace duckdb
