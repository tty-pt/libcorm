<p align="center">
  <img src="assets/logo.png" alt="libcorm logo" width="220" />
</p>

# libcorm

[![C99](https://img.shields.io/badge/C-C99-555?logo=c)](#)
[![BSD-2-Clause](https://img.shields.io/badge/License-BSD--2--Clause-blue)](#)
[![In-memory maps](https://img.shields.io/badge/in--memory-maps-16A34A)](#)
[![Recall Kernel](https://img.shields.io/badge/engine-recall--kernel-blueviolet)](#)
[![Resident Daemon](https://img.shields.io/badge/daemon-cormd-4B8BBE)](#)

> Need SQLite-level query indexing in plain C without disk overhead or SQL parser complexity? **libcorm** provides ultra-fast in-memory key-value maps with secondary indexes, bidirectional mirrors, record-aware schemas, the unified recall kernel, and the `cormd` resident memory daemon. Canonical header is `<ttypt/corm.h>`.

---

## 1. Quickstart

### In-Memory Key-Value Map with Secondary Index
Index active player sessions with instant secondary lookup in under 15 lines:

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

## 2. Core API Reference

The essential functions for in-memory and persisted key-value operations:

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

## 3. The Unified Recall Kernel (`<ttypt/cormr.h>`)

`libcorm` hosts the domain-free Recall Kernel, standardizing multi-dimensional retrieval into a high-throughput `"filter by axis → join → rank"` pipeline over uniform 32-bit references (`cormr_ref_t`).

### The 50-Microsecond Multidimensional Search
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

## 4. `cormd` Resident Memory Daemon

`libcorm` includes `cormd`, a resident memory daemon designed to keep high-frequency database maps, inverted full-text indexes, and vector stores warm in shared RAM:
- **Zero Cold Boot Latency**: Keeps indexes memory-resident across ephemeral command-line tools, CGI handlers, and worker restarts.
- **Fast Local IPC**: Communicates over non-blocking UNIX domain sockets with zero-copy shared memory mapping.
- **Background Persistence**: Flushes write journals to disk asynchronously while maintaining sub-microsecond in-memory read throughput.

```bash
# Start cormd in background listening on standard socket
cormd -d -s /var/run/corm.sock
```

---

## 5. Installation & Build

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
