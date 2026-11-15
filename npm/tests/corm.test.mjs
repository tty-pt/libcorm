import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import {
  CormDb,
  CormEngine,
  CandidateSet,
  MinHeapRanker,
  JointTemporalAxis,
  htokPack,
  htokUnpack,
  closeCormEngine
} from "../dist/index.js";

test("CormDb basic CRUD persistence and cleanup", () => {
  const tmpDir = path.join(os.tmpdir(), `corm_npm_test_${Date.now()}`);
  fs.mkdirSync(tmpDir, { recursive: true });
  const dbFile = path.join(tmpDir, "test.corm");

  const db = new CormDb(dbFile, "npm_test_store");
  assert.ok(db, "CormDb should initialize successfully");

  db.put("user:101", { name: "Alice", role: "admin" });
  db.put("user:102", { name: "Bob", role: "member" });
  db.save();

  assert.equal(db.count(), 2);
  const keys = db.listKeys();
  assert.deepEqual(keys.sort(), ["user:101", "user:102"]);

  const alice = db.get("user:101");
  assert.equal(alice?.name, "Alice");
  assert.equal(alice?.role, "admin");

  const bob = db.get("user:102");
  assert.equal(bob?.name, "Bob");

  db.del("user:102");
  const missing = db.get("user:102");
  assert.equal(missing, null);

  db.delAll();
  assert.equal(db.count(), 0);

  db.close();
  fs.rmSync(tmpDir, { recursive: true, force: true });
});

test("CormDb typed keys (u, a, 2s) and htokPack / htokUnpack", () => {
  const tmpDir = path.join(os.tmpdir(), `corm_typed_test_${Date.now()}`);
  fs.mkdirSync(tmpDir, { recursive: true });
  const dbFile = path.join(tmpDir, "typed.corm");

  const jsonDoc = JSON.stringify({
    level: "L1",
    type: "episode",
    lifecycle: "supported",
    summary: "Fix audio latency in FLAC pipeline",
    timestamp: "2026-03-30T12:00:00Z",
    standing: false,
    evidence: [1, 5],
    tags: ["audio", "flac"],
    context: { project: "site", subsystem: "audio" },
    episode: {
      problem: "High buffer latency",
      outcome: "success",
      findings: ["Ring buffer size was throttled"],
      diffs: ["src/audio.c"],
      approaches: [],
      sessionId: "12345678-1234-5678-1234-567812345678"
    }
  });
  const packed = htokPack(jsonDoc);
  assert.ok(packed instanceof Uint8Array);
  assert.ok(packed.byteLength > 12);
  const unpacked = JSON.parse(htokUnpack(packed));
  assert.equal(unpacked.level, "L1");
  assert.equal(unpacked.type, "episode");
  assert.equal(unpacked.summary, "Fix audio latency in FLAC pipeline");

  const autoDb = new CormDb(dbFile, "auto_store", { ktype: "a", vtype: "s" });
  const id1 = autoDb.put(null, { title: "First" });
  const id2 = autoDb.put(null, { title: "Second" });
  assert.equal(id1, 0);
  assert.equal(id2, 1);
  assert.equal(autoDb.get(0)?.title, "First");
  assert.equal(autoDb.get(1)?.title, "Second");

  const pairDb = new CormDb(dbFile, "pair_store", { ktype: "2s", vtype: "u" });
  pairDb.put(["ns", "itemA"], 99);
  assert.equal(pairDb.get(["ns", "itemA"]), 99);

  autoDb.close();
  pairDb.close();
  fs.rmSync(tmpDir, { recursive: true, force: true });
});

test("CormEngine in-process PUT_JSON, GET_JSON, LIST, QUERY with lemma/joint axes", () => {
  const tmpDir = path.join(os.tmpdir(), `corm_engine_test_${Date.now()}`);
  fs.mkdirSync(tmpDir, { recursive: true });

  // 1. Unconditional base CormEngine in-process test (no external Layer 2 axis plugins needed)
  const baseDbFile = path.join(tmpDir, "base.corm");
  const baseEngine = new CormEngine(`${baseDbFile}:a:b`);
  const bp1 = baseEngine.put(
    ".",
    {
      level: "L1",
      type: "episode",
      lifecycle: "supported",
      summary: "In-process CormEngine base HTOK v4 roundtrip",
      timestamp: "2026-03-10T12:00:00Z",
      standing: false,
      evidence: [],
      tags: ["corm", "napi"],
      context: { project: "site" }
    },
    true
  );
  assert.equal(bp1.code, 0, bp1.stderr);
  assert.equal(Number(bp1.stdout.trim()), 0);

  const bList = baseEngine.list();
  assert.equal(bList.code, 0, bList.stderr);
  assert.ok(bList.stdout.includes("0"));

  const bGet = baseEngine.get(0, true);
  assert.equal(bGet.code, 0, bGet.stderr);
  assert.equal(JSON.parse(bGet.stdout).summary, "In-process CormEngine base HTOK v4 roundtrip");
  assert.equal(baseEngine.close(), true);
  closeCormEngine(baseDbFile);

  // 2. Full @lemma,joint axis query test when Layer 2 axis shared libraries are available
  const probeDbFile = path.join(tmpDir, "engine.corm");
  const engine = new CormEngine(`${probeDbFile}@lemma,joint:a:b`);
  const p1 = engine.put(
    ".",
    {
      level: "L1",
      type: "episode",
      lifecycle: "supported",
      summary: "PostgreSQL connection pool tuning and B-tree index latency",
      timestamp: "2026-03-10T12:00:00Z",
      standing: false,
      evidence: [],
      tags: ["postgres", "database"],
      context: { project: "site" },
      episode: {
        problem: "Slow connection checkout",
        outcome: "success",
        findings: ["B-tree index reduced query time"],
        diffs: [],
        approaches: [],
        sessionId: "sess-1"
      }
    },
    true
  );
  if (p1.code !== 0 && p1.stderr.includes("not found in CORM_AXIS_PATH")) {
    engine.close();
    fs.rmSync(tmpDir, { recursive: true, force: true });
    return;
  }
  assert.equal(p1.code, 0, p1.stderr);
  const ref1 = Number(p1.stdout.trim());
  assert.equal(ref1, 0);

  const p2 = engine.put(
    ".",
    {
      level: "L1",
      type: "episode",
      lifecycle: "supported",
      summary: "Frontend CSS grid layout alignment",
      timestamp: "2026-03-20T12:00:00Z",
      standing: false,
      evidence: [],
      tags: ["css", "ui"],
      context: { project: "site" },
      episode: {
        problem: "Misaligned grid",
        outcome: "success",
        findings: ["Fixed flex fallback"],
        diffs: [],
        approaches: [],
        sessionId: "sess-2"
      }
    },
    true
  );
  assert.equal(p2.code, 0, p2.stderr);
  const ref2 = Number(p2.stdout.trim());
  assert.equal(ref2, 1);

  const listRes = engine.list();
  assert.equal(listRes.code, 0);
  assert.ok(listRes.stdout.includes("0"));
  assert.ok(listRes.stdout.includes("1"));

  const getRes = engine.get(ref1, true);
  assert.equal(getRes.code, 0, getRes.stderr);
  const parsed = JSON.parse(getRes.stdout);
  assert.equal(parsed.summary, "PostgreSQL connection pool tuning and B-tree index latency");

  const qRes = engine.query(
    "(lemma AND joint)",
    ["--query=PostgreSQL", "--since=2026-03-01", "--until=2026-03-15"],
    5
  );
  assert.equal(qRes.code, 0, qRes.stderr);
  assert.ok(qRes.stdout.startsWith("0 "), `unexpected query stdout: ${qRes.stdout}`);

  // Scoped query expression test (used by CodeStore)
  const scopedRes = engine.query(
    "(a:lemma AND b:joint)",
    ["--query@a=PostgreSQL", "--since@b=2026-03-01", "--until@b=2026-03-15"],
    5
  );
  assert.equal(scopedRes.code, 0, scopedRes.stderr);
  assert.ok(scopedRes.stdout.startsWith("0 "), `unexpected scoped stdout: ${scopedRes.stdout}`);

  assert.equal(engine.close(), true);
  closeCormEngine(probeDbFile);
  fs.rmSync(tmpDir, { recursive: true, force: true });
});

test("CandidateSet C algebra: intersect, union, and subtract", () => {
  const setA = new CandidateSet();
  setA.push(10).push(20).push(30).seal();

  const setB = new CandidateSet();
  setB.push(20).push(30).push(40).seal();

  const intersection = setA.intersect(setB);
  const interArr = intersection.toArray();
  assert.deepEqual(interArr.sort(), [20, 30]);

  const union = setA.union(setB);
  const unionArr = union.toArray();
  assert.deepEqual(unionArr.sort(), [10, 20, 30, 40]);

  const diff = setA.subtract(setB);
  const diffArr = diff.toArray();
  assert.deepEqual(diffArr.sort(), [10]);
});

test("MinHeapRanker bounded streaming ranking", () => {
  const ranker = new MinHeapRanker(3, 0.1);
  ranker.push(1, 0.4);
  ranker.push(2, 0.9);
  ranker.push(3, 0.2);
  ranker.push(4, 0.7);
  ranker.push(5, 0.05); // Below threshold

  const results = ranker.sorted();
  assert.equal(results.length, 3);
  assert.equal(results[0].ref, 2);
  assert.equal(results[1].ref, 4);
  assert.equal(results[2].ref, 1);
});

test("JointTemporalAxis integration with CormDb and CandidateSet", () => {
  let temporal;
  try {
    temporal = new JointTemporalAxis();
  } catch (err) {
    assert.match(String(err?.message || err), /required by JointTemporalAxis/);
    return;
  }
  temporal.registerInterval(101, 1, 1000, 2000);
  temporal.registerInterval(102, 2, 3000, 4000);

  const candidates = temporal.fill({ start: 1200, end: 1800, resourceId: 1 });
  assert.equal(candidates.toArray().length, 1);
  assert.equal(candidates.toArray()[0], 101);

  temporal.close();
});

test("Zero-copy raw pointer chaining across CandidateSet and JointTimeline", () => {
  const setA = CandidateSet.wasm();
  setA.push(100).push(200).push(300).seal();

  assert.equal(setA.count(), 3);
  assert.ok(setA.rawPointer() > 0);

  const u32 = setA.toUint32Array();
  assert.equal(u32 instanceof Uint32Array, true);
  assert.equal(u32.length, 3);
  assert.equal(u32[0], 100);
  assert.equal(u32[1], 200);
  assert.equal(u32[2], 300);

  // Cross-module raw pointer handoff into another CandidateSet
  const setB = CandidateSet.wasm();
  setB.addFromRaw(setA.rawPointer(), setA.count()).seal();
  assert.equal(setB.count(), 3);
  assert.deepEqual(setB.toArray(), [100, 200, 300]);
});
