/**
 * 撤销与重做：一次完整笔划构成一个事务。
 * 事务按“脏矩形”记录该笔划影响区域的修改前与修改后高度，恢复时整块写回，
 * 不复制整张地形，也不依赖重新执行画笔（重做精确恢复笔划完成后的状态）。
 */
import { clampHeight, HeightField } from "./heightfield";

/** 一个已完成的事务：矩形区域内的前后快照。 */
export interface TransactionData {
  /** 矩形起点网格坐标。 */
  x0: number;
  z0: number;
  /** 矩形宽度与高度（顶点数）。 */
  width: number;
  depth: number;
  /** 修改前高度（行主序）。 */
  before: Float32Array;
  /** 修改后高度（行主序）。 */
  after: Float32Array;
  /** 该事务占用的内存字节数（前后快照合计）。 */
  bytes: number;
}

/** 进行中的事务：懒记录首次写入前的高度，并把影响区域聚合为矩形。 */
export class StrokeTransaction {
  private readonly originals = new Map<number, number>();
  private minX = Number.POSITIVE_INFINITY;
  private minZ = Number.POSITIVE_INFINITY;
  private maxX = Number.NEGATIVE_INFINITY;
  private maxZ = Number.NEGATIVE_INFINITY;

  /** 返回该格在本次笔划开始前的高度；未写入过的格等于当前值。 */
  originalAt(field: HeightField, x: number, z: number): number {
    const index = field.index(x, z);
    if (index < 0) return 0;
    const recorded = this.originals.get(index);
    return recorded !== undefined ? recorded : field.heights[index];
  }

  /** 写入高度；首次触碰该格时记录修改前的值。 */
  write(field: HeightField, x: number, z: number, value: number): void {
    const index = field.index(x, z);
    if (index < 0) return;
    if (!this.originals.has(index)) {
      this.originals.set(index, field.heights[index]);
    }
    field.heights[index] = clampHeight(value);
    if (x < this.minX) this.minX = x;
    if (z < this.minZ) this.minZ = z;
    if (x > this.maxX) this.maxX = x;
    if (z > this.maxZ) this.maxZ = z;
  }

  /** 是否触碰过任何格子。 */
  get touched(): boolean {
    return this.originals.size > 0;
  }

  /** 当前影响范围（网格坐标矩形）；未触碰时返回 null。 */
  bounds(): { x0: number; z0: number; x1: number; z1: number } | null {
    if (!this.touched) return null;
    return {
      x0: Math.floor(this.minX), z0: Math.floor(this.minZ),
      x1: Math.floor(this.maxX), z1: Math.floor(this.maxZ)
    };
  }

  /**
   * 结束事务：生成脏矩形的前后快照。没有任何实际高度变化时返回 null
   * （不占用撤销步数）。矩形内未被触碰的格子前后值相同（取当前值），
   * 保证撤销/重做整块写回时不破坏矩形内的原地貌。
   */
  finish(field: HeightField): TransactionData | null {
    if (!this.touched) return null;
    const x0 = Math.floor(this.minX);
    const z0 = Math.floor(this.minZ);
    const width = Math.floor(this.maxX) - x0 + 1;
    const depth = Math.floor(this.maxZ) - z0 + 1;
    const before = new Float32Array(width * depth);
    const after = new Float32Array(width * depth);
    let changed = false;
    for (let z = 0; z < depth; ++z) {
      for (let x = 0; x < width; ++x) {
        const index = (z0 + z) * field.gridWidth + (x0 + x);
        const slot = z * width + x;
        const original = this.originals.get(index);
        before[slot] = original !== undefined ? original : field.heights[index];
        after[slot] = field.heights[index];
        if (original !== undefined && field.heights[index] !== original) changed = true;
      }
    }
    if (!changed) return null;
    return { x0, z0, width, depth, before, after, bytes: before.byteLength + after.byteLength };
  }
}

/** 撤销栈：固定步数上限，超出时移除最旧历史；新笔划清空重做分支。 */
export class UndoStack {
  private undoStack: TransactionData[] = [];
  private redoStack: TransactionData[] = [];

  constructor(public maxSteps = 30) {}

  /** 提交一个已完成的事务；没有实际变化的空事务直接丢弃。 */
  push(transaction: TransactionData | null): boolean {
    if (!transaction) return false;
    this.undoStack.push(transaction);
    this.redoStack = [];
    this.trim();
    return true;
  }

  /** 是否可以撤销。 */
  get canUndo(): boolean {
    return this.undoStack.length > 0;
  }

  /** 是否可以重做。 */
  get canRedo(): boolean {
    return this.redoStack.length > 0;
  }

  /** 可撤销的步数。 */
  get undoCount(): number {
    return this.undoStack.length;
  }

  /** 可重做的步数。 */
  get redoCount(): number {
    return this.redoStack.length;
  }

  /** 撤销一步：把矩形区域写回修改前高度，返回矩形供渲染局部刷新。 */
  undo(field: HeightField): TransactionData | null {
    const transaction = this.undoStack.pop();
    if (!transaction) return null;
    applyRect(field, transaction, transaction.before);
    this.redoStack.push(transaction);
    return transaction;
  }

  /** 重做一步：把矩形区域写回笔划完成后高度，返回矩形供渲染局部刷新。 */
  redo(field: HeightField): TransactionData | null {
    const transaction = this.redoStack.pop();
    if (!transaction) return null;
    applyRect(field, transaction, transaction.after);
    this.undoStack.push(transaction);
    return transaction;
  }

  /** 修改步数上限；减少时优先移除最旧历史。 */
  setMaxSteps(steps: number): void {
    this.maxSteps = Math.max(1, Math.floor(steps));
    this.trim();
  }

  /** 清空全部历史（重采样后调用）。 */
  clear(): void {
    this.undoStack = [];
    this.redoStack = [];
  }

  /** 撤销历史的大致内存占用（字节）。 */
  memoryBytes(): number {
    let total = 0;
    for (const transaction of this.undoStack) total += transaction.bytes;
    for (const transaction of this.redoStack) total += transaction.bytes;
    return total;
  }

  /** 裁掉超出上限的最旧历史。 */
  private trim(): void {
    while (this.undoStack.length > this.maxSteps) {
      this.undoStack.shift();
    }
  }
}

/** 把快照数据整块写回高度场的矩形区域。 */
function applyRect(field: HeightField, transaction: TransactionData, source: Float32Array): void {
  const { x0, z0, width, depth } = transaction;
  for (let z = 0; z < depth; ++z) {
    for (let x = 0; x < width; ++x) {
      const target = field.index(x0 + x, z0 + z);
      if (target < 0) continue;
      field.heights[target] = clampHeight(source[z * width + x]);
    }
  }
}
