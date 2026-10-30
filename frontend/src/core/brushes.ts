/**
 * 画笔算法（CPU 实现，数据层与界面解耦）。
 * 通用规则：
 *  - 所有笔刷只通过事务写入高度，保证一次笔划可精确撤销/重做；
 *  - 持续作用的笔刷按时间（dt，秒）归一化，效果不随渲染帧率变化；
 *  - 河流/湖泊的“目标深度”以笔划开始前的原始地面（事务的 originalAt）为基准，
 *    同一笔划反复覆盖不会因地面已挖低而无限加深；新笔划以当前地形为新基准；
 *  - 画笔大小以世界空间米为单位，不随相机距离变化。
 */
import {
  clamp, clampHeight, HeightField, smoothFalloff, smoothStep, valueNoise
} from "./heightfield";
import { StrokeTransaction } from "./undo";

/** 画笔工具标识，与左侧工具栏一一对应。 */
export type ToolId =
  | "detail" | "adjust" | "mountain" | "river" | "road" | "lake" | "coast";

/** 调整画笔模式。 */
export type AdjustMode = "smooth" | "flatten" | "sharpen";
/** 山体预设。 */
export type MountainType = "hill" | "peak" | "ridge" | "mesa";
/** 地层类型：影响河/湖岸坡宽度与坡度、湖盆截面。 */
export type StratumType = "soft" | "normal" | "hard";
/** 路基模式。 */
export type RoadBedMode = "gentle" | "steep";
/** 海岸类型。 */
export type CoastType = "sand" | "dirt" | "rock";

/** 各工具的参数集合（界面直接读写）。 */
export interface BrushSettings {
  detail: { size: number; strength: number; invert: boolean };
  adjust: { size: number; strength: number; mode: AdjustMode };
  mountain: { size: number; strength: number; type: MountainType };
  river: { width: number; depth: number; stratum: StratumType };
  road: { width: number; bed: RoadBedMode };
  lake: { size: number; depth: number; stratum: StratumType };
  coast: { size: number; strength: number; seaLevel: number; type: CoastType };
}

/** 默认参数。 */
export function defaultBrushSettings(): BrushSettings {
  return {
    detail: { size: 20, strength: 24, invert: false },
    adjust: { size: 24, strength: 0.6, mode: "smooth" },
    mountain: { size: 60, strength: 0.7, type: "hill" },
    river: { width: 12, depth: 4, stratum: "normal" },
    road: { width: 8, bed: "gentle" },
    lake: { size: 60, depth: 6, stratum: "normal" },
    coast: { size: 80, strength: 0.7, seaLevel: 0, type: "sand" }
  };
}

/** 单次落笔的上下文。 */
export interface StampContext {
  /** 落笔中心（世界坐标，米）。 */
  centerX: number;
  centerZ: number;
  /** 本次落笔的作用时长（秒），已按路径长度分摊。 */
  dt: number;
  /** 绘制方向（单位向量），山脊、道路需要。 */
  dirX: number;
  dirZ: number;
}

/** 笔划级别的共享状态。 */
export interface StrokeEnvironment {
  field: HeightField;
  transaction: StrokeTransaction;
  /** 平整画笔的目标高度（笔划开始时落笔中心地面高度）。 */
  flattenTarget: number;
  /** 笔划内的临时累积表（山体已抬升量等），笔划结束后丢弃。 */
  scratch: Map<number, number>;
  /** 海岸画笔的反馈信息（例如未检测到岸线）。 */
  hint: (message: string) => void;
}

/** 单次落笔：按工具把高度变化写入事务。 */
export function applyStamp(tool: ToolId, settings: BrushSettings, env: StrokeEnvironment,
                           ctx: StampContext): void {
  switch (tool) {
    case "detail": stampDetail(env, settings.detail, ctx); break;
    case "adjust": stampAdjust(env, settings.adjust, ctx); break;
    case "mountain": stampMountain(env, settings.mountain, ctx); break;
    case "river": stampRiver(env, settings.river, ctx); break;
    case "road": stampRoad(env, settings.road, ctx); break;
    case "lake": stampLake(env, settings.lake, ctx); break;
    case "coast": stampCoast(env, settings.coast, ctx); break;
  }
}

// ---- 细节画笔：抬高 / 降低（Alt 反向） ----

function stampDetail(env: StrokeEnvironment, params: BrushSettings["detail"], ctx: StampContext): void {
  const direction = params.invert ? -1 : 1;
  env.field.forEachInDisc(ctx.centerX, ctx.centerZ, params.size / 2, (x, z, weight) => {
    const delta = direction * params.strength * ctx.dt * weight;
    env.transaction.write(env.field, x, z, env.field.heights[env.field.index(x, z)] + delta);
  });
}

// ---- 调整画笔：平滑 / 平整 / 锐化 ----

function stampAdjust(env: StrokeEnvironment, params: BrushSettings["adjust"], ctx: StampContext): void {
  const field = env.field;
  const radius = params.size / 2;
  if (params.mode === "flatten") {
    // 目标高度在整个笔划期间固定（笔划开始时采集），不会随拖动漂移。
    field.forEachInDisc(ctx.centerX, ctx.centerZ, radius, (x, z, weight) => {
      const current = field.heights[field.index(x, z)];
      const blended = current + (env.flattenTarget - current) * Math.min(1, params.strength * 3 * ctx.dt) * weight;
      env.transaction.write(field, x, z, blended);
    });
    return;
  }
  field.forEachInDisc(ctx.centerX, ctx.centerZ, radius, (x, z, weight) => {
    const index = field.index(x, z);
    const current = field.heights[index];
    const average = discAverage(field, x, z, radius);
    if (params.mode === "smooth") {
      const blended = current + (average - current) * Math.min(1, params.strength * 4 * ctx.dt) * weight;
      env.transaction.write(field, x, z, blended);
    } else {
      // 锐化：增强已有起伏（细节项），单次变化限幅，避免数值失控与孤立尖刺。
      const detail = current - average;
      let delta = detail * params.strength * 2.5 * ctx.dt * weight;
      delta = clamp(delta, -1.5, 1.5);
      env.transaction.write(field, x, z, current + delta);
    }
  });
}

/** 以格点为中心、radius（格）范围的加权平均高度，用于平滑与锐化。 */
function discAverage(field: HeightField, x: number, z: number, radiusMeters: number): number {
  const reach = Math.max(1, Math.round(radiusMeters / field.spacing));
  let sum = 0;
  let weight = 0;
  const reachSq = reach * reach;
  for (let dz = -reach; dz <= reach; ++dz) {
    for (let dx = -reach; dx <= reach; ++dx) {
      const distSq = dx * dx + dz * dz;
      if (distSq > reachSq) continue;
      const index = field.index(x + dx, z + dz);
      if (index < 0) continue;
      const w = 1 / (1 + distSq);
      sum += field.heights[index] * w;
      weight += w;
    }
  }
  return weight > 0 ? sum / weight : field.heights[field.index(x, z)];
}

// ---- 山画笔：四种可辨认的预设 ----

/** 山体预设的高度包络与幅度上限比例（相对画笔半径），上限另受 150 米硬顶约束。 */
function mountainProfile(type: MountainType): { shape: (u: number) => number; cap: number } {
  switch (type) {
    case "hill": // 圆润丘陵：柔和穹顶，山坡平缓
      return { shape: (u) => Math.pow((Math.cos(clamp(u, 0, 1) * Math.PI) + 1) / 2, 1.15), cap: 0.42 };
    case "peak": // 尖峰山地：中心突出、坡面陡峭
      return { shape: (u) => Math.pow(1 - clamp(u, 0, 1), 3), cap: 1.15 };
    case "ridge": // 山脊：沿绘制方向狭长连续隆起（由调用方按方向压缩距离）
      return { shape: (u) => Math.pow(1 - clamp(u, 0, 1), 2), cap: 0.55 };
    case "mesa": // 台地：平顶、边缘陡峭
      return { shape: (u) => smoothStep(0.95, 0.62, u), cap: 0.5 };
  }
}

function stampMountain(env: StrokeEnvironment, params: BrushSettings["mountain"], ctx: StampContext): void {
  const field = env.field;
  const isRidge = params.type === "ridge";
  const radius = params.size / 2;
  // 山脊沿绘制方向拉伸 2.6 倍，其余类型为圆形。
  const reach = isRidge ? radius * 2.6 : radius;
  const { shape, cap } = mountainProfile(params.type);
  const amplitude = Math.min(150, cap * radius) * clamp(params.strength, 0.05, 1);
  const stretch = reach / radius;

  field.forEachInDisc(ctx.centerX, ctx.centerZ, reach, (x, z, _weight, _distance) => {
    const index = field.index(x, z);
    if (index < 0) return;
    const worldX = x * field.spacing;
    const worldZ = z * field.spacing;
    let dx = worldX - ctx.centerX;
    let dz = worldZ - ctx.centerZ;
    if (isRidge) {
      // 旋转到绘制方向坐标系：纵向拉伸、横向保持。
      const along = dx * ctx.dirX + dz * ctx.dirZ;
      const across = -dx * ctx.dirZ + dz * ctx.dirX;
      dx = along / stretch;
      dz = across;
    }
    const u = Math.min(1, Math.hypot(dx, dz) / radius);
    // 固定种子的空间噪声：幅度 ±15%，坡面细部稳定不跳变。
    const noise = 0.85 + 0.3 * valueNoise(worldX / 24, worldZ / 24, 197);
    const desired = amplitude * shape(u) * (u < 1 ? noise : 1);
    const added = env.scratch.get(index) ?? 0;
    // 向目标包络渐进抬升，重复落笔不叠加超过包络。
    const grow = (desired - added) * (1 - Math.exp(-3.2 * ctx.dt));
    if (grow <= 0) return;
    env.scratch.set(index, added + grow);
    env.transaction.write(field, x, z, field.heights[index] + grow);
  });
}

// ---- 河流画笔：挖出河床与河岸 ----

/** 地层参数：岸坡相对河宽的宽度比例与截面指数（松软宽缓 / 坚硬陡峭）。 */
function stratumProfile(stratum: StratumType): { bankRatio: number; bedExp: number; bankExp: number } {
  switch (stratum) {
    case "soft": return { bankRatio: 2.1, bedExp: 0.5, bankExp: 1.4 };
    case "normal": return { bankRatio: 1.3, bedExp: 0.7, bankExp: 1.8 };
    case "hard": return { bankRatio: 0.75, bedExp: 0.9, bankExp: 2.4 };
  }
}

/** 河/湖挖掘的通用应用规则：只向下逼近目标，重复覆盖不再加深。 */
function easeDown(env: StrokeEnvironment, x: number, z: number, target: number, dt: number): void {
  const index = env.field.index(x, z);
  const current = env.field.heights[index];
  if (target >= current) return; // 目标不比当前低时不修改，保证只挖不填。
  const lowered = current - (current - target) * (1 - Math.exp(-9 * dt));
  env.transaction.write(env.field, x, z, Math.max(target, lowered));
}

function stampRiver(env: StrokeEnvironment, params: BrushSettings["river"], ctx: StampContext): void {
  const field = env.field;
  const { bankRatio, bedExp, bankExp } = stratumProfile(params.stratum);
  const bedHalf = Math.max(field.spacing, params.width / 2);
  const bankWidth = Math.max(field.spacing, params.width * bankRatio / 2);
  const reach = bedHalf + bankWidth;

  field.forEachInDisc(ctx.centerX, ctx.centerZ, reach, (x, z, _weight, distance) => {
    const index = field.index(x, z);
    if (index < 0 || distance > reach) return;
    // 横截面：中心为平缓河床，向外过渡为河岸，岸坡宽度与坡度由地层决定。
    const t = distance;
    let shape: number;
    if (t <= bedHalf) {
      // 河床：中央平缓、略有弧度，指数由地层控制。
      shape = 1 - Math.pow(t / bedHalf, 2 * bedExp) * 0.25;
    } else {
      const bank = (t - bedHalf) / bankWidth;
      shape = Math.pow(1 - Math.pow(Math.min(1, bank), bankExp), 1.6) * 0.75;
    }
    if (shape <= 0) return;
    const original = env.transaction.originalAt(field, x, z);
    easeDown(env, x, z, original - params.depth * shape, ctx.dt);
  });
}

// ---- 道路画笔：横向平整、纵向随原地形 ----

function stampRoad(env: StrokeEnvironment, params: BrushSettings["road"], ctx: StampContext): void {
  const field = env.field;
  const halfPavement = Math.max(field.spacing, params.width / 2);
  // 平缓路基的边坡更宽；陡直路基更窄更陡。边坡至少两个采样格，避免出现垂直面。
  const shoulder = Math.max(field.spacing * 2,
    params.width * (params.bed === "gentle" ? 0.95 : 0.28));
  const reach = halfPavement + shoulder;

  // 纵向参考高度：从笔划开始前的地形沿方向采样并平均，避免被刚生成的路基影响。
  const original = env.transaction.originalAt.bind(env.transaction);
  const sampleOriginal = (offsetMeters: number): number => {
    const wx = ctx.centerX + ctx.dirX * offsetMeters;
    const wz = ctx.centerZ + ctx.dirZ * offsetMeters;
    const [gx, gz] = field.worldToGrid(wx, wz);
    return original(field, Math.round(gx), Math.round(gz));
  };
  const span = Math.max(halfPavement, field.spacing);
  let reference = 0;
  let samples = 0;
  for (let offset = -span; offset <= span; offset += span / 3) {
    reference += sampleOriginal(offset);
    samples += 1;
  }
  reference /= Math.max(1, samples);

  field.forEachInDisc(ctx.centerX, ctx.centerZ, reach, (x, z, _weight, distance) => {
    const index = field.index(x, z);
    if (index < 0) return;
    const dx = x * field.spacing - ctx.centerX;
    const dz = z * field.spacing - ctx.centerZ;
    const across = Math.abs(-dx * ctx.dirZ + dz * ctx.dirX);
    if (across > reach) return;
    let target: number;
    if (across <= halfPavement) {
      target = reference; // 路面横向保持水平。
    } else {
      const bank = (across - halfPavement) / shoulder;
      const ground = original(field, x, z);
      target = reference + (ground - reference) * smoothStep(0, 1, bank);
    }
    // 路基既可填也可挖，向目标渐进；目标源自笔划前地形，重复覆盖稳定。
    const currentIndex = field.index(x, z);
    const current = field.heights[currentIndex];
    const moved = current + (target - current) * (1 - Math.exp(-10 * ctx.dt));
    env.transaction.write(field, x, z, moved);
  });
}

// ---- 湖泊画笔：连续湖盆与岸坡 ----

function stampLake(env: StrokeEnvironment, params: BrushSettings["lake"], ctx: StampContext): void {
  const field = env.field;
  const { bankRatio, bedExp } = stratumProfile(params.stratum);
  const basinRadius = Math.max(field.spacing * 2, params.size / 2);
  const shoreWidth = basinRadius * bankRatio * 0.8;
  const reach = basinRadius + shoreWidth;

  field.forEachInDisc(ctx.centerX, ctx.centerZ, reach, (x, z, _weight, distance) => {
    const index = field.index(x, z);
    if (index < 0) return;
    let shape: number;
    if (distance <= basinRadius) {
      // 湖盆：底部柔和（余弦形），与河槽的抛物线截面区分。
      shape = Math.pow(Math.cos(Math.min(1, distance / basinRadius) * Math.PI / 2), 0.75 + bedExp * 0.2);
    } else {
      const bank = (distance - basinRadius) / shoreWidth;
      shape = Math.pow(1 - Math.pow(Math.min(1, bank), 1.9), 1.7) * 0.35;
    }
    if (shape <= 0) return;
    const ground = env.transaction.originalAt(field, x, z);
    easeDown(env, x, z, ground - params.depth * shape, ctx.dt);
  });
}

// ---- 海岸画笔：以海平面为参考重塑岸坡 ----

function stampCoast(env: StrokeEnvironment, params: BrushSettings["coast"], ctx: StampContext): void {
  const field = env.field;
  const radius = Math.max(field.spacing * 2, params.size / 2);
  // 收集笔划开始前的局部高低参考，用于判断是否存在可识别的岸线过渡。
  let low = Number.POSITIVE_INFINITY;
  let high = Number.NEGATIVE_INFINITY;
  field.forEachInDisc(ctx.centerX, ctx.centerZ, radius, (x, z) => {
    const ground = env.transaction.originalAt(field, x, z);
    if (ground < low) low = ground;
    if (ground > high) high = ground;
  });
  if (!Number.isFinite(low) || !Number.isFinite(high)
    || high - low < field.spacing * 0.5 || low > params.seaLevel || high < params.seaLevel) {
    env.hint("画笔范围内未检测到海平面附近的岸坡过渡，本次绘制未修改地形。");
    return;
  }
  const range = high - low;
  // 以“到岸线的水平距离”建模：x ≈ (h0-海平面)×半径/高差，目标岸坡按特征坡度
  // 重塑；坡度相对本地平均坡度缩放（沙缓、岩陡），低点与高点保持不变。
  const localSlope = range / radius;
  const slopeFactor = params.type === "sand" ? 0.4 : params.type === "dirt" ? 0.8 : 1.8;
  const targetSlope = localSlope * slopeFactor;
  field.forEachInDisc(ctx.centerX, ctx.centerZ, radius, (x, z, weight) => {
    const index = field.index(x, z);
    if (index < 0) return;
    const ground = env.transaction.originalAt(field, x, z);
    const distanceToShore = (ground - params.seaLevel) * radius / range;
    let target = params.seaLevel + distanceToShore * targetSlope;
    target = Math.min(high, Math.max(low, target));
    const current = field.heights[index];
    const moved = current + (target - current)
      * Math.min(1, 2.2 * Math.max(0.05, params.strength) * ctx.dt) * weight;
    env.transaction.write(field, x, z, clampHeight(moved));
  });
}
