<p align="center">
  <img src="assets/logo.png" alt="libcorm logo" width="220" />
</p>

# libcorm

[![C99](https://img.shields.io/badge/C-C99-555?logo=c)](#)
[![BSD-2-Clause](https://img.shields.io/badge/License-BSD--2--Clause-blue)](#)
[![In-memory maps](https://img.shields.io/badge/in--memory-maps-16A34A)](#)
[![Recall Kernel](https://img.shields.io/badge/engine-recall--kernel-blueviolet)](#)
[![Resident Daemon](https://img.shields.io/badge/daemon-cormd-4B8BBE)](#)

> Ultra-fast in-memory key-value maps, secondary indexes, record-aware schemas, unified multi-axis recall kernel, the `corm` CLI tool, and the `cormd` resident memory daemon in plain C. Canonical header is `<ttypt/corm.h>`.

---

## 1. Quickstart

### The `corm` CLI Tool (< 30 Seconds)
Index documents, query entries, and run composed multi-axis searches directly from the command line:

```bash
# 1. Ingest documents tagged with lexical (liblemma) and vector (libsepal) axes:
corm -p "Zero-copy in-memory caching and RAM table indexing" docs.db@lemma,sepal:a:s
corm -p "Traditional Portuguese bakery baking warm pão de queijo" docs.db@lemma,sepal:a:s
corm -p "High-performance vector similarity search in pure C" docs.db@lemma,sepal:a:s

# 2. List all indexed entries:
corm -l docs.db

# 3. Composed multi-axis boolean search across axes:
corm -X "lemma AND sepal" --lemma="caching" --sepal="in-memory" docs.db@lemma,sepal:a:s

# 4. List all loaded/available axis plugins:
corm --list-axes
```

### In-Memory C API
Index player sessions with instant secondary lookup and safe retrieval in under 15 lines:

```c
#include <stdio.h>
#include <stdint.h>
#include <ttypt/corm.h>

int main(void) {
    // Open in-memory map: uint32 player_id -> string token
    uint32_t sessions = corm_open(NULL, NULL, CM_U32, CM_STR, 0xFF, 0);

    uint32_t player_id = 1001;
    corm_put(sessions, &player_id, "session_token_xyz");

    // Safe, non-aborting retrieval:
    char token[64];
    if (corm_read(sessions, &player_id, token, sizeof(token)) == 0) {
        printf("Player %u active session: %s\n", player_id, token);
    }

    corm_close(sessions);
    return 0;
}
```

Compile and link:
```sh
cc player_session.c $(pkg-config --cflags --libs libcorm) -o player_session
```

---

## 2. `cormd` Resident Memory Daemon

`libcorm` includes `cormd`, a resident memory daemon designed to keep high-frequency database maps, inverted full-text indexes, and vector stores warm in shared RAM:
- **Zero Cold-Boot Latency**: Keeps indexes memory-resident across ephemeral CLI tools, CGI scripts, and worker restarts.
- **Fast Local IPC**: Communicates over non-blocking UNIX domain sockets with zero-copy shared memory mapping.
- **Background Persistence**: Flushes write journals to disk asynchronously while maintaining sub-microsecond in-memory read throughput.

```bash
# Start cormd in background listening on standard socket
cormd -d -s /var/run/corm.sock
```

All `corm` CLI commands automatically lease and route mutations through `cormd` when `.cormd.lock` is held.

---

## 3. `corm` CLI Options & Multi-Axis Syntax

The filespec syntax `file[@axis1,axis2...][:k[:v]]` binds dynamic axis plugins to database columns:
- `k` and `v`: Key and value types (`u` = uint32, `s` = string, `a` = auto-increment uint32 ID).
- `@axis`: Attaches domain axis engines: `@lemma` (Text), `@sepal` (Meaning), `@joint` (Time), `@islet` (Space).

| Option | Syntax | Description |
| :--- | :--- | :--- |
| `-p` | `-p KEY[:VAL]` | Put a key/value pair into database |
| `-g` | `-g KEY` (or `-g .`) | Get value(s) for a key (use `.` for all keys) |
| `-d` / `-D` | `-d KEY[:VAL]` | Delete key/value pair (`-D` deletes all matching multivalue entries) |
| `-l` | `-l` | List all stored values |
| `-X` | `-X "EXPR"` | Composed multi-axis set query (`AND`, `OR`, `EXCEPT`, `NOT`) |
| `--<axis>=VAL` | `--lemma="query"` | Pass query parameters to specific bound axis plugin |
| `--list-axes` | *none* | List all registered axis plugins and exit |

---

## 4. The Unified Recall Kernel (`<ttypt/cormr.h>`)

`libcorm` hosts the domain-free Recall Kernel, standardizing multi-dimensional retrieval into a high-throughput `"filter by axis → join → rank"` pipeline over uniform 32-bit references (`cormr_ref_t`).

### 50-Microsecond Multidimensional Search
Instead of querying separate databases (spatial, time-series, text) and merging them in application memory, the Space engine ([libislet](https://github.com/tty-pt/libislet)), Time engine ([libjoint](https://github.com/tty-pt/libjoint)), Text engine ([liblemma](https://github.com/tty-pt/liblemma)), and Vector engine ([libsepal](https://github.com/tty-pt/libsepal)) each stream 32-bit entity IDs into an in-memory candidate pool. `libcorm` intersects them in CPU cache lines in **50 microseconds** with zero malloc calls in the query loop.

```c
#include <ttypt/corm.h>

void search_players(void) {
    cormr_query_t *q = cormr_query_new();
    cormr_query_axis(q, "text", "warrior", CORMR_JOIN_AND);
    cormr_query_axis(q, "space", "bbox:10,10,50,50", CORMR_JOIN_AND);

    cormr_rank_t *results = cormr_query_run(q);
    cormr_ref_t ref;
    float score;
    while (cormr_rank_next(results, &ref, &score)) {
        printf("Match ref=%u score=%.3f\n", ref, score);
    }
    cormr_rank_free(results);
    cormr_query_free(q);
}
```

---

## 5. Core API Reference

| Function | Signature | Description |
| :--- | :--- | :--- |
| `corm_open` | `uint32_t corm_open(file, db, ktype, vtype, mask, flags)` | Open an in-memory or auto-persisted map |
| `corm_put` | `uint32_t corm_put(hd, key_ptr, val_ptr)` | Insert or update a key-value pair |
| `corm_get` | `const void *corm_get(hd, key_ptr)` | Retrieve a direct value pointer (`NULL` if absent) |
| `corm_read` | `int corm_read(hd, key_ptr, out_buf, max_sz)` | Non-aborting safe retrieval with zero-initialization on miss |
| `corm_del` | `int corm_del(hd, key_ptr)` | Delete key-value entry from map |
| `corm_assoc` | `void corm_assoc(sec_hd, prim_hd, cb, ud)` | Automatically maintain an inverted secondary index |
| `corm_iter` | `uint32_t corm_iter(hd, key_ptr, flags)` | Iterate all entries or scan a sorted range |
| `corm_rec_reg` | `uint32_t corm_rec_reg(name, fields, n)` | Register a structured record schema |
| `cormr_query_run` | `cormr_rank_t *cormr_query_run(query)` | Execute multi-axis boolean join and rank loop |

👉 *For custom type registration, multi-value keys, ID managers, and the complete 40+ function reference, see [docs/API.md](docs/API.md).*

---

## 6. Installation & Build

### Prebuilt Packages
Prebuilt packages for Linux (APT / Alpine / Arch / Fedora-RHEL), macOS (Homebrew), Windows (winget / MSYS2), and OpenBSD are distributed via [tty.pt packages](https://github.com/tty-pt/ci/blob/main/docs/install.md). Use **libcorm** as the package name.

### Build from Source
```sh
make                  # builds lib/libcorm.so, bin/corm, bin/cormd, and test suite
make test             # run unit, CLI, and recall-kernel tests
sudo make install     # install headers, daemon, and library to system
```

## License

BSD 2-Clause License. Copyright (c) 2025, tty-pt. See `LICENSE`.
