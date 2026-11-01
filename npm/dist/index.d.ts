export interface CormAxis<TFilter = any, TRank = any> {
  readonly name: string;
  fill?(params: TFilter): Promise<number[]> | number[] | CandidateSet;
  rank?(params: TRank, ref: number): Promise<number> | number;
}

export declare class CandidateSet {
  constructor(handle?: any);
  push(ref: number): this;
  seal(): this;
  toArray(): number[];
  intersect(other: CandidateSet): CandidateSet;
  union(other: CandidateSet): CandidateSet;
  subtract(other: CandidateSet): CandidateSet;
}

export declare class MinHeapRanker {
  constructor(topK?: number, minScore?: number);
  push(ref: number, score: number): void;
  sorted(): Array<{ ref: number; score: number }>;
}

export declare class JointTemporalAxis implements CormAxis<{ start: number; end: number; resourceId?: number }> {
  readonly name: string;
  constructor(filePath?: string);
  registerInterval(ref: number, resourceId: number, start: number, end: number): void;
  fill(params: { start: number; end: number; resourceId?: number }): CandidateSet;
  close(): void;
}

export declare class SepalVectorAxis implements CormAxis<void, { queryVector: number[] }> {
  readonly name: string;
  constructor();
  registerVector(ref: number, vector: number[]): void;
  rank(params: { queryVector: number[] }, ref: number): number;
}

export declare class LemmaLexicalAxis implements CormAxis<{ term: string }, { term: string }> {
  readonly name: string;
  constructor();
  registerText(ref: number, text: string): void;
  fill(params: { term: string }): CandidateSet;
  rank(params: { term: string }, ref: number): number;
}

export declare class CormDb<T = any> {
  constructor(filePath?: string, dbName?: string);
  registerAxis(axis: CormAxis): this;
  getAxis(name: string): CormAxis | undefined;
  put(key: string, value: T): number;
  get(key: string): T | null;
  del(key: string): boolean;
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
