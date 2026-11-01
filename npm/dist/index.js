import { createRequire } from "node:module";
import path from "node:path";

let nativeBinding = null;
try {
  const req = createRequire(import.meta.url);
  // 1. Local build in package
  try {
    nativeBinding = req("../build/Release/corm_node.node");
  } catch {}
  // 2. Prebuilt platform package
  if (!nativeBinding && typeof process !== "undefined" && process.platform && process.arch) {
    try {
      nativeBinding = req(`@ttypt/corm-${process.platform}-${process.arch}`);
    } catch {}
  }
  // 3. Project cwd build
  if (!nativeBinding) {
    try {
      nativeBinding = req(path.join(process.cwd(), "build/Release/corm_node.node"));
    } catch {}
  }
} catch {
  // Native addon not available; in-memory / WASM fallback
}

function getSharedWasm() {
  if (globalThis.__ttypt_shared_wasm__) {
    return globalThis.__ttypt_shared_wasm__;
  }
  try {
    const req = createRequire(import.meta.url);
    const qsysPaths = [
      path.join(path.dirname(import.meta.url.replace("file://", "")), "../../libqsys/npm/dist/index.js"),
      path.join(process.cwd(), "node_modules/@ttypt/qsys/dist/index.js"),
      "@ttypt/qsys"
    ];
    for (const p of qsysPaths) {
      try {
        const mod = req(p);
        if (mod && mod.getSharedWasm) {
          return mod.getSharedWasm();
        }
      } catch {}
    }
  } catch {}
  return null;
}

/**
 * CandidateSet: Fast set algebra operations wrapping cormr_set_t
 */
export class CandidateSet {
  constructor(handle) {
    if (handle) {
      this.handle = handle;
    } else if (nativeBinding) {
      this.handle = nativeBinding.setCreate();
    } else {
      const wasm = getSharedWasm();
      if (wasm) {
        this.wasmHandle = wasm.cormr_set_new();
      } else {
        this._inMemory = new Set();
      }
    }
  }

  push(ref) {
    if (nativeBinding && this.handle) {
      nativeBinding.setPush(this.handle, ref);
    } else if (this.wasmHandle !== undefined) {
      const wasm = getSharedWasm();
      if (wasm) wasm.cormr_set_push(this.wasmHandle, ref);
    } else if (this._inMemory) {
      this._inMemory.add(ref);
    }
    return this;
  }

  seal() {
    if (nativeBinding && this.handle) {
      nativeBinding.setSeal(this.handle);
    } else if (this.wasmHandle !== undefined) {
      const wasm = getSharedWasm();
      if (wasm) wasm.cormr_set_seal(this.wasmHandle);
    }
    return this;
  }

  toArray() {
    if (nativeBinding && this.handle) {
      return nativeBinding.setToArray(this.handle);
    } else if (this.wasmHandle !== undefined) {
      const wasm = getSharedWasm();
      if (wasm) {
        const count = wasm.cormr_set_count(this.wasmHandle);
        const ptr = wasm.cormr_set_at(this.wasmHandle);
        const arr = new Uint32Array(wasm.memory.buffer, ptr, count);
        return Array.from(arr);
      }
    } else if (this._inMemory) {
      return Array.from(this._inMemory);
    }
    return [];
  }

  intersect(other) {
    if (nativeBinding && this.handle && other.handle) {
      const res = nativeBinding.setIntersect(this.handle, other.handle);
      return new CandidateSet(res);
    }
    const res = new CandidateSet();
    const otherArr = other.toArray();
    for (const item of this.toArray()) {
      if (otherArr.includes(item)) res.push(item);
    }
    res.seal();
    return res;
  }

  union(other) {
    if (nativeBinding && this.handle && other.handle) {
      const res = nativeBinding.setUnion(this.handle, other.handle);
      return new CandidateSet(res);
    }
    const res = new CandidateSet();
    for (const item of this.toArray()) res.push(item);
    for (const item of other.toArray()) res.push(item);
    res.seal();
    return res;
  }

  subtract(other) {
    if (nativeBinding && this.handle && other.handle) {
      const res = nativeBinding.setSubtract(this.handle, other.handle);
      return new CandidateSet(res);
    }
    const res = new CandidateSet();
    const otherArr = other.toArray();
    for (const item of this.toArray()) {
      if (!otherArr.includes(item)) res.push(item);
    }
    res.seal();
    return res;
  }
}

/**
 * MinHeapRanker: Bounded streaming min-heap ranker wrapping cormr_rank_t
 */
export class MinHeapRanker {
  constructor(topK = 10, minScore = 0.0) {
    this.topK = topK;
    this.minScore = minScore;
    if (nativeBinding) {
      this.handle = nativeBinding.rankCreate(topK, minScore);
    } else {
      this._items = [];
    }
  }

  push(ref, score) {
    if (score < this.minScore) return;
    if (nativeBinding && this.handle) {
      nativeBinding.rankPush(this.handle, ref, score);
    } else {
      this._items.push({ ref, score });
      this._items.sort((a, b) => b.score - a.score);
      if (this._items.length > this.topK) {
        this._items.length = this.topK;
      }
    }
  }

  sorted() {
    if (nativeBinding && this.handle) {
      return nativeBinding.rankSorted(this.handle, this.topK);
    }
    return this._items || [];
  }
}

/**
 * CormDb: Persistent key-value / record storage
 */
export class CormDb {
  constructor(filePath, dbName = "default") {
    this.filePath = filePath || "";
    this.dbName = dbName;
    this.axes = new Map();
    this.handle = null;
    this._memStore = new Map();

    if (nativeBinding) {
      this.handle = nativeBinding.open(this.filePath, this.dbName);
    }
  }

  registerAxis(axis) {
    this.axes.set(axis.name, axis);
    return this;
  }

  getAxis(name) {
    return this.axes.get(name);
  }

  put(key, value) {
    const serialized = typeof value === "string" ? value : JSON.stringify(value);
    if (nativeBinding && this.handle) {
      return nativeBinding.put(this.handle, key, serialized);
    }
    this._memStore.set(key, serialized);
    return 1;
  }

  get(key) {
    let raw = null;
    if (nativeBinding && this.handle) {
      raw = nativeBinding.get(this.handle, key);
    } else {
      raw = this._memStore.get(key) || null;
    }
    if (!raw) return null;
    try {
      return JSON.parse(raw);
    } catch {
      return raw;
    }
  }

  del(key) {
    if (nativeBinding && this.handle) {
      return nativeBinding.del(this.handle, key);
    }
    return this._memStore.delete(key);
  }

  save() {
    if (nativeBinding && this.handle) {
      return nativeBinding.save();
    }
    return true;
  }

  close() {
    if (nativeBinding && this.handle) {
      nativeBinding.close(this.handle);
      this.handle = null;
    }
    this._memStore.clear();
  }

  query() {
    return new CormQuery(this);
  }
}

/**
 * Native Temporal Interval Axis (wrapping libjoint)
 */
export class JointTemporalAxis {
  constructor(filePath = "") {
    this.name = "temporal";
    this.filePath = filePath;
    this.resourceMapping = new Map();
    this.intervals = [];
    this.handle = null;
    this._jointAddon = null;

    try {
      const req = createRequire(import.meta.url);
      // Attempt to load colocated libjoint addon
      const jointPaths = [
        path.join(path.dirname(import.meta.url.replace("file://", "")), "../../../libjoint/npm/build/Release/joint_node.node"),
        path.join(process.cwd(), "node_modules/@ttypt/joint/build/Release/joint_node.node"),
        path.join(process.cwd(), "build/Release/joint_node.node")
      ];
      for (const p of jointPaths) {
        try {
          this._jointAddon = req(p);
          if (this._jointAddon) {
            this.handle = this._jointAddon.init(this.filePath);
            break;
          }
        } catch {}
      }
    } catch {
      // In-memory fallback
    }
  }

  registerInterval(ref, resourceId, start, end) {
    this.resourceMapping.set(ref, resourceId);
    if (this._jointAddon && this.handle) {
      this._jointAddon.record(this.handle, ref, start, end);
    } else {
      this.intervals.push({ ref, resourceId, start, end });
    }
  }

  fill(params) {
    const set = new CandidateSet();
    if (this._jointAddon && this.handle) {
      const hits = this._jointAddon.intersect(this.handle, params.start, params.end);
      for (const ref of hits) {
        if (params.resourceId !== undefined) {
          const res = this.resourceMapping.get(ref);
          if (res !== undefined && res !== params.resourceId) continue;
        }
        set.push(ref);
      }
    } else {
      for (const item of this.intervals) {
        if (params.start < item.end && params.end > item.start) {
          if (params.resourceId === undefined || item.resourceId === params.resourceId) {
            set.push(item.ref);
          }
        }
      }
    }
    set.seal();
    return set;
  }

  close() {
    if (this._jointAddon && this.handle) {
      this._jointAddon.close(this.handle);
      this.handle = null;
    }
    this.intervals = [];
  }
}

/**
 * Dense Vector Axis (wrapping vector cosine similarity)
 */
export class SepalVectorAxis {
  constructor() {
    this.name = "vector";
    this.vectors = new Map();
  }

  registerVector(ref, vector) {
    this.vectors.set(ref, vector);
  }

  rank(params, ref) {
    const vec = this.vectors.get(ref);
    if (!vec || !params.queryVector || vec.length !== params.queryVector.length) return 0.0;

    let dot = 0.0, normA = 0.0, normB = 0.0;
    for (let i = 0; i < vec.length; i++) {
      dot += vec[i] * params.queryVector[i];
      normA += vec[i] * vec[i];
      normB += params.queryVector[i] * params.queryVector[i];
    }
    if (normA === 0 || normB === 0) return 0.0;
    return dot / (Math.sqrt(normA) * Math.sqrt(normB));
  }
}

/**
 * Lexical Search Axis (wrapping Unicode case folding & token matching)
 */
export class LemmaLexicalAxis {
  constructor() {
    this.name = "lexical";
    this.documents = new Map();
  }

  registerText(ref, text) {
    const tokens = text
      .toLowerCase()
      .replace(/[^\w\s\u00C0-\u017F]/g, " ")
      .split(/\s+/)
      .filter(Boolean);
    this.documents.set(ref, tokens);
  }

  fill(params) {
    const set = new CandidateSet();
    const queryTokens = (params.term || "").toLowerCase().split(/\s+/).filter(Boolean);

    for (const [ref, docTokens] of this.documents.entries()) {
      if (queryTokens.some((q) => docTokens.includes(q))) {
        set.push(ref);
      }
    }
    set.seal();
    return set;
  }

  rank(params, ref) {
    const tokens = this.documents.get(ref);
    if (!tokens || tokens.length === 0) return 0.0;

    const queryTokens = (params.term || "").toLowerCase().split(/\s+/).filter(Boolean);
    let matches = 0;
    for (const q of queryTokens) {
      if (tokens.includes(q)) matches++;
    }
    return matches / queryTokens.length;
  }
}

/**
 * CormQuery: Composable multi-axis query builder
 */
export class CormQuery {
  constructor(db) {
    this.db = db;
    this.filters = [];
    this.rankers = [];
    this.limitTopK = 20;
    this.scoreThreshold = 0.0;
  }

  filter(axis, params, join = "AND") {
    this.filters.push({ axis, params, join });
    return this;
  }

  rank(axis, params, weight = 1.0) {
    this.rankers.push({ axis, params, weight });
    return this;
  }

  topK(k) {
    this.limitTopK = k;
    return this;
  }

  minScore(score) {
    this.scoreThreshold = score;
    return this;
  }

  async execute() {
    let runningSet = null;

    for (const f of this.filters) {
      const axis = this.db.getAxis(f.axis);
      if (!axis || !axis.fill) continue;

      const candidates = await axis.fill(f.params);
      let s = null;
      if (candidates instanceof CandidateSet) {
        s = candidates;
      } else if (Array.isArray(candidates)) {
        s = new CandidateSet();
        for (const ref of candidates) s.push(ref);
        s.seal();
      }

      if (!s) continue;

      if (!runningSet) {
        runningSet = s;
      } else {
        if (f.join === "AND") runningSet = runningSet.intersect(s);
        else if (f.join === "OR") runningSet = runningSet.union(s);
        else if (f.join === "NOT") runningSet = runningSet.subtract(s);
      }
    }

    const candidateRefs = runningSet ? runningSet.toArray() : [];
    const ranker = new MinHeapRanker(this.limitTopK, this.scoreThreshold);

    for (const ref of candidateRefs) {
      let totalScore = 0.0;
      let matchedAny = false;

      for (const r of this.rankers) {
        const axis = this.db.getAxis(r.axis);
        if (axis && axis.rank) {
          const score = await axis.rank(r.params, ref);
          totalScore += score * r.weight;
          matchedAny = true;
        }
      }

      if (!matchedAny) totalScore = 1.0;
      ranker.push(ref, totalScore);
    }

    return ranker.sorted();
  }
}
