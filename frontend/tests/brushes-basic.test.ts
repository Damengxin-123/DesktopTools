/* 细节/调整/山体画笔的行为验证。 */
import { describe, expect, it } from "vitest";
import { makeField, runStroke } from "./helpers";

describe("细节画笔", () => {
  it("左键抬高中心区域，边缘影响小于中心", () => {
    const field = makeField(33, 33, 2);
    const transaction = runStroke(field, "detail",
      [{ x: 32, z: 32, dt: 0.4 }]);
    expect(transaction).not.toBeNull();
    const center = field.sampleWorld(32, 32);
    const edge = field.sampleWorld(32 + 9, 32);
    expect(center).toBeGreaterThan(0);
    expect(edge).toBeGreaterThan(0);
    expect(edge).toBeLessThan(center);
  });

  it("Alt 降低地形（invert）", () => {
    const field = makeField(33, 33, 2);
    runStroke(field, "detail", [{ x: 32, z: 32, dt: 0.4 }],
      { detail: { size: 20, strength: 24, invert: true } });
    expect(field.sampleWorld(32, 32)).toBeLessThan(0);
  });

  it("按时间归一化：相同总时长的笔划效果一致", () => {
    const a = makeField(33, 33, 2);
    runStroke(a, "detail", [{ x: 32, z: 32, dt: 0.5 }]);
    const b = makeField(33, 33, 2);
    for (let index = 0; index < 5; ++index) {
      runStroke(b, "detail", [{ x: 32, z: 32, dt: 0.1 }]);
    }
    expect(b.sampleWorld(32, 32)).toBeCloseTo(a.sampleWorld(32, 32), 1);
  });

  it("快速拖动时笔划连续（路径插值）", () => {
    const field = makeField(81, 81, 2);
    runStroke(field, "detail", [{ x: 8, z: 80, dt: 0.02 }, { x: 152, z: 80, dt: 0.02 }]);
    // 相距 4 米的三个中点都被抬升，无明显断点。
    expect(field.sampleWorld(76, 80)).toBeGreaterThan(0.01);
    expect(field.sampleWorld(80, 80)).toBeGreaterThan(0.01);
    expect(field.sampleWorld(84, 80)).toBeGreaterThan(0.01);
  });
});

describe("调整画笔", () => {
  it("平滑减少局部高度突变", () => {
    const field = makeField(33, 33, 2, (x) => (x === 16 ? 20 : 0));
    const before = Math.abs(field.sampleWorld(32, 0) - field.sampleWorld(30, 0));
    runStroke(field, "adjust", [{ x: 32, z: 0, dt: 0.3 }],
      { adjust: { size: 20, strength: 1, mode: "smooth" } });
    const after = Math.abs(field.sampleWorld(32, 0) - field.sampleWorld(30, 0));
    expect(after).toBeLessThan(before);
  });

  it("平整把区域调整到落笔时高度，目标在笔划期间固定", () => {
    const field = makeField(65, 65, 2, (x, z) => x * 2 + z);
    // 落笔点 (64,64) 的原始高度 = 96+64=160？网格 32,32 → 64+32=96。
    runStroke(field, "adjust",
      [{ x: 64, z: 64, dt: 0.2 }, { x: 80, z: 80, dt: 0.2 }, { x: 96, z: 96, dt: 0.6 }],
      { adjust: { size: 24, strength: 1, mode: "flatten" } });
    const atEnd = field.sampleWorld(96, 96);
    expect(atEnd).toBeGreaterThan(96); // 目标 96 低于原地形，区域被压向 96。
    expect(atEnd).toBeLessThan(192); // 未完全回到原值。
  });

  it("锐化的单次高度变化受限，不产生失控尖峰", () => {
    const field = makeField(33, 33, 2, (x, z) => x === 16 && z === 16 ? 50 : 0);
    runStroke(field, "adjust",
      Array.from({ length: 10 }, () => ({ x: 32, z: 32, dt: 0.016 })),
      { adjust: { size: 12, strength: 2, mode: "sharpen" } });
    for (let index = 0; index < field.heights.length; ++index) {
      expect(Math.abs(field.heights[index])).toBeLessThan(70);
    }
    // 单次变化限幅：一次短笔划中心增量不超过约 1.5 米。
    const single = makeField(33, 33, 2, (x, z) => x === 16 && z === 16 ? 50 : 0);
    runStroke(single, "adjust", [{ x: 32, z: 32, dt: 0.016 }],
      { adjust: { size: 12, strength: 2, mode: "sharpen" } });
    expect(single.sampleWorld(32, 32)).toBeLessThan(52);
  });
});

describe("山画笔", () => {
  it("四种预设形成可辨认的不同剖面", () => {
    const profiles: Record<string, number[]> = {};
    for (const type of ["hill", "peak", "ridge", "mesa"] as const) {
      const field = makeField(101, 101, 2);
      // 单次长时间笔划：包络在笔划内收敛，得到该预设的目标外形。
      runStroke(field, "mountain", [{ x: 100, z: 100, dt: 3 }],
        { mountain: { size: 60, strength: 1, type } });
      const line: number[] = [];
      for (let x = 80; x <= 120; x += 2) line.push(field.sampleWorld(x, 100));
      profiles[type] = line;
    }
    // 台地顶部平缓：中心与 1/4 处高差小；尖峰陡峭：高差大。
    const mesa = profiles.mesa;
    const peak = profiles.peak;
    expect(Math.abs(mesa[8] - mesa[10])).toBeLessThan(Math.abs(peak[8] - peak[10]) * 0.5);
    // 峰值明显高于丘陵。
    expect(peak[10]).toBeGreaterThan(mesa[10] * 1.4);
    // 包络上限：单笔划不超过预设幅度（含噪声余量）。
    expect(mesa[10]).toBeLessThan(0.5 * 30 * 1.05 + 2);
    expect(peak[10]).toBeLessThan(1.15 * 30 * 1.15 + 2);
  });

  it("山脊沿绘制方向拉长", () => {
    const field = makeField(161, 161, 2);
    runStroke(field, "mountain", [{ x: 160, z: 160, dt: 3 }],
      { mountain: { size: 40, strength: 1, type: "ridge" } });
    const along = field.sampleWorld(160 + 50, 160);
    const across = field.sampleWorld(160, 160 + 50);
    expect(along).toBeGreaterThan(across * 2);
  });

  it("绘制方向改变山脊走向", () => {
    const field = makeField(161, 161, 2);
    runStroke(field, "mountain",
      [{ x: 120, z: 120, dt: 0.1 }, { x: 200, z: 200, dt: 2.9 }],
      { mountain: { size: 40, strength: 1, type: "ridge" } });
    // 沿对角线方向 45°：对角线上远点高于横向远点。
    const along = field.sampleWorld(160 + 35, 160 + 35);
    const across = field.sampleWorld(160 + 35, 160 - 35);
    expect(along).toBeGreaterThan(across);
  });
});
