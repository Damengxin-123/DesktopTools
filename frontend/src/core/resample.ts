/**
 * 重采样与高度映射：供“编辑采样间距”设置和 16 位灰度 PNG 导出共用的数据处理模块。
 * 降采样使用盒式面积滤波（避免跳点抽样造成锯齿），加密只做双线性插值且不宣称恢复细节。
 */
import { HeightField } from "./heightfield";

/** 计算新采样间距下的顶点数：向下取整保证网格不超出原范围。 */
export function verticesForSize(sizeMeters: number, spacing: number): number {
  if (!(spacing > 0)) return 2;
  return Math.max(2, Math.floor(sizeMeters / spacing + 1e-9) + 1);
}

/**
 * 把高度场重采样为新的采样间距。
 *  - spacingNew > spacing：盒式降采样（合理滤波）；
 *  - spacingNew < spacing：双线性插值加密（仅提高后续编辑密度，不恢复已有细节）。
 * 实际尺寸 = (新顶点数-1) × spacingNew，可能比原尺寸小不足一个采样间距。
 */
export function resample(field: HeightField, spacingNew: number): HeightField {
  if (!(spacingNew > 0) || !Number.isFinite(spacingNew)) {
    throw new Error("采样间距无效。");
  }
  const width = verticesForSize(field.sizeWidth, spacingNew);
  const depth = verticesForSize(field.sizeDepth, spacingNew);
  const result = new HeightField(width, depth, spacingNew);
  const scaleX = (field.gridWidth - 1) / (width - 1);
  const scaleZ = (field.gridDepth - 1) / (depth - 1);
  const downsample = spacingNew > field.spacing;
  for (let z = 0; z < depth; ++z) {
    for (let x = 0; x < width; ++x) {
      const gx = x * scaleX;
      const gz = z * scaleZ;
      if (downsample) {
        result.heights[z * width + x] = boxSample(field, gx, gz, scaleX, scaleZ);
      } else {
        result.heights[z * width + x] = field.sampleBilinear(gx, gz);
      }
    }
  }
  return result;
}

/**
 * 盒式面积滤波采样：以目标点为中心、覆盖 scaleX×scaleZ 网格范围的加权平均。
 * 面积权重按重叠比例计算，非整数比例时同样平滑，不产生跳点锯齿。
 */
function boxSample(field: HeightField, gx: number, gz: number,
                   scaleX: number, scaleZ: number): number {
  const halfX = Math.max(0.5, scaleX / 2);
  const halfZ = Math.max(0.5, scaleZ / 2);
  const x0 = Math.floor(gx - halfX);
  const x1 = Math.ceil(gx + halfX);
  const z0 = Math.floor(gz - halfZ);
  const z1 = Math.ceil(gz + halfZ);
  let sum = 0;
  let weight = 0;
  for (let z = z0; z <= z1; ++z) {
    const overlapZ = Math.min(gz + halfZ, z + 1) - Math.max(gz - halfZ, z);
    if (overlapZ <= 0) continue;
    for (let x = x0; x <= x1; ++x) {
      const overlapX = Math.min(gx + halfX, x + 1) - Math.max(gx - halfX, x);
      if (overlapX <= 0) continue;
      sum += field.get(x, z) * overlapX * overlapZ;
      weight += overlapX * overlapZ;
    }
  }
  return weight > 0 ? sum / weight : field.sampleBilinear(gx, gz);
}

/** 导出网格的解析结果，供界面显示实际间距与 JSON 说明文件使用。 */
export interface ExportGrid {
  /** 导出图像宽度（列数）。 */
  width: number;
  /** 导出图像高度（行数）。 */
  depth: number;
  /** 实际导出采样间距（米）：与地形范围精确匹配，可能略小于请求值。 */
  spacing: number;
  /** 请求间距被调整的说明；未调整时为空。 */
  note: string;
}

/**
 * 解析导出网格：请求间距必须 ≥ 编辑间距（不允许宣称比编辑数据更高的精度）。
 * 实际间距以宽度方向能整除地形范围为准（向下取整），长度方向随之匹配；
 * 调整结果通过 note 明确回传，不静默产生尺寸误差。
 */
export function resolveExportGrid(field: HeightField, requestedSpacing: number): ExportGrid {
  if (!Number.isFinite(requestedSpacing) || requestedSpacing < field.spacing - 1e-9) {
    throw new Error("导出采样间距不能小于当前编辑采样间距。");
  }
  const lengthX = (field.gridWidth - 1) * field.spacing;
  const cellsX = Math.max(1, Math.floor(lengthX / requestedSpacing + 1e-9));
  const width = cellsX + 1;
  const spacing = lengthX / cellsX;
  const lengthZ = (field.gridDepth - 1) * field.spacing;
  const cellsZ = Math.max(1, Math.floor(lengthZ / spacing + 1e-9));
  const depth = cellsZ + 1;
  const adjusted = Math.abs(spacing - requestedSpacing) > 1e-9;
  const note = adjusted
    ? `请求间距 ${requestedSpacing} 米无法整除地形范围，已调整为 ${spacing.toFixed(3)} 米，`
      + `导出分辨率 ${width}×${depth}。`
    : "";
  return { width, depth, spacing, note };
}

/** 高度映射结果：16 位编码所需的最小、最大值与映射函数。 */
export interface HeightMapping {
  min: number;
  max: number;
  /** 平坦地形（min==max）时映射到中点灰度，避免除零。 */
  flat: boolean;
}

/** 计算导出数据的映射范围（对重采样后的数据取实际最小/最大值）。 */
export function computeMapping(field: HeightField): HeightMapping {
  let min = Number.POSITIVE_INFINITY;
  let max = Number.NEGATIVE_INFINITY;
  for (let index = 0; index < field.heights.length; ++index) {
    const value = field.heights[index];
    if (value < min) min = value;
    if (value > max) max = value;
  }
  if (!Number.isFinite(min) || !Number.isFinite(max)) {
    return { min: 0, max: 0, flat: true };
  }
  return { min, max, flat: max - min < 1e-9 };
}

/**
 * 把高度场按映射范围编码为 16 位行主序样本（0..65535，黑色=最低，白色=最高）。
 * 平坦地形全部映射为中点 32768。返回 Uint16Array 便于分块传输给 Qt 编码 PNG。
 */
export function encode16Bit(field: HeightField, mapping: HeightMapping): Uint16Array {
  const out = new Uint16Array(field.heights.length);
  if (mapping.flat) {
    out.fill(32768);
    return out;
  }
  const scale = 65535 / (mapping.max - mapping.min);
  for (let index = 0; index < field.heights.length; ++index) {
    const normalized = (field.heights[index] - mapping.min) * scale;
    out[index] = Math.max(0, Math.min(65535, Math.round(normalized)));
  }
  return out;
}
