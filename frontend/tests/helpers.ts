/**
 * 核心算法测试辅助：在数据层直接执行一次完整笔划（等价于一次拖动）。
 * 与编辑器相同的画笔入口与路径插值方式，但不依赖 DOM 与渲染。
 */
import { applyStamp, BrushSettings, defaultBrushSettings, ToolId } from "../src/core/brushes";
import { HeightField } from "../src/core/heightfield";
import { StrokeTransaction, TransactionData, UndoStack } from "../src/core/undo";

export interface StrokePoint { x: number; z: number; dt?: number }

/** 执行一次笔划：沿点序列插值落笔（步长与编辑器一致）→ 结束并返回事务。 */
export function runStroke(field: HeightField, tool: ToolId, points: StrokePoint[],
                          overrides: Partial<BrushSettings> = {},
                          baseDirection = { x: 1, z: 0 }): TransactionData | null {
  const settings: BrushSettings = { ...defaultBrushSettings(), ...overrides };
  const transaction = new StrokeTransaction();
  const scratch = new Map<number, number>();
  const flattenTarget = field.sampleWorld(points[0].x, points[0].z);
  let direction = { ...baseDirection };
  const stamp = (x: number, z: number, dt: number): void => {
    applyStamp(tool, settings, {
      field, transaction, flattenTarget, scratch,
      hint: () => undefined
    }, { centerX: x, centerZ: z, dt, dirX: direction.x, dirZ: direction.z });
  };
  // 与编辑器 stampAlong 相同的插值：相邻点之间按固定步长补齐，快速拖动不断线。
  for (let index = 0; index < points.length; ++index) {
    if (index > 0) {
      const from = points[index - 1];
      const to = points[index];
      const segment = { x: to.x - from.x, z: to.z - from.z };
      const length = Math.hypot(segment.x, segment.z);
      if (length > 0) direction = { x: segment.x / length, z: segment.z / length };
      const steps = Math.max(1, Math.min(256, Math.ceil(length / 4)));
      for (let step = 1; step <= steps; ++step) {
        const t = step / steps;
        stamp(from.x + segment.x * t, from.z + segment.z * t, (to.dt ?? 0.016) / steps);
      }
    } else {
      stamp(points[index].x, points[index].z, points[index].dt ?? 0.016);
    }
  }
  return transaction.finish(field);
}

/** 收集画笔提示信息（海岸画笔未检测到岸线时使用）。 */
export function runStrokeWithHints(field: HeightField, tool: ToolId, points: StrokePoint[],
                                   overrides: Partial<BrushSettings> = {}): {
  transaction: TransactionData | null; hints: string[];
} {
  const settings: BrushSettings = { ...defaultBrushSettings(), ...overrides };
  const transaction = new StrokeTransaction();
  const scratch = new Map<number, number>();
  const flattenTarget = field.sampleWorld(points[0].x, points[0].z);
  const hints: string[] = [];
  for (const point of points) {
    applyStamp(tool, settings, {
      field, transaction, flattenTarget, scratch, hint: (message) => hints.push(message)
    }, { centerX: point.x, centerZ: point.z, dt: point.dt ?? 0.016, dirX: 1, dirZ: 0 });
  }
  return { transaction: transaction.finish(field), hints };
}

/** 创建 W×D 顶点的高度场（米）。 */
export function makeField(width: number, depth: number, spacing: number,
                          fill: (x: number, z: number) => number = () => 0): HeightField {
  const field = new HeightField(width, depth, spacing);
  for (let z = 0; z < depth; ++z) {
    for (let x = 0; x < width; ++x) {
      field.heights[z * width + x] = fill(x, z);
    }
  }
  return field;
}

export { UndoStack };
