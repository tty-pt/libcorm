import { loadWasmModule, loadNativeAddon, getSubstrate } from "@tty-pt/stele";

const nativeBinding = loadNativeAddon(import.meta.url, "corm");
let wasmInstance = null;

/**
 * Plug-and-play WebAssembly loader for @tty-pt/corm via Stele substrate
 */
export function getCormWasm() {
  if (wasmInstance) return wasmInstance;
  wasmInstance = loadWasmModule(new URL("./corm.wasm", import.meta.url));
  return wasmInstance;
}

/**
 * CandidateSet: Fast set algebra operations strictly wrapping C cormr_set_t via WebAssembly
 */
export class CandidateSet {
  constructor(handle, options = {}) {
    if (handle && typeof handle === "object") {
      this.handle = handle;
    } else if (handle && typeof handle === "number") {
      this.wasmHandle = handle;
    } else if (options.wasm || !nativeBinding) {
      const wasm = getCormWasm();
      if (!wasm || !wasm.exports.cormr_set_new) {
        throw new Error("WebAssembly substrate for @tty-pt/corm is unavailable");
      }
      this.wasmHandle = wasm.exports.cormr_set_new();
    } else if (nativeBinding && nativeBinding.setCreate) {
      this.handle = nativeBinding.setCreate();
    } else {
      const wasm = getCormWasm();
      if (!wasm || !wasm.exports.cormr_set_new) {
        throw new Error("WebAssembly substrate for @tty-pt/corm is unavailable");
      }
      this.wasmHandle = wasm.exports.cormr_set_new();
    }
  }

  static wasm() {
    return new CandidateSet(null, { wasm: true });
  }

  push(ref) {
    if (nativeBinding && this.handle) {
      nativeBinding.setPush(this.handle, ref);
    } else {
      const wasm = getCormWasm();
      wasm.exports.cormr_set_push(this.wasmHandle, ref);
    }
    return this;
  }

  seal() {
    if (nativeBinding && this.handle) {
      nativeBinding.setSeal(this.handle);
    } else {
      const wasm = getCormWasm();
      wasm.exports.cormr_set_seal(this.wasmHandle);
    }
    return this;
  }

  toArray() {
    if (nativeBinding && this.handle) {
      return nativeBinding.setToArray(this.handle);
    }
    const wasm = getCormWasm();
    const count = wasm.exports.cormr_set_count(this.wasmHandle);
    const ptr = wasm.exports.cormr_set_at(this.wasmHandle);
    const mem = getSubstrate().memory.buffer;
    const arr = new Uint32Array(mem, ptr, count);
    return Array.from(arr);
  }

  /**
   * Returns count of elements in candidate set
   * @returns {number}
   */
  count() {
    if (nativeBinding && this.handle) {
      return this.toArray().length;
    }
    const wasm = getCormWasm();
    return wasm.exports.cormr_set_count(this.wasmHandle);
  }

  /**
   * Returns raw WASM memory pointer to contiguous uint32_t array for zero-copy cross-module chaining
   * @returns {number} 32-bit linear address pointer
   */
  rawPointer() {
    if (this.wasmHandle) {
      const wasm = getCormWasm();
      return wasm.exports.cormr_set_at(this.wasmHandle);
    }
    return 0;
  }

  /**
   * Returns zero-copy Uint32Array view directly over WASM linear memory
   * @returns {Uint32Array}
   */
  toUint32Array() {
    if (this.wasmHandle) {
      const wasm = getCormWasm();
      const count = wasm.exports.cormr_set_count(this.wasmHandle);
      const ptr = wasm.exports.cormr_set_at(this.wasmHandle);
      const mem = getSubstrate().memory.buffer;
      return new Uint32Array(mem, ptr, count);
    }
    return new Uint32Array(this.toArray());
  }

  /**
   * Appends elements from raw uint32_t memory pointer in shared WASM memory
   * @param {number} ptr
   * @param {number} count
   */
  addFromRaw(ptr, count) {
    if (!ptr || count <= 0) return this;
    const mem = getSubstrate().memory.buffer;
    const view = new Uint32Array(mem, ptr, count);
    for (let i = 0; i < count; i++) {
      this.push(view[i]);
    }
    return this;
  }

  intersect(other) {
    if (nativeBinding && this.handle && other.handle) {
      const res = nativeBinding.setIntersect(this.handle, other.handle);
      return new CandidateSet(res);
    }
    const wasm = getCormWasm();
    const res = new CandidateSet(null, { wasm: Boolean(this.wasmHandle) });
    wasm.exports.cormr_set_intersect(res.wasmHandle, this.wasmHandle, other.wasmHandle);
    return res;
  }

  union(other) {
    if (nativeBinding && this.handle && other.handle) {
      const res = nativeBinding.setUnion(this.handle, other.handle);
      return new CandidateSet(res);
    }
    const wasm = getCormWasm();
    const res = new CandidateSet(null, { wasm: Boolean(this.wasmHandle) });
    wasm.exports.cormr_set_union(res.wasmHandle, this.wasmHandle, other.wasmHandle);
    return res;
  }

  subtract(other) {
    if (nativeBinding && this.handle && other.handle) {
      const res = nativeBinding.setSubtract(this.handle, other.handle);
      return new CandidateSet(res);
    }
    const wasm = getCormWasm();
    const res = new CandidateSet(null, { wasm: Boolean(this.wasmHandle) });
    wasm.exports.cormr_set_subtract(res.wasmHandle, this.wasmHandle, other.wasmHandle);
    return res;
  }

  free() {
    if (nativeBinding && this.handle) {
      nativeBinding.setFree(this.handle);
      this.handle = null;
    } else if (this.wasmHandle) {
      const wasm = getCormWasm();
      wasm.exports.cormr_set_free(this.wasmHandle);
      this.wasmHandle = null;
    }
  }
}

/**
 * MinHeapRanker: Bounded streaming top-K ranking strictly wrapping C cormr_rank_t via WebAssembly
 */
export class MinHeapRanker {
  constructor(topK = 20, minScore = 0.0, options = {}) {
    this.topK = topK;
    this.minScore = minScore;
    if (!options.wasm && nativeBinding && nativeBinding.rankCreate) {
      this.handle = nativeBinding.rankCreate(topK, minScore);
    } else {
      const wasm = getCormWasm();
      if (!wasm || !wasm.exports.cormr_rank_new) {
        throw new Error("WebAssembly substrate for @tty-pt/corm is unavailable");
      }
      this.wasmHandle = wasm.exports.cormr_rank_new(topK, minScore);
    }
  }

  static wasm(topK = 20, minScore = 0.0) {
    return new MinHeapRanker(topK, minScore, { wasm: true });
  }

  push(ref, score) {
    if (nativeBinding && this.handle) {
      nativeBinding.rankPush(this.handle, ref, score);
    } else {
      const wasm = getCormWasm();
      wasm.exports.cormr_rank_push(this.wasmHandle, ref, score);
    }
    return this;
  }

  sorted() {
    if (nativeBinding && this.handle) {
      return nativeBinding.rankSorted(this.handle);
    }
    const wasm = getCormWasm();
    const { malloc, free, cormr_rank_sorted } = wasm.exports;
    const refsPtr = malloc(this.topK * 4);
    const scoresPtr = malloc(this.topK * 4);
    const count = cormr_rank_sorted(this.wasmHandle, refsPtr, scoresPtr);
    const mem = getSubstrate().memory.buffer;
    const refs = new Uint32Array(mem, refsPtr, count);
    const scores = new Float32Array(mem, scoresPtr, count);
    const results = [];
    for (let i = 0; i < count; i++) {
      results.push({ ref: refs[i], score: scores[i] });
    }
    free(refsPtr);
    free(scoresPtr);
    return results;
  }

  free() {
    if (nativeBinding && this.handle) {
      nativeBinding.rankFree(this.handle);
      this.handle = null;
    } else if (this.wasmHandle) {
      const wasm = getCormWasm();
      wasm.exports.cormr_rank_free(this.wasmHandle);
      this.wasmHandle = null;
    }
  }
}

/**
 * CormDb: Multi-axis key-value storage engine
 */
export class CormDb {
  constructor(filePath, dbName = "default") {
    this.filePath = filePath || "";
    this.dbName = dbName;
    this.axes = new Map();
    this.handle = null;
    this._memStore = new Map();

    if (nativeBinding && nativeBinding.open) {
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

    if (typeof process !== "undefined" && process.versions?.node) {
      try {
        const getModule = globalThis.process?.getBuiltinModule;
        const req = getModule ? getModule("module").createRequire(import.meta.url) : null;
        if (req) {
          const candidates = [
            "../build/Release/joint_node.node",
            "../build/Release/duna_joint.node",
            "./joint_node.node",
            "./duna_joint.node"
          ];
          for (const c of candidates) {
            try {
              this._jointAddon = req(c);
              if (this._jointAddon) {
                this.handle = this._jointAddon.init ? this._jointAddon.init(this.filePath) : (this._jointAddon.initDb ? this._jointAddon.initDb(this.filePath) : null);
                if (this.handle) break;
              }
            } catch {}
          }
        }
      } catch {}
    }
  }

  registerInterval(ref, resourceId, start, end) {
    this.resourceMapping.set(ref, resourceId);
    if (this._jointAddon && this.handle) {
      if (this._jointAddon.record) {
        this._jointAddon.record(this.handle, ref, start, end);
      } else if (this._jointAddon.recordInterval) {
        this._jointAddon.recordInterval(this.handle, ref, start, end);
      }
    } else {
      this.intervals.push({ ref, resourceId, start, end });
    }
  }

  fill(params) {
    const set = new CandidateSet();
    if (this._jointAddon && this.handle) {
      const hits = this._jointAddon.intersect
        ? this._jointAddon.intersect(this.handle, params.start, params.end)
        : (this._jointAddon.queryOverlaps ? this._jointAddon.queryOverlaps(this.handle, params.start, params.end) : []);
      for (const item of hits) {
        const ref = typeof item === "number" ? item : (item.who !== undefined ? item.who : item.ref);
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
      if (this._jointAddon.close) {
        this._jointAddon.close(this.handle);
      } else if (this._jointAddon.closeDb) {
        this._jointAddon.closeDb(this.handle);
      }
      this.handle = null;
    }
    this.intervals = [];
    this.resourceMapping.clear();
  }
}

/**
 * Dense Vector Embedding Axis (wrapping libsepal)
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
    if (!vec || !params.queryVector) return 0.0;
    if (vec.length !== params.queryVector.length) return 0.0;

    let dot = 0.0;
    let normA = 0.0;
    let normB = 0.0;
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
 * Lexical Search Axis (wrapping liblemma)
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
        if (f.join === "OR") {
          runningSet = runningSet.union(s);
        } else if (f.join === "NOT") {
          runningSet = runningSet.subtract(s);
        } else {
          runningSet = runningSet.intersect(s);
        }
      }
    }

    const candidateRefs = runningSet ? runningSet.toArray() : [];
    const ranker = new MinHeapRanker(this.limitTopK, this.scoreThreshold);

    for (const ref of candidateRefs) {
      let totalScore = 0.0;
      let totalWeight = 0.0;

      for (const r of this.rankers) {
        const axis = this.db.getAxis(r.axis);
        if (axis && axis.rank) {
          const score = await axis.rank(r.params, ref);
          totalScore += score * r.weight;
          totalWeight += r.weight;
        }
      }

      const finalScore = totalWeight > 0 ? totalScore / totalWeight : 1.0;
      ranker.push(ref, finalScore);
    }

    const topItems = ranker.sorted();
    return topItems.map((item) => ({
      ref: item.ref,
      score: item.score,
      data: this.db.get(String(item.ref))
    }));
  }
}
