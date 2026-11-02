<p align="center">
  <img src="https://raw.githubusercontent.com/tty-pt/libcorm/main/assets/logo.png" alt="@tty-pt/corm logo" width="220" />
</p>

# @tty-pt/corm

[![npm version](https://img.shields.io/badge/npm-v0.2.0-CB3837?logo=npm)](#)
[![License: BSD-2-Clause](https://img.shields.io/badge/License-BSD--2--Clause-blue)](#)
[![TypeScript Ready](https://img.shields.io/badge/TypeScript-Ready-3178C6?logo=typescript)](#)
[![Node.js Native](https://img.shields.io/badge/Node.js-N--API-339933?logo=node.js)](#)
[![WebAssembly](https://img.shields.io/badge/WebAssembly-Reactor-654FF0?logo=webassembly)](#)
[![Zero Dependencies](https://img.shields.io/badge/dependencies-0-success)](#)

> Universal Node.js and Browser bindings for **`libcorm`** and the **Corm Recall Kernel (`cormr`)**. Features persistent memory-mapped storage, candidate set algebra, streaming min-heap ranking, and multi-axis composable queries.

---

## 1. Quickstart

```bash
npm install @tty-pt/corm
```

```typescript
import { CormDb, CandidateSet, MinHeapRanker } from "@tty-pt/corm";

// 1. Persistent key-value storage (Node mmap or Browser in-memory/OPFS)
const db = new CormDb("./data.corm", "app_store");
db.put("member:101", { name: "Alice", role: "admin" });
const member = db.get("member:101");
console.log(member); // { name: "Alice", role: "admin" }

// 2. Ultra-fast Candidate Set Algebra (cormr_set_t)
const setA = new CandidateSet();
setA.push(10).push(20).push(30).seal();

const setB = new CandidateSet();
setB.push(20).push(30).push(40).seal();

const intersection = setA.intersect(setB);
console.log(intersection.toArray()); // [20, 30]

// 3. Streaming Bounded Min-Heap Ranker (cormr_rank_t)
const ranker = new MinHeapRanker(3 /* topK */, 0.2 /* minScore */);
ranker.push(101, 0.85);
ranker.push(102, 0.95);
ranker.push(103, 0.40);
console.log(ranker.sorted()); // [{ ref: 102, score: 0.95 }, { ref: 101, score: 0.85 }, ...]

db.close();
```

---

## 2. Dual-Engine Runtime: Native Speed & Browser Portability

`@tty-pt/corm` operates on an infallible 3-tier runtime engine:
1. **Node.js Native (N-API)**: Executes native C99 compiled binaries with direct OS memory mapping (`mmap(2)`), zero copy, and multi-gigabyte storage throughput.
2. **Browser & Edge WebAssembly**: When running in the browser, Cloudflare Workers, or environments without a C compiler, it seamlessly falls back to the pre-compiled `dist/corm.wasm` reactor with zero native dependencies.

---

## 3. Composable Multi-Axis Queries

The Corm Recall Kernel coordinates orthogonal domain axes (`joint` for time, `islet` for space, `lemma` for text, `sepal` for vectors) into unified boolean filtering and ranking:

```typescript
import { CormDb, JointTemporalAxis, SepalVectorAxis } from "@tty-pt/corm";

const db = new CormDb("./workspace.corm");
const temporal = new JointTemporalAxis();
const vector = new SepalVectorAxis();

db.registerAxis(temporal);
db.registerAxis(vector);

// Compose: Filter by temporal window AND rank by dense vector cosine similarity
const hits = await db
  .query()
  .filter("temporal", { start: 1793405000, end: 1793415000 })
  .rank("vector", { queryVector: [0.12, 0.85, 0.44] })
  .topK(5)
  .execute();
```

---

## 4. C Development Headers via NPM

This package includes the canonical C development headers in `include/` and `corm.pc`:
```makefile
# Include in your C Makefile without system installation:
CFLAGS += -I./node_modules/@tty-pt/corm/include
LDLIBS += -lcorm
```

---

## 5. Building from Source

```bash
make npm-build    # Compiles native addon (build/Release/) and WASM reactor (dist/)
make npm-test     # Runs verification test suite
```

## License

BSD 2-Clause License. Copyright (c) 2026, tty-pt.
