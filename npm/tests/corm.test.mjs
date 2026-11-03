import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { CormDb, CandidateSet, MinHeapRanker, JointTemporalAxis } from "../dist/index.js";

test("CormDb basic CRUD persistence and cleanup", () => {
  const tmpDir = path.join("/tmp", `corm_npm_test_${Date.now()}`);
  fs.mkdirSync(tmpDir, { recursive: true });
  const dbFile = path.join(tmpDir, "test.corm");

  const db = new CormDb(dbFile, "npm_test_store");
  assert.ok(db, "CormDb should initialize successfully");

  db.put("user:101", { name: "Alice", role: "admin" });
  db.put("user:102", { name: "Bob", role: "member" });
  db.save();

  const alice = db.get("user:101");
  assert.equal(alice?.name, "Alice");
  assert.equal(alice?.role, "admin");

  const bob = db.get("user:102");
  assert.equal(bob?.name, "Bob");

  db.del("user:102");
  const missing = db.get("user:102");
  assert.equal(missing, null);

  db.close();
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
  const temporal = new JointTemporalAxis();
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
