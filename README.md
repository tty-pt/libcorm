<p align="center">
  <img src="assets/logo.png" alt="libcorm logo" width="220" />
</p>

# libcorm

[![C99](https://img.shields.io/badge/C-C99-555?logo=c)](#)
[![BSD-2-Clause](https://img.shields.io/badge/License-BSD--2--Clause-blue)](#)
[![In-memory maps](https://img.shields.io/badge/in--memory-maps-16A34A)](#)
[![Recall Kernel](https://img.shields.io/badge/engine-recall--kernel-blueviolet)](#)

> Need SQLite-level query indexing in plain C without disk overhead or SQL parser complexity? **libcorm** provides ultra-fast in-memory key-value maps with secondary indexes, bidirectional mirrors, record-aware field access, and the unified recall kernel.

---

## Recall Kernel Composition

`libcorm` hosts the domain-free recall kernel (`rec.h`), which composes queries across four independent domain axes:

- **[libjoint](https://github.com/tty-pt/libit)** (Time Axis): Evaluates temporal presence intervals via `rec_axis_fill_interval`.
- **[libislet](https://github.com/tty-pt/libgeo)** (Space Axis): Evaluates multi-dimensional spatial bounding boxes via `Point3_2.fill_bbox`.
- **[liblemma](https://github.com/tty-pt/liblemma)** (Text Axis): Evaluates accent-preserving token matching via `rec_axis_fill_tokens`.
- **[libsepal](https://github.com/tty-pt/libsepal)** (Meaning Axis): Ranks vector candidates via `sepal_fill_approx` and `sepal_rank`.

---

## Quickstart

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

    const char *token = (const char *)corm_get(sessions, &player_id);
    if (token) {
        printf("Player %u active session: %s\n", player_id, token);
    }

    corm_close(sessions);
    return 0;
}
```

Compile and link:
```sh
cc player_session.c $(pkg-config --cflags --libs corm)
```

---

## Core API (The Essential 20%)

| Function | Signature | Description |
| :--- | :--- | :--- |
| `corm_open` | `uint32_t corm_open(file, db, ktype, vtype, mask, flags)` | Open an in-memory or auto-persisted map |
| `corm_put` | `uint32_t corm_put(hd, key_ptr, val_ptr)` | Insert or update a key-value pair |
| `corm_get` | `const void *corm_get(hd, key_ptr)` | Retrieve a value by key (`NULL` if absent) |
| `corm_assoc` | `void corm_assoc(sec_hd, prim_hd, cb, ud)` | Automatically maintain a secondary index |
| `corm_iter` | `uint32_t corm_iter(hd, key_ptr, flags)` | Iterate all entries or scan a sorted range |
| `rec_set_new` | `rec_set_t *rec_set_new(void)` | Allocate arena for cross-axis recall set joins |

👉 *For custom type registration, multi-value keys, ID managers, and the complete 40+ function reference, see [docs/API.md](docs/API.md).*

---

## Design Philosophy: Constraints as Superpowers

* **Zero Lock Contention**: `libcorm` is deliberately single-threaded to achieve optimal L1/L2 CPU cache residency and microsecond-level query latencies without mutex overhead. For multi-core systems, shard maps per worker thread or front with a non-blocking event loop.
* **Predictable Memory Footprint**: Compact arena and mask-based doubling allocation prevents memory fragmentation during long-running daemon operations.
* **Unified Recall Composition (`rec.h`)**: Standardizes search queries into a domain-free `"filter by axis → join → rank"` pipeline over uniform 32-bit refs.

---

## Installation & Build

### Prebuilt Packages
Prebuilt packages are distributed from tty.pt for Linux (APT / Alpine / Arch / Fedora-RHEL), macOS (Homebrew), Windows (winget / MSYS2), and OpenBSD. Follow the [installation instructions](https://github.com/tty-pt/ci/blob/main/docs/install.md) and use **libcorm** as the package name.

### Build from Source
```sh
make                  # builds lib/libcorm.so, bin/corm, and test suite
make test             # run unit, CLI, and recall-kernel tests
sudo make install     # install headers and library to system
```

## License

BSD 2-Clause License. Copyright (c) 2025, tty-pt. See `LICENSE`.
