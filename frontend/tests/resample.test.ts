/* 重采样、导出网格解析与 16 位映射的验证。 */
import { describe, expect, it } from "vitest";
import { HeightField } from "../src/core/heightfield";
import { computeMapping, encode16Bit, resolveExportGrid, resample, verticesForSize } from "../src/core/resample";
import { makeField } from "./helpers";

describe("重采样", () => {
  it("降采样保持平均值与尺寸不变", () => {
    const field = makeField(101, 101, 1, () => 7);
    const down = resample(field, 2);
    expect(down.spacing).toBe(2);
    expect(down.sizeWidth).toBeLessThanOrEqual(field.sizeWidth);
    expect(down.sizeWidth).toBeGreaterThan(field.sizeWidth - 2);
    let sum = 0;
    for (const value of down.heights) sum += value;
    expect(sum / down.heights.length).toBeCloseTo(7, 3);
  });

  it("加密重采样不超出原尺寸且保持取值范围", () => {
    const field = makeField(51, 51, 2, (x, z) => x + z);
    const up = resample(field, 1);
    expect(up.sizeWidth).toBeLessThanOrEqual(field.sizeWidth);
    for (const value of up.heights) {
      expect(value).toBeGreaterThanOrEqual(0);
      expect(value).toBeLessThanOrEqual(100 + 1e-6);
    }
  });

  it("verticesForSize 向下取整保证网格一致", () => {
    expect(verticesForSize(500, 2)).toBe(251);
    expect(verticesForSize(501, 2)).toBe(251);
    expect((verticesForSize(501, 2) - 1) * 2).toBe(500);
  });
});

describe("导出网格解析", () => {
  it("不允许小于编辑间距的导出", () => {
    const field = makeField(501, 501, 2);
    expect(() => resolveExportGrid(field, 1)).toThrow();
  });

  it("整除时实际间距等于请求间距", () => {
    const field = makeField(501, 501, 2); // 1000 米
    const grid = resolveExportGrid(field, 4);
    expect(grid.width).toBe(251);
    expect(grid.depth).toBe(251);
    expect(grid.spacing).toBeCloseTo(4, 6);
    expect(grid.note).toBe("");
  });

  it("不能整除时明确调整并给出说明", () => {
    const field = makeField(501, 501, 2); // 1000 米
    const grid = resolveExportGrid(field, 3);
    expect(grid.width).toBe(334); // floor(1000/3)=333 → 334 顶点
    expect(grid.spacing).toBeCloseTo(1000 / 333, 6);
    expect(grid.spacing).toBeGreaterThanOrEqual(3); // 向下取整使实际间距略增。
    expect(grid.spacing).toBeLessThan(6);
    expect(grid.note).not.toBe("");
  });

  it("平坦地形也能解析与导出", () => {
    const field = makeField(101, 101, 2);
    const grid = resolveExportGrid(field, 2);
    const mapping = computeMapping(field);
    expect(mapping.flat).toBe(true);
    const samples = encode16Bit(field, mapping);
    expect(samples[0]).toBe(32768);
    expect(grid.width).toBe(101);
  });
});

describe("16 位映射", () => {
  it("线性映射端点：最低为 0、最高为 65535", () => {
    const field = makeField(11, 11, 1, (x) => x * 10);
    const mapping = computeMapping(field);
    expect(mapping.min).toBe(0);
    expect(mapping.max).toBe(100);
    const samples = encode16Bit(field, mapping);
    expect(samples[0]).toBe(0);
    expect(samples[10]).toBe(65535);
    // 中点值落在中位数附近。
    expect(samples[5]).toBeGreaterThan(32000);
    expect(samples[5]).toBeLessThan(33536);
  });

  it("负高度范围正确映射", () => {
    const field = makeField(11, 11, 1, (x) => x * 5 - 25);
    const mapping = computeMapping(field);
    expect(mapping.min).toBe(-25);
    expect(mapping.max).toBe(25);
    const samples = encode16Bit(field, mapping);
    expect(samples[0]).toBe(0);
    expect(samples[10]).toBe(65535);
  });
});
