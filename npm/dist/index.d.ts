export interface CormAxis<TFilter = any, TRank = any> {
  readonly name: string;
  fill?(params: TFilter): Promise<number[]> | number[] | CandidateSet;
  rank?(params: TRank, ref: number): Promise<number> | number;
}

export type CormKeyType = "s" | "u" | "a" | "2s" | "2u";
export type CormValType = "s" | "u" | "b";

export interface CormDbOptions {
  ktype?: CormKeyType;
  vtype?: CormValType;
}

export interface CormExecResult {
  code: number;
  stdout: string;
  stderr: string;
  stdoutBytes?: Uint8Array;
}

export declare function getCormWasm(): WebAssembly.Instance | null;
export declare function htokPack(jsonInput: string | Record<string, any>): Uint8Array;
export declare function htokUnpack(bytes: Uint8Array): string;
export declare function execCormCommand(
  op: string,
  filespec: string,
  arg2?: string | number,
  payload?: string | Uint8Array,
  env?: Record<string, string>
): CormExecResult;
export declare function closeCormEngine(filespec?: string): boolean;

export declare class CormEngine {
  readonly filespec: string;
  readonly env?: Record<string, string>;
  constructor(filespec: string, env?: Record<string, string>);
  ping(): CormExecResult;
  list(): CormExecResult;
  get(key?: string | number, json?: boolean): CormExecResult;
  put(key?: string | number, value?: any, json?: boolean): CormExecResult;
  del(key: string | number): CormExecResult;
  delAll(key?: string | number): CormExecResult;
  query(exprLine: string, optionsLines?: string[] | string, topK?: number): CormExecResult;
  close(): boolean;
}

export declare class CandidateSet {
  constructor(handle?: any, options?: { wasm?: boolean });
  static wasm(): CandidateSet;
  push(ref: number): this;
  seal(): this;
  toArray(): number[];
  count(): number;
  rawPointer(): number;
  toUint32Array(): Uint32Array;
  addFromRaw(ptr: number, count: number): this;
  intersect(other: CandidateSet): CandidateSet;
  union(other: CandidateSet): CandidateSet;
  subtract(other: CandidateSet): CandidateSet;
  free(): void;
}

export declare class MinHeapRanker {
  constructor(topK?: number, minScore?: number, options?: { wasm?: boolean });
  static wasm(topK?: number, minScore?: number): MinHeapRanker;
  push(ref: number, score: number): this;
  sorted(): Array<{ ref: number; score: number }>;
  free(): void;
}

export declare class JointTemporalAxis implements CormAxis<{ start: number; end: number; resourceId?: number }> {
  readonly name: string;
  constructor(filePath?: string);
  registerInterval(ref: number, resourceId: number, start: number, end: number): void;
  fill(params: { start: number; end: number; resourceId?: number }): CandidateSet;
  close(): void;
}

export declare class SepalVectorAxis implements CormAxis<void, { queryVector: number[] | Float32Array }> {
  readonly name: string;
  constructor(filePath?: string, dim?: number);
  registerVector(ref: number, vector: number[] | Float32Array): void;
  rank(params: { queryVector: number[] | Float32Array }, ref: number): number;
  close(): void;
}

export declare class LemmaLexicalAxis implements CormAxis<{ term: string }, { term: string }> {
  readonly name: string;
  constructor(filePath?: string);
  registerText(ref: number, text: string): void;
  fill(params: { term: string }): CandidateSet;
  rank(params: { term: string }, ref: number): number;
  close(): void;
}

export declare class CormDb<T = any> {
  constructor(filePath?: string, dbName?: string, options?: CormDbOptions);
  registerAxis(axis: CormAxis): this;
  getAxis(name: string): CormAxis | undefined;
  put(key: any, value: T): number;
  get(key: any): T | null;
  del(key: any): boolean;
  delAll(): boolean;
  listKeys(): any[];
  count(): number;
  save(): boolean;
  close(): void;
  query(): CormQuery<T>;
}

export declare class CormQuery<T = any> {
  constructor(db: CormDb<T>);
  filter(axis: string, params: any, join?: "AND" | "OR" | "NOT"): this;
  rank(axis: string, params: any, weight?: number): this;
  topK(k: number): this;
  minScore(score: number): this;
  execute(): Promise<Array<{ ref: number; score: number; data?: T | null }>>;
}
