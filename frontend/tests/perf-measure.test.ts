/* 临时性能测量：记录核心数据层操作在两档网格规模下的耗时。 */
import { describe, expect, it } from "vitest";
import { HeightField } from "../src/core/heightfield";
import { computeMapping, encode16Bit, resample } from "../src/core/resample";
import { makeField, runStroke } from "./helpers";

function measure(label: string, fn: () => void): number {
  const start = performance.now();
  fn();
  const elapsed = performance.now() - start;
  console.log(`PERF ${label}: ${elapsed.toFixed(1)} ms`);
  return elapsed;
}

describe("性能实测（数据层）", () => {
  it("513×513 与 1025×1025 两档", () => {
    for (const side of [513, 1025]) {
      console.log(`PERF === ${side}x${side} (${(side * side * 4 / 1048576).toFixed(1)} MB 高度数据) ===`);
      const field = makeField(side, side, 2, (x, z) => Math.sin(x * 0.01) * 20 + z * 0.05);
      measure(`${side} 细节笔划（10 点拖动）`, () => {
        const points = Array.from({ length: 10 }, (_, i) => ({ x: 20 + i * 4, z: side }));
        runStroke(field, "detail", points);
      });
      measure(`${side} 河流笔划（20 点，宽 12 深 5）`, () => {
        const points = Array.from({ length: 20 }, (_, i) => ({ x: 40 + i * 6, z: side * 2 }));
        runStroke(field, "river", points, { river: { width: 12, depth: 5, stratum: "normal" } });
      });
      measure(`${side} 山笔划（长 dt 单点收敛）`, () => {
        runStroke(field, "mountain", [{ x: side * 2, z: side * 2, dt: 3 }],
          { mountain: { size: 60, strength: 1, type: "hill" } });
      });
      measure(`${side} 16 位编码`, () => {
        const mapping = computeMapping(field);
        encode16Bit(field, mapping);
      });
      measure(`${side} 重采样 2m→4m`, () => {
        resample(field, 4);
      });
    }
    expect(true).toBe(true);
  });
});
