/* 河流、道路、湖泊、海岸画笔与撤销/重做的行为验证。 */
import { describe, expect, it } from "vitest";
import { UndoStack } from "../src/core/undo";
import { makeField, runStroke, runStrokeWithHints } from "./helpers";

describe("河流画笔", () => {
  it("沿路径挖出连续河床，深度接近设定值且同一笔划重叠不加深", () => {
    const field = makeField(81, 81, 2, () => 40);
    runStroke(field, "river", [
      { x: 20, z: 80 }, { x: 60, z: 80, dt: 0.4 }, { x: 100, z: 80, dt: 0.4 },
      { x: 60, z: 80, dt: 0.4 } // 回笔覆盖同一位置。
    ], { river: { width: 10, depth: 5, stratum: "normal" } });
    const bed = field.sampleWorld(60, 80);
    expect(40 - bed).toBeGreaterThan(3); // 明显挖深。
    expect(40 - bed).toBeLessThanOrEqual(5.6); // 不超过设计深度（含噪声/边缘容差）。
    // 连续性：沿途各点同样挖开，无断点。
    expect(field.sampleWorld(70, 80)).toBeLessThan(38);
    expect(field.sampleWorld(90, 80)).toBeLessThan(38);
  });

  it("新笔划可以基于当前地形继续挖深", () => {
    const field = makeField(81, 81, 2, () => 40);
    runStroke(field, "river", [{ x: 80, z: 80, dt: 0.4 }],
      { river: { width: 10, depth: 5, stratum: "normal" } });
    const afterFirst = field.sampleWorld(80, 80);
    runStroke(field, "river", [{ x: 80, z: 80, dt: 0.4 }],
      { river: { width: 10, depth: 5, stratum: "normal" } });
    const afterSecond = field.sampleWorld(80, 80);
    expect(afterSecond).toBeLessThan(afterFirst);
    expect(40 - afterSecond).toBeGreaterThan(5); // 相对新地面再挖 5 米。
    expect(40 - afterSecond).toBeLessThan(11); // 但不会失控。
  });

  it("地层类型影响岸坡：松软土层岸坡更宽缓（同等距离下挖得更深）", () => {
    const build = (stratum: "soft" | "normal" | "hard") => {
      const field = makeField(81, 81, 2, () => 40);
      runStroke(field, "river", [{ x: 80, z: 80, dt: 0.6 }],
        { river: { width: 10, depth: 6, stratum } });
      return Math.abs(field.sampleWorld(80 + 7, 80) - 40);
    };
    const soft = build("soft");
    const hard = build("hard");
    expect(soft).toBeGreaterThan(hard); // 松软：坡度延伸更远更缓。
  });
});

describe("道路画笔", () => {
  it("跨越斜坡时路面横向平整、纵向随原地形", () => {
    // 原地形网格方向斜坡：h = x + 2z（世界每米 0.5 + 1 米）。
    const field = makeField(101, 101, 2, (x, z) => x + 2 * z);
    runStroke(field, "road", [
      { x: 40, z: 120, dt: 1.0 }, { x: 160, z: 120, dt: 1.0 }
    ], { road: { width: 8, bed: "gentle" } });
    // 横向：路面内三点高度接近（平整）。
    const center = field.sampleWorld(80, 120);
    const sideA = field.sampleWorld(80, 116);
    const sideB = field.sampleWorld(80, 124);
    expect(Math.abs(sideA - center)).toBeLessThan(2.5);
    expect(Math.abs(sideB - center)).toBeLessThan(2.5);
    // 纵向：路面高度随原地形上升，且接近原地面高度（±放坡容差）。
    const at40 = field.sampleWorld(40, 120);
    const at160 = field.sampleWorld(160, 120);
    expect(at160 - at40).toBeGreaterThan(30);
    expect(Math.abs(at40 - 140)).toBeLessThan(8); // 原高度 = 20 + 2×60 = 140。
    expect(Math.abs(at160 - 200)).toBeLessThan(8); // 原高度 = 80 + 2×60 = 200。
  });

  it("陡直路基的过渡带比平缓路基窄", () => {
    const build = (bed: "gentle" | "steep") => {
      const field = makeField(101, 101, 2, (x, z) => x + 2 * z);
      runStroke(field, "road", [{ x: 40, z: 120, dt: 1.0 }, { x: 160, z: 120, dt: 1.0 }],
        { road: { width: 8, bed } });
      // 路边缘外 8 米处：过渡带越窄，这里越接近原地面。
      return field.sampleWorld(80, 120 + 8);
    };
    const gentle = build("gentle");
    const steep = build("steep");
    expect(steep).toBeGreaterThan(gentle);
  });
});

describe("湖泊画笔", () => {
  it("形成连续湖盆，中心最深，重复覆盖不加深", () => {
    const field = makeField(121, 121, 2, () => 30);
    runStroke(field, "lake", [
      { x: 120, z: 120, dt: 0.2 }, { x: 140, z: 120, dt: 0.4 }, { x: 120, z: 120, dt: 0.4 }
    ], { lake: { size: 60, depth: 8, stratum: "normal" } });
    const center = field.sampleWorld(120, 120);
    expect(30 - center).toBeGreaterThan(5);
    expect(30 - center).toBeLessThanOrEqual(8.8);
    // 盆底柔和：中心与 1/4 半径处高差有限；范围外不受影响。
    const quarter = field.sampleWorld(120 + 10, 120);
    expect(Math.abs(quarter - center)).toBeLessThan(4);
    expect(field.sampleWorld(120 + 90, 120)).toBeCloseTo(30, 0);
  });

  it("地层类型改变湖岸坡度（松软岸坡更宽缓）", () => {
    const build = (stratum: "soft" | "hard") => {
      const field = makeField(161, 161, 2, () => 30);
      runStroke(field, "lake", [{ x: 160, z: 160, dt: 0.6 }],
        { lake: { size: 60, depth: 8, stratum } });
      return Math.abs(field.sampleWorld(160 + 45, 160) - 30);
    };
    expect(build("soft")).toBeGreaterThan(build("hard"));
  });
});

describe("海岸画笔", () => {
  // 原地形：网格方向斜坡，海平面 0 在网格第 50 列（世界 100 米）。
  const shoreline = (type: "sand" | "dirt" | "rock") => {
    const field = makeField(101, 101, 2, (x) => (x - 50) * 0.8);
    const { transaction, hints } = runStrokeWithHints(field, "coast",
      [{ x: 100, z: 100, dt: 0.8 }],
      { coast: { size: 60, strength: 1, seaLevel: 0, type } });
    return { field, transaction, hints };
  };
  it("三种类型都修改岸坡并保留高低关系", () => {
    for (const type of ["sand", "dirt", "rock"] as const) {
      const { field, transaction, hints } = shoreline(type);
      expect(transaction).not.toBeNull();
      expect(hints.length).toBe(0);
      // 陆地一侧仍高于海平面、低地仍低于海平面。
      expect(field.sampleWorld(160, 100)).toBeGreaterThan(0);
      expect(field.sampleWorld(40, 100)).toBeLessThan(0);
    }
  });
  it("类型间坡形可区分：岩岸岸线附近最高、沙岸最缓", () => {
    const nearShore = (type: "sand" | "dirt" | "rock") => {
      const { field } = shoreline(type);
      return field.sampleWorld(110, 100); // 岸线陆地一侧 10 米处。
    };
    expect(nearShore("rock")).toBeGreaterThan(nearShore("dirt"));
    expect(nearShore("dirt")).toBeGreaterThan(nearShore("sand"));
  });
  it("没有岸线过渡时不修改并给出提示", () => {
    const field = makeField(101, 101, 2, () => 20);
    const { transaction, hints } = runStrokeWithHints(field, "coast",
      [{ x: 100, z: 100, dt: 0.5 }], { coast: { size: 60, strength: 1, seaLevel: 0, type: "sand" } });
    expect(transaction).toBeNull();
    expect(hints.length).toBeGreaterThan(0);
  });
});

describe("撤销与重做", () => {
  it("一步撤销完整恢复笔划前地形，重做精确恢复笔划后地形", () => {
    const field = makeField(81, 81, 2, (x, z) => Math.sin(x * 0.3) * 5 + z * 0.1);
    const before = Float32Array.from(field.heights);
    const transaction = runStroke(field, "mountain", [
      { x: 40, z: 60, dt: 0.1 }, { x: 70, z: 80, dt: 0.1 }, { x: 100, z: 110, dt: 0.1 }
    ], { mountain: { size: 40, strength: 1, type: "peak" } });
    expect(transaction).not.toBeNull();
    const after = Float32Array.from(field.heights);
    const stack = new UndoStack(30);
    expect(stack.push(transaction)).toBe(true);
    stack.undo(field);
    expect(field.heights).toEqual(before);
    stack.redo(field);
    expect(field.heights).toEqual(after);
  });

  it("多次笔划后逐步撤销与重做精确恢复", () => {
    const field = makeField(81, 81, 2, () => 10);
    const stack = new UndoStack(30);
    const snapshots: Float32Array[] = [Float32Array.from(field.heights)];
    const first = runStroke(field, "river", [{ x: 20, z: 40, dt: 0.3 }, { x: 100, z: 40, dt: 0.3 }],
      { river: { width: 10, depth: 4, stratum: "normal" } });
    stack.push(first);
    snapshots.push(Float32Array.from(field.heights));
    const second = runStroke(field, "mountain", [{ x: 120, z: 120, dt: 0.3 }],
      { mountain: { size: 30, strength: 1, type: "hill" } });
    stack.push(second);
    snapshots.push(Float32Array.from(field.heights));
    stack.undo(field);
    expect(field.heights).toEqual(snapshots[1]);
    stack.undo(field);
    expect(field.heights).toEqual(snapshots[0]);
    stack.redo(field);
    stack.redo(field);
    expect(field.heights).toEqual(snapshots[2]);
  });

  it("新笔划清空重做分支；超过上限移除最旧历史", () => {
    const field = makeField(41, 41, 2);
    const stack = new UndoStack(2);
    stack.push(runStroke(field, "detail", [{ x: 10, z: 20, dt: 0.2 }]));
    stack.push(runStroke(field, "detail", [{ x: 20, z: 20, dt: 0.2 }]));
    stack.undo(field);
    expect(stack.canRedo).toBe(true);
    stack.push(runStroke(field, "detail", [{ x: 30, z: 20, dt: 0.2 }]));
    expect(stack.canRedo).toBe(false); // 重做分支被清空。
    expect(stack.undoCount).toBe(2);
    stack.undo(field);
    stack.undo(field);
    expect(stack.canUndo).toBe(false);
  });

  it("没有实际改变高度的操作不占撤销步数", () => {
    const field = makeField(41, 41, 2);
    // 平整画笔的落笔点高度即目标，平坦地形上无变化。
    const empty = runStroke(field, "adjust", [{ x: 20, z: 20, dt: 0.3 }],
      { adjust: { size: 20, strength: 1, mode: "flatten" } });
    expect(empty).toBeNull();
  });

  it("撤销历史按上限裁剪并统计内存占用", () => {
    const field = makeField(65, 65, 2);
    const stack = new UndoStack(3);
    for (let index = 0; index < 6; ++index) {
      stack.push(runStroke(field, "detail", [{ x: 10 + index * 6, z: 40, dt: 0.2 }]));
    }
    expect(stack.undoCount).toBe(3);
    expect(stack.memoryBytes()).toBeGreaterThan(0);
  });
});

describe("笔划边界安全", () => {
  it("地形边缘与平坦地形绘制不越界、不产生 NaN", () => {
    const field = makeField(21, 21, 2);
    const transaction = runStroke(field, "detail", [
      { x: 0, z: 0, dt: 0.2 }, { x: 40, z: 0, dt: 0.2 }, { x: 40, z: 40, dt: 0.2 }
    ]);
    expect(transaction).not.toBeNull();
    for (const value of field.heights) {
      expect(Number.isFinite(value)).toBe(true);
      expect(Math.abs(value)).toBeLessThanOrEqual(10000);
    }
  });

  it("河流在地形边缘绘制安全裁剪", () => {
    const field = makeField(21, 21, 2, () => 10);
    const transaction = runStroke(field, "river",
      [{ x: 2, z: 2, dt: 0.4 }], { river: { width: 10, depth: 4, stratum: "normal" } });
    expect(transaction).not.toBeNull();
    expect(field.heights.every((value) => Number.isFinite(value))).toBe(true);
  });
});
