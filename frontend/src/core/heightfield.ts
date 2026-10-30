/**
 * 规则高度场：地形编辑的唯一权威数据源。
 * 坐标约定：
 *  - 网格列 x 对应世界 +X（东），行 z 对应世界 +Z（南），高度向上为正，单位米；
 *  - 世界尺寸 = (顶点数-1) × 采样间距，地形尺寸始终与采样网格一致；
 *  - 所有画笔、拾取、保存与导出都基于同一份 Float32Array。
 */

/** 高度数值的安全范围（米），与 Qt 服务端的写入校验保持一致。 */
export const MAX_ABSOLUTE_HEIGHT = 10000;

/** 规则高度场，内部以 Float32Array 行主序存储。 */
export class HeightField {
  readonly gridWidth: number;
  readonly gridDepth: number;
  readonly spacing: number;
  /** 实际地形宽度（米），= (gridWidth-1)*spacing。 */
  readonly sizeWidth: number;
  /** 实际地形长度（米）。 */
  readonly sizeDepth: number;
  readonly heights: Float32Array;

  constructor(gridWidth: number, gridDepth: number, spacing: number, heights?: Float32Array) {
    if (!Number.isFinite(gridWidth) || !Number.isFinite(gridDepth) || !Number.isFinite(spacing)
      || gridWidth < 2 || gridDepth < 2 || spacing <= 0) {
      throw new Error("高度场网格参数无效。");
    }
    this.gridWidth = Math.floor(gridWidth);
    this.gridDepth = Math.floor(gridDepth);
    this.spacing = spacing;
    this.sizeWidth = (this.gridWidth - 1) * spacing;
    this.sizeDepth = (this.gridDepth - 1) * spacing;
    const expected = this.gridWidth * this.gridDepth;
    if (heights && heights.length === expected) {
      this.heights = heights;
    } else {
      this.heights = new Float32Array(expected);
    }
  }

  /** 把网格坐标转换为线性索引；越界时返回 -1。 */
  index(x: number, z: number): number {
    if (x < 0 || z < 0 || x >= this.gridWidth || z >= this.gridDepth) return -1;
    return z * this.gridWidth + x;
  }

  /** 读取高度；越界时使用边缘钳制采样，避免滤波与插值引入虚假的 0。 */
  get(x: number, z: number): number {
    const column = Math.min(this.gridWidth - 1, Math.max(0, Math.round(x)));
    const row = Math.min(this.gridDepth - 1, Math.max(0, Math.round(z)));
    return this.heights[row * this.gridWidth + column];
  }

  /** 安全写入高度，并钳制到安全范围；越界时忽略。 */
  set(x: number, z: number, value: number): void {
    const index = this.index(x, z);
    if (index < 0) return;
    this.heights[index] = clampHeight(value);
  }

  /** 世界坐标 → 网格浮点坐标。 */
  worldToGrid(wx: number, wz: number): [number, number] {
    return [wx / this.spacing, wz / this.spacing];
  }

  /** 网格浮点坐标 → 世界坐标。 */
  gridToWorld(gx: number, gz: number): [number, number] {
    return [gx * this.spacing, gz * this.spacing];
  }

  /** 双线性插值采样（浮点网格坐标），用于拾取与法线计算。 */
  sampleBilinear(gx: number, gz: number): number {
    const x0 = Math.floor(gx);
    const z0 = Math.floor(gz);
    const fx = gx - x0;
    const fz = gz - z0;
    const h00 = this.get(x0, z0);
    const h10 = this.get(x0 + 1, z0);
    const h01 = this.get(x0, z0 + 1);
    const h11 = this.get(x0 + 1, z0 + 1);
    return (h00 * (1 - fx) + h10 * fx) * (1 - fz) + (h01 * (1 - fx) + h11 * fx) * fz;
  }

  /** 世界坐标高度采样。 */
  sampleWorld(wx: number, wz: number): number {
    const [gx, gz] = this.worldToGrid(wx, wz);
    return this.sampleBilinear(gx, gz);
  }

  /** 遍历以世界坐标为中心、radius 为半径的圆盘内的网格点，回调收到平滑衰减权重。 */
  forEachInDisc(centerWx: number, centerWz: number, radius: number,
    visit: (x: number, z: number, weight: number, distance: number) => void): void {
    if (!(radius > 0)) return;
    const [cgx, cgz] = this.worldToGrid(centerWx, centerWz);
    const reach = radius / this.spacing;
    const x0 = Math.max(0, Math.floor(cgx - reach));
    const x1 = Math.min(this.gridWidth - 1, Math.ceil(cgx + reach));
    const z0 = Math.max(0, Math.floor(cgz - reach));
    const z1 = Math.min(this.gridDepth - 1, Math.ceil(cgz + reach));
    const radiusSq = reach * reach;
    for (let z = z0; z <= z1; ++z) {
      for (let x = x0; x <= x1; ++x) {
        const dx = x - cgx;
        const dz = z - cgz;
        const distSq = dx * dx + dz * dz;
        if (distSq > radiusSq) continue;
        const u = Math.sqrt(distSq) / reach; // 0 中心 → 1 边缘
        visit(x, z, smoothFalloff(u), Math.sqrt(distSq) * this.spacing);
      }
    }
  }
}

/** 把高度钳制到安全范围，并拦截 NaN 与无穷大。 */
export function clampHeight(value: number): number {
  if (!Number.isFinite(value)) return 0;
  return Math.min(MAX_ABSOLUTE_HEIGHT, Math.max(-MAX_ABSOLUTE_HEIGHT, value));
}

/** 平滑衰减曲线：u∈[0,1]（0 中心，1 边缘），返回 1→0 的平滑权重。 */
export function smoothFalloff(u: number): number {
  const t = Math.min(1, Math.max(0, 1 - u));
  return t * t * (3 - 2 * t); // smoothstep
}

/** 三次 smoothstep，a→b 映射。 */
export function smoothStep(edge0: number, edge1: number, x: number): number {
  const t = Math.min(1, Math.max(0, (x - edge0) / (edge1 - edge0)));
  return t * t * (3 - 2 * t);
}

/** 把数值限制在 [min,max]。 */
export function clamp(value: number, min: number, max: number): number {
  return Math.min(max, Math.max(min, value));
}

/**
 * 稳定的二维值噪声：固定种子、按世界坐标取整格点插值，
 * 用于山体画笔的自然变化；同一位置反复采样结果一致，不会随绘制跳变。
 */
export function valueNoise(wx: number, wz: number, seed: number): number {
  const hash = (ix: number, iz: number): number => {
    let h = ix * 374761393 + iz * 668265263 + seed * 1442695040;
    h = (h ^ (h >>> 13)) >>> 0;
    h = Math.imul(h, 1274126177) >>> 0;
    return ((h ^ (h >>> 16)) >>> 0) / 4294967295;
  };
  const ix = Math.floor(wx);
  const iz = Math.floor(wz);
  const fx = wx - ix;
  const fz = wz - iz;
  const sx = fx * fx * (3 - 2 * fx);
  const sz = fz * fz * (3 - 2 * fz);
  const a = hash(ix, iz);
  const b = hash(ix + 1, iz);
  const c = hash(ix, iz + 1);
  const d = hash(ix + 1, iz + 1);
  return (a * (1 - sx) + b * sx) * (1 - sz) + (c * (1 - sx) + d * sx) * sz;
}
