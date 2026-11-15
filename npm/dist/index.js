import {
  loadWasmModule,
  loadNativeAddon,
  getSubstrate,
  resolveAxisPrebuildDirs
} from "@tty-pt/stele";

const nativeBinding = loadNativeAddon(import.meta.url, "corm");
let wasmInstance = null;

function ensureDefaultAxisPath() {
  if (typeof process === "undefined" || !process.env) return;
  const dirs = [];
  if (process.env.CORM_AXIS_PATH) {
    for (const part of process.env.CORM_AXIS_PATH.split(":")) {
      if (part) dirs.push(part);
    }
  }
  try {
    const prebuildDirs = resolveAxisPrebuildDirs(import.meta.url);
    for (const d of prebuildDirs) {
      if (d && !dirs.includes(d)) dirs.push(d);
    }
  } catch {}
  try {
    const getModule = globalThis.process?.getBuiltinModule;
    const fs = getModule ? getModule("fs") : null;
    const path = getModule ? getModule("path") : null;
    const url = getModule ? getModule("url") : null;
    if (fs && path && url) {
      const baseDir = path.dirname(url.fileURLToPath(import.meta.url));
      const siblingAxes = ["liblemma", "libsepal", "libjoint", "libislet"];
      for (const sibling of siblingAxes) {
        const candidate = path.resolve(baseDir, "..", "..", "..", sibling, "lib");
        if (fs.existsSync(candidate) && !dirs.includes(candidate)) {
          dirs.push(candidate);
        }
      }
    }
  } catch {}
  if (dirs.length > 0 && !process.env.CORM_DEFAULT_AXIS_PATH) {
    process.env.CORM_DEFAULT_AXIS_PATH = dirs.join(":");
  }
}

ensureDefaultAxisPath();

/**
 * Plug-and-play WebAssembly loader for @tty-pt/corm via Stele substrate
 */
export function getCormWasm() {
  if (wasmInstance) return wasmInstance;
  wasmInstance = loadWasmModule(new URL("./corm.wasm", import.meta.url));
  return wasmInstance;
}

/**
 * HTOK v4 Binary Envelope Pack (corm_htok_pack)
 * @param {string | Record<string, any>} jsonInput JSON string or object
 * @returns {Uint8Array}
 */
export function htokPack(jsonInput) {
  if (!nativeBinding || typeof nativeBinding.htokPack !== "function") {
    throw new Error("Native addon for @tty-pt/corm with htokPack is required");
  }
  const str = typeof jsonInput === "string" ? jsonInput : JSON.stringify(jsonInput);
  return nativeBinding.htokPack(str);
}

/**
 * HTOK v4 Binary Envelope Unpack (corm_htok_unpack)
 * @param {Uint8Array} bytes Packed HTOK v4 binary buffer
 * @returns {string} Unpacked JSON string
 */
export function htokUnpack(bytes) {
  if (!nativeBinding || typeof nativeBinding.htokUnpack !== "function") {
    throw new Error("Native addon for @tty-pt/corm with htokUnpack is required");
  }
  const u8 = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  return nativeBinding.htokUnpack(u8);
}

/**
 * Execute an in-process Corm protocol command (PING, LIST, GET, GET_JSON, PUT, PUT_JSON, DEL, DEL_ALL, QUERY)
 * @param {string} op
 * @param {string} filespec
 * @param {string | number} [arg2]
 * @param {string | Uint8Array} [payload]
 * @param {Record<string, string>} [env]
 * @returns {{ code: number, stdout: string, stderr: string, stdoutBytes?: Uint8Array }}
 */
export function execCormCommand(op, filespec, arg2 = "", payload = "", env = undefined) {
  if (!nativeBinding || typeof nativeBinding.execCommand !== "function") {
    throw new Error("Native addon for @tty-pt/corm with execCommand is required");
  }
  ensureDefaultAxisPath();
  const defaultAxisPath = process.env.CORM_AXIS_PATH || process.env.CORM_DEFAULT_AXIS_PATH || "";
  const callerAxisPath = env && typeof env.CORM_AXIS_PATH === "string" ? env.CORM_AXIS_PATH : "";
  const mergedAxisDirs = [];
  for (const raw of [callerAxisPath, defaultAxisPath]) {
    if (!raw) continue;
    for (const part of raw.split(":")) {
      if (part && !mergedAxisDirs.includes(part)) mergedAxisDirs.push(part);
    }
  }
  const envToPass = {
    ...(env || {}),
    ...(mergedAxisDirs.length > 0 ? { CORM_AXIS_PATH: mergedAxisDirs.join(":") } : {})
  };
  const bodyToPass =
    payload instanceof Uint8Array
      ? payload
      : payload !== undefined && payload !== null
      ? String(payload)
      : "";
  return nativeBinding.execCommand(
    String(op),
    String(filespec),
    arg2 !== undefined && arg2 !== null ? String(arg2) : "",
    bodyToPass,
    envToPass
  );
}

/**
 * Close and flush a cached in-process Corm engine instance for a filespec
 * @param {string} [filespec]
 * @returns {boolean}
 */
export function closeCormEngine(filespec) {
  if (!nativeBinding || typeof nativeBinding.closeEngine !== "function") {
    return false;
  }
  if (filespec === undefined || filespec === null) {
    return Boolean(nativeBinding.closeEngine());
  }
  return Boolean(nativeBinding.closeEngine(String(filespec)));
}

/**
 * CormEngine: High-level wrapper around in-process Corm file execution
 */
export class CormEngine {
  constructor(filespec, env = undefined) {
    if (!filespec) {
      throw new Error("CormEngine requires a filespec");
    }
    this.filespec = filespec;
    this.env = env;
  }

  ping() {
    return execCormCommand("PING", this.filespec, "", "", this.env);
  }

  list() {
    return execCormCommand("LIST", this.filespec, "", "", this.env);
  }

  get(key = ".", json = false) {
    return execCormCommand(json ? "GET_JSON" : "GET", this.filespec, String(key), "", this.env);
  }

  put(key = ".", value = "", json = false) {
    const payload =
      value instanceof Uint8Array || typeof value === "string"
        ? value
        : JSON.stringify(value);
    return execCormCommand(json ? "PUT_JSON" : "PUT", this.filespec, String(key), payload, this.env);
  }

  del(key) {
    return execCormCommand("DEL", this.filespec, String(key), "", this.env);
  }

  delAll(key = "all") {
    return execCormCommand("DEL_ALL", this.filespec, String(key), "", this.env);
  }

  query(exprLine, optionsLines = [], topK = 0) {
    const bodyParts = [String(exprLine || "")];
    if (Array.isArray(optionsLines)) {
      for (const line of optionsLines) {
        if (line) bodyParts.push(String(line));
      }
    } else if (typeof optionsLines === "string" && optionsLines.length > 0) {
      bodyParts.push(optionsLines);
    }
    return execCormCommand("QUERY", this.filespec, String(topK || 0), bodyParts.join("\n"), this.env);
  }

  close() {
    return closeCormEngine(this.filespec);
  }
}

/**
 * CandidateSet: Fast set algebra operations strictly wrapping C cormr_set_t via N-API or WebAssembly
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

  count() {
    if (nativeBinding && this.handle) {
      return this.toArray().length;
    }
    const wasm = getCormWasm();
    return wasm.exports.cormr_set_count(this.wasmHandle);
  }

  rawPointer() {
    if (this.wasmHandle) {
      const wasm = getCormWasm();
      return wasm.exports.cormr_set_at(this.wasmHandle);
    }
    return 0;
  }

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
 * MinHeapRanker: Bounded streaming top-K ranking strictly wrapping C cormr_rank_t via N-API or WebAssembly
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
 * CormDb: Multi-axis key-value storage engine strictly backed by C libcorm
 */
export class CormDb {
  constructor(filePath, dbName = "default", options = {}) {
    if (!nativeBinding || !nativeBinding.open) {
      throw new Error("Native addon for @tty-pt/corm is unavailable; CormDb requires native libcorm");
    }
    this.filePath = filePath || "";
    this.dbName = dbName;
    this.ktype = options.ktype || "s";
    this.vtype = options.vtype || "s";
    this.axes = new Map();
    this.handle = nativeBinding.open(this.filePath, this.dbName, this.ktype, this.vtype);
    if (!this.handle) {
      throw new Error(`Failed to open CormDb '${this.dbName}' at '${this.filePath}'`);
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
    if (!this.handle) {
      throw new Error("CormDb is closed");
    }
    let valToPass = value;
    if (this.vtype === "s") {
      valToPass = typeof value === "string" ? value : JSON.stringify(value);
    }
    return nativeBinding.put(this.handle, key, valToPass);
  }

  get(key) {
    if (!this.handle) {
      throw new Error("CormDb is closed");
    }
    const raw = nativeBinding.get(this.handle, key);
    if (raw === null || raw === undefined) return null;
    if (this.vtype === "s" && typeof raw === "string") {
      try {
        return JSON.parse(raw);
      } catch {
        return raw;
      }
    }
    return raw;
  }

  del(key) {
    if (!this.handle) {
      throw new Error("CormDb is closed");
    }
    return nativeBinding.del(this.handle, key);
  }

  delAll() {
    if (!this.handle) {
      throw new Error("CormDb is closed");
    }
    return nativeBinding.delAll(this.handle);
  }

  listKeys() {
    if (!this.handle) {
      throw new Error("CormDb is closed");
    }
    return nativeBinding.listKeys(this.handle);
  }

  count() {
    if (!this.handle) {
      throw new Error("CormDb is closed");
    }
    return nativeBinding.count(this.handle);
  }

  save() {
    if (!this.handle) {
      throw new Error("CormDb is closed");
    }
    return nativeBinding.save();
  }

  close() {
    if (this.handle) {
      nativeBinding.close(this.handle);
      this.handle = null;
    }
  }

  query() {
    return new CormQuery(this);
  }
}

let _jointCounter = 0;

function loadAxisAddon(axisName, siblingDirName) {
  let addon = loadNativeAddon(import.meta.url, axisName);
  if (addon) return addon;
  const candidateUrls = [
    `../node_modules/@tty-pt/${axisName}/dist/index.js`,
    `../../${axisName}/dist/index.js`,
    `../../../${siblingDirName}/npm/dist/index.js`
  ];
  for (const rel of candidateUrls) {
    try {
      addon = loadNativeAddon(new URL(rel, import.meta.url), axisName);
      if (addon) return addon;
    } catch {}
  }
  return null;
}

function loadAxisWasm(axisName, siblingDirName) {
  const candidateUrls = [
    `./${axisName}.wasm`,
    `../node_modules/@tty-pt/${axisName}/dist/${axisName}.wasm`,
    `../../${axisName}/dist/${axisName}.wasm`,
    `../../../${siblingDirName}/npm/dist/${axisName}.wasm`
  ];
  for (const rel of candidateUrls) {
    try {
      const wasm = loadWasmModule(new URL(rel, import.meta.url));
      if (wasm && wasm.exports) return wasm;
    } catch {}
  }
  return null;
}

function defaultTempPath(prefix) {
  try {
    const getModule = globalThis.process?.getBuiltinModule;
    const os = getModule ? getModule("os") : null;
    const path = getModule ? getModule("path") : null;
    if (os && path) {
      return path.join(os.tmpdir(), `${prefix}_${process.pid}_${Date.now()}.db`);
    }
  } catch {}
  return `${prefix}_${process.pid}_${Date.now()}.db`;
}

/**
 * Native Temporal Interval Axis strictly wrapping libjoint C/WASM substrate
 */
export class JointTemporalAxis {
  constructor(filePath = "") {
    this.name = "temporal";
    this.filePath = filePath;
    this.handle = null;
    this._jointWasm = null;
    this._jointAddon = loadAxisAddon("joint", "libjoint");
    if (this._jointAddon && this._jointAddon.init) {
      this.handle = this._jointAddon.init(this.filePath);
    } else {
      this._jointWasm = loadAxisWasm("joint", "libjoint");
      if (!this._jointWasm || !this._jointWasm.exports.joint_wasm_record) {
        throw new Error("Native addon or WASM module for libjoint is required by JointTemporalAxis");
      }
      this._jointWasm.exports.joint_wasm_clear();
    }
  }

  registerInterval(ref, resourceId, start, end) {
    const low32 = ref >>> 0;
    const resId = (resourceId || 0) & 0xffff;
    const encodedWho = resId > 0 && low32 <= 0xffff ? ((resId << 16) | low32) >>> 0 : low32;
    if (this.handle) {
      this._jointAddon.record(this.handle, encodedWho, start, end);
    } else if (this._jointWasm) {
      this._jointWasm.exports.joint_wasm_record(encodedWho, BigInt(start), BigInt(end));
    } else {
      throw new Error("JointTemporalAxis is closed");
    }
  }

  fill(params) {
    const set = new CandidateSet();
    let hits = [];
    if (this.handle) {
      hits = this._jointAddon.intersect(this.handle, params.start, params.end);
    } else if (this._jointWasm) {
      const { malloc, free, joint_wasm_intersect } = this._jointWasm.exports;
      const outPtr = malloc(4096);
      const count = joint_wasm_intersect(BigInt(params.start), BigInt(params.end), outPtr, 1024);
      const mem = getSubstrate().memory.buffer;
      hits = Array.from(new Uint32Array(mem, outPtr, count));
      free(outPtr);
    } else {
      throw new Error("JointTemporalAxis is closed");
    }
    for (const item of hits) {
      const encodedWho = (typeof item === "number" ? item : item.who) >>> 0;
      const resId = (encodedWho >>> 16) & 0xffff;
      const ref = resId > 0 ? (encodedWho & 0xffff) : encodedWho;
      if (params.resourceId !== undefined && resId !== (params.resourceId & 0xffff)) {
        continue;
      }
      set.push(ref);
    }
    set.seal();
    return set;
  }

  close() {
    if (this.handle) {
      this._jointAddon.close(this.handle);
      this.handle = null;
    }
    if (this._jointWasm) {
      this._jointWasm.exports.joint_wasm_clear();
      this._jointWasm = null;
    }
  }
}

let _sepalCounter = 0;

/**
 * Dense Vector Embedding Axis strictly wrapping libsepal C substrate
 */
export class SepalVectorAxis {
  constructor(filePath = "", dim = 0) {
    this.name = "vector";
    this._tempFile = !filePath;
    this.filePath = filePath || defaultTempPath(`corm_sepal_${++_sepalCounter}`);
    this.dim = dim;
    this.handle = null;
    this._sepalAddon = loadAxisAddon("sepal", "libsepal");
    if (!this._sepalAddon) {
      throw new Error("Native addon for libsepal is required by SepalVectorAxis");
    }
    if (this.dim > 0) {
      this.handle = this._sepalAddon.open(this.filePath, this.dim);
    }
  }

  registerVector(ref, vector) {
    const f32 = vector instanceof Float32Array ? vector : new Float32Array(vector);
    if (!this.handle) {
      this.dim = f32.length;
      this.handle = this._sepalAddon.open(this.filePath, this.dim);
    }
    this._sepalAddon.store(this.handle, ref >>> 0, f32);
  }

  rank(params, ref) {
    if (!this.handle || !params.queryVector) return 0.0;
    const q = params.queryVector instanceof Float32Array ? params.queryVector : new Float32Array(params.queryVector);
    if (q.length !== this.dim) return 0.0;
    return this._sepalAddon.score(this.handle, ref >>> 0, q);
  }

  close() {
    if (this.handle) {
      this._sepalAddon.close(this.handle);
      this.handle = null;
    }
    if (this._tempFile) {
      try {
        const getModule = globalThis.process?.getBuiltinModule;
        const fs = getModule ? getModule("fs") : null;
        if (fs) fs.rmSync(this.filePath, { force: true });
      } catch {}
    }
  }
}

let _lemmaCounter = 0;

/**
 * Lexical Search Axis strictly wrapping liblemma C substrate
 */
export class LemmaLexicalAxis {
  constructor(filePath = "") {
    this.name = "lexical";
    this._tempFile = !filePath;
    this.filePath = filePath || defaultTempPath(`corm_lemma_${++_lemmaCounter}`);
    this._lemmaAddon = loadAxisAddon("lemma", "liblemma");
    if (!this._lemmaAddon) {
      throw new Error("Native addon for liblemma is required by LemmaLexicalAxis");
    }
    this.handle = this._lemmaAddon.open(this.filePath);
  }

  registerText(ref, text) {
    if (!this.handle) {
      throw new Error("LemmaLexicalAxis is closed");
    }
    this._lemmaAddon.indexDoc(this.handle, ref >>> 0, 0, String(text));
  }

  fill(params) {
    if (!this.handle) {
      throw new Error("LemmaLexicalAxis is closed");
    }
    const set = new CandidateSet();
    const hits = this._lemmaAddon.fill(this.handle, String(params.term || ""), -1, 0);
    for (const ref of hits) {
      set.push(ref);
    }
    set.seal();
    return set;
  }

  rank(params, ref) {
    if (!this.handle) {
      throw new Error("LemmaLexicalAxis is closed");
    }
    const qHandle = this._lemmaAddon.prepareQuery(this.handle, String(params.term || ""), -1);
    try {
      return this._lemmaAddon.scorePrepared(this.handle, qHandle, ref >>> 0);
    } finally {
      this._lemmaAddon.freeQuery(qHandle);
    }
  }

  close() {
    if (this.handle) {
      this._lemmaAddon.close(this.handle);
      this.handle = null;
    }
    if (this._tempFile) {
      try {
        const getModule = globalThis.process?.getBuiltinModule;
        const fs = getModule ? getModule("fs") : null;
        if (fs) fs.rmSync(this.filePath, { force: true });
      } catch {}
    }
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
      data: this.db.get(this.db.ktype === "u" ? item.ref : String(item.ref))
    }));
  }
}
