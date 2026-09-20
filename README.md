# lmdbxx.tcl

`lmdbxx.tcl` is a compact Tcl interface to LMDB. Built on vendored LMDB, header-only `lmdb++`, and `tclxx`, it offers shared environment and database handles, scoped read/write transactions, binary-safe CRUD, batch access, ordered iteration, and LMDB flag constants. `tclxx` handles C++ object conversion and shared ownership.

## Compact API

```tcl
set env [lmdbxx::open_env_shared path flags mode]
lmdbxx::env_set_mapsize $env bytes       ;# before open_db_shared/transactions
set db [lmdbxx::open_db_shared $env]

lmdbxx::eval_write dbVar {               ;# commit only when body returns TCL_OK
    lmdbxx::put key value ?flags?
    lmdbxx::del key
    lmdbxx::mput {key value ...} ?flags?
    lmdbxx::mdel {key ...}
}
lmdbxx::eval_read dbVar {                ;# always abort; read commands only
    lmdbxx::get key | lmdbxx::exists key
    lmdbxx::mget {key ...} | lmdbxx::lget {key ...}
    lmdbxx::foreach {keyVar dataVar} body
    lmdbxx::foreach_reverse {keyVar dataVar} body
    lmdbxx::forrange {keyVar dataVar} start end body
    lmdbxx::forrange_reverse {keyVar dataVar} start end body
}
```

`dbVar` is a variable name, such as `db`, not `$db`; environment/database arguments elsewhere are handle values. Paths are normalized and equivalent live opens share handles. Transactions are thread-local, cannot nest, and release automatically with Tcl handle ownership. Keys and values are Tcl byte arrays, so embedded NUL bytes and empty values work; LMDB rejects empty keys.

## Quickstart

Build and expose the generated package directory through `TCLLIBPATH`:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
export TCLLIBPATH="$PWD/build/src"
tclsh
```

Use `open_env_shared` to open or reuse an environment, then open its default database with `open_db_shared`:

```tcl
package require lmdbxx 1.0.0

set env [lmdbxx::open_env_shared "/path/to/lmdb" 0 0644]
# Optional. Must run before open_db_shared or any transaction.
lmdbxx::env_set_mapsize $env 1073741824
set myDb [lmdbxx::open_db_shared $env]

# Receives db object by variable name. Write transaction commits on TCL_OK.
lmdbxx::eval_write myDb {
    lmdbxx::put "key1" "value1"
    puts [lmdbxx::get "key1"]
    puts [lmdbxx::exists "key1"]
    lmdbxx::del "key1"
    puts [lmdbxx::exists "key1"]
}

# Read transaction allows read and iterator commands only.
lmdbxx::eval_read myDb {
    puts [lmdbxx::exists "key1"]
}

# Batch access. Keys and values remain Tcl byte arrays.
lmdbxx::eval_write myDb {
    lmdbxx::mput {key1 value1 key2 value2}
}
lmdbxx::eval_read myDb {
    puts [lmdbxx::mget {key1 key2 missing}] ;# key/value pairs; missing omitted
    puts [lmdbxx::lget {key2 missing key1}] ;# values aligned with input keys
}
lmdbxx::eval_write myDb {
    lmdbxx::mdel {key1 key2}
}

# Seed data for iterator examples.
lmdbxx::eval_write myDb {
    foreach key {key1 key2 key3} {
        lmdbxx::put $key "data-$key"
    }
}

lmdbxx::eval_read myDb {
    # Inclusive ascending range. Empty boundary means unbounded.
    lmdbxx::forrange {key data} "key1" "key3" {
        puts "key: $key, data: $data"
    }

    # Inclusive descending range.
    lmdbxx::forrange_reverse {key data} "key1" "key3" {
        puts "key: $key, data: $data"
    }

    # Full ascending scan.
    lmdbxx::foreach {key data} {
        puts "key: $key, data: $data"
    }

    # Full descending scan.
    lmdbxx::foreach_reverse {key data} {
        puts "key: $key, data: $data"
    }
}

# No close command. Unset handles when finished; shared ownership closes LMDB
# after the last environment/database/transaction reference is released.
unset myDb env
```

Keys and values use Tcl byte arrays. Embedded NUL bytes and empty values are supported. LMDB rejects empty keys with `MDB_BAD_VALSIZE`.

`eval_write` aborts on Tcl errors, `return`, `break`, or `continue`; it commits only when body returns `TCL_OK`. `eval_read` always aborts its read transaction. Missing keys produce errors from `get` and `del`; `exists` returns `0`.

## Commands

| Command | Purpose |
| --- | --- |
| `open_env_shared path flags mode` | Open or reuse normalized LMDB environment. |
| `env_set_mapsize envValue mapsize` | Set map size before `open_db_shared` or any transaction starts. |
| `open_db_shared envValue` | Open or reuse environment's unnamed default database. |
| `eval_write dbVar body` | Evaluate body in write transaction. |
| `eval_read dbVar body` | Evaluate body in read-only transaction; always abort. |
| `get key` | Read value. Requires active transaction. |
| `exists key` | Test key. Requires active transaction. |
| `put key value ?flags?` | Write value. Requires `eval_write`. |
| `del key` | Delete key. Requires `eval_write`. |
| `mput {key value ...} ?flags?` | Write multiple key/value pairs. Requires `eval_write`. |
| `mget {key ...}` | Return found key/value pairs. Requires active transaction. |
| `lget {key ...}` | Return values in request order; missing keys become empty values. Requires active transaction. |
| `mdel {key ...}` | Delete multiple keys; missing keys are errors. Requires `eval_write`. |
| `foreach {keyVar dataVar} body` | Ascending full scan. |
| `foreach_reverse {keyVar dataVar} body` | Descending full scan. |
| `forrange {keyVar dataVar} start end body` | Inclusive ascending range. |
| `forrange_reverse {keyVar dataVar} start end body` | Inclusive descending range. |

`eval_read` and `eval_write` receive database variable names, such as `myDb`, not `$myDb`. `open_db_shared` and `env_set_mapsize` receive handle values, such as `$env`.

LMDB flag constants are available as Tcl namespace variables, for example:

```tcl
set env [lmdbxx::open_env_shared $path $lmdbxx::MDB_NOSYNC 0644]
lmdbxx::put key value $lmdbxx::MDB_NOOVERWRITE
```

Exposed constants include environment flags (`MDB_NOSUBDIR`, `MDB_NOSYNC`, `MDB_NOMETASYNC`, `MDB_WRITEMAP`, `MDB_MAPASYNC`, `MDB_RDONLY`, and related flags), database flags (`MDB_CREATE`, `MDB_DUPSORT`, and related flags), and put flags (`MDB_NOOVERWRITE`, `MDB_APPEND`, `MDB_RESERVE`, and related flags). Tcl namespace variables are mutable by Tcl code; extension code treats them as constant values.

Batch commands accept Tcl lists of byte-array elements. `mput` requires an even-length alternating key/value list; its optional flags apply to every put, and duplicate keys are processed sequentially. `mget` omits missing keys and returns an alternating key/value list suitable for `dict`. `lget` preserves request order and uses an empty byte array for missing keys. `mdel` stops at the first missing key. Empty input lists are no-ops. Batch operations are atomic only at transaction level: an uncaught error aborts the enclosing write transaction, while a caught error can leave earlier changes in that transaction.

## Requirements

- CMake 3.16 or newer
- C compiler with C99 support
- C++ compiler with C++17 support
- Tcl 8.6 development headers, library, and `tclsh`
- Threads support on Unix-like systems
- Network access during first test build when GoogleTest is not installed, because CMake FetchContent retrieves GoogleTest 1.15.2

LMDB, `lmdb++`, and `tclxx` sources are included in this repository.

## Build

Default build enables tests and demo smoke tests:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
```

Debug or Release build:

```sh
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j

cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
```

Disable tests or demos when needed:

```sh
cmake -S . -B build \
    -DLMDBXX_TCL_BUILD_TESTS=OFF \
    -DLMDBXX_TCL_BUILD_DEMO=OFF
cmake --build build -j
```

Install extension and package index:

```sh
cmake --install build --prefix "$PWD/install"
```

## Tests

Run all GoogleTest cases and Tcl demo smoke tests:

```sh
ctest --test-dir build --output-on-failure
```

Current suite covers package loading, environment reuse, map sizing, binary values, transactions, rollback, iterators, and automatic handle cleanup.

## Demos

Demo scripts are in [`demo/`](demo/):

- [`basic_crud.tcl`](demo/basic_crud.tcl)
- [`transactions.tcl`](demo/transactions.tcl)
- [`iterators.tcl`](demo/iterators.tcl)
- [`mapsize.tcl`](demo/mapsize.tcl)

Run one directly after building:

```sh
TCLLIBPATH="$PWD/build/src" tclsh demo/basic_crud.tcl
TCLLIBPATH="$PWD/build/src" tclsh demo/transactions.tcl
TCLLIBPATH="$PWD/build/src" tclsh demo/iterators.tcl
TCLLIBPATH="$PWD/build/src" tclsh demo/mapsize.tcl
```

## Benchmark

Benchmark compares Tcl `dict`, LMDB through `lmdbxx`, and SQLite through Tcl's `sqlite3` package for scalar insert, lookup, full-scan, and batch put/get/delete workloads. It reports separate durability profiles so write results remain comparable. SQLite is optional; install a Tcl SQLite package to include it.

Profiles:

- `lmdb-safe`: LMDB flags `0`; commit synchronization enabled.
- `lmdb-fast`: LMDB `MDB_NOSYNC` (`65536`); commit `fsync` disabled.
- `sqlite-safe`: rollback journal with `synchronous=FULL`.
- `sqlite-fast`: memory journal with `synchronous=OFF`.

Safe profiles favor crash durability. Fast profiles favor throughput and can lose recent writes after a crash. SQLite batch rows use one transaction containing one statement per key; LMDB batch rows use `mput`, `mget`, and `mdel`.

Configure with benchmark support enabled:

```sh
cmake -S . -B build-benchmark \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DLMDBXX_TCL_BUILD_TESTS=OFF \
    -DLMDBXX_TCL_BUILD_DEMO=OFF \
    -DLMDBXX_TCL_BUILD_BENCHMARK=ON
cmake --build build-benchmark --target lmdbxx_benchmark -j
```

Run custom workloads directly:

```sh
TCLLIBPATH="$PWD/build-benchmark/src" tclsh benchmark/benchmark.tcl \
    -records 100000 -value-size 128 -runs 5
```

Benchmark reports median-run operations per second. Results depend on hardware, Tcl version, LMDB map size, SQLite build, filesystem, and workload parameters. Compare runs only under matching conditions.

## License

`lmdbxx.tcl` project glue is released under the MIT License [`LICENSE.md`](LICENSE.md). See [`tclxx/LICENSE.md`](tclxx/LICENSE.md) for `tclxx`. Vendored LMDB, `lmdb++`, and other bundled components retain their respective licenses and notices.
