/**
 * 地形拾取：把屏幕射线与当前高度场求交。
 * 使用沿射线的步进加二分细化，直接作用于最新高度数据，
 * 不依赖三维网格的过期包围结果，修改后立即生效。
 */
import * as THREE from "three";
import { HeightField } from "../core/heightfield";

/** 拾取结果：世界坐标交点与网格坐标。 */
export interface PickResult {
  point: THREE.Vector3;
  gridX: number;
  gridZ: number;
}

/** 把 NDC 坐标转换为世界射线（基于当前相机）。 */
export function rayFromPointer(camera: THREE.Camera, ndcX: number, ndcY: number): THREE.Ray {
  const origin = new THREE.Vector3(ndcX, ndcY, 0.5).unproject(camera);
  const direction = origin.clone().sub(camera.position).normalize();
  return new THREE.Ray(camera.position.clone(), direction);
}

/** 射线与包围盒的进入/离开参数区间（slab 法）；不相交返回 null。 */
function rayBoxInterval(ray: THREE.Ray, box: THREE.Box3): [number, number] | null {
  let tMin = Number.NEGATIVE_INFINITY;
  let tMax = Number.POSITIVE_INFINITY;
  const origin = ray.origin;
  const direction = ray.direction;
  const axes: Array<"x" | "y" | "z"> = ["x", "y", "z"];
  for (const axis of axes) {
    const slabMin = box.min[axis];
    const slabMax = box.max[axis];
    if (Math.abs(direction[axis]) < 1e-12) {
      // 射线与该轴的板平行：起点在板外则无交点。
      if (origin[axis] < slabMin || origin[axis] > slabMax) return null;
      continue;
    }
    const inverse = 1 / direction[axis];
    let t1 = (slabMin - origin[axis]) * inverse;
    let t2 = (slabMax - origin[axis]) * inverse;
    if (t1 > t2) { const swap = t1; t1 = t2; t2 = swap; }
    if (t1 > tMin) tMin = t1;
    if (t2 < tMax) tMax = t2;
    if (tMin > tMax) return null;
  }
  // 相机可能位于包围盒内部：进入点钳制到 0。
  const entry = Math.max(tMin, 0);
  if (tMax < entry) return null;
  return [entry, tMax];
}

/** 求射线与高度场的交点；未命中返回 null。 */
export function pickHeightField(ray: THREE.Ray, field: HeightField,
                                minHint: number, maxHint: number): PickResult | null {
  // 地形包围盒（高度范围留出余量，覆盖编辑中的起伏）。
  const pad = Math.max(4, (maxHint - minHint) * 0.1 + field.spacing * 4);
  const box = new THREE.Box3(
    new THREE.Vector3(-1, minHint - pad, -1),
    new THREE.Vector3(field.sizeWidth + 1, maxHint + pad, field.sizeDepth + 1));
  const interval = rayBoxInterval(ray, box);
  if (!interval) return null;
  const [entry, exit] = interval;
  const length = exit - entry;
  if (length <= 0) return null;
  const step = Math.max(field.spacing * 0.5, length / 4096);
  const direction = ray.direction.clone().normalize();
  const origin = ray.origin.clone();
  const cursor = new THREE.Vector3();
  for (let t = step; t <= length; t += step) {
    cursor.copy(origin).addScaledVector(direction, entry + t);
    const heightGround = field.sampleWorld(cursor.x, cursor.z);
    if (cursor.y <= heightGround) {
      // 符号从正变负：在段起点（t=0 位于相机处，高于地形）与 t 之间二分细化。
      let low = 0;
      let high = entry + t;
      for (let iteration = 0; iteration < 14; ++iteration) {
        const mid = (low + high) / 2;
        const point = origin.clone().addScaledVector(direction, mid);
        if (point.y > field.sampleWorld(point.x, point.z)) low = mid; else high = mid;
      }
      const point = origin.clone().addScaledVector(direction, (low + high) / 2);
      point.x = Math.min(field.sizeWidth, Math.max(0, point.x));
      point.z = Math.min(field.sizeDepth, Math.max(0, point.z));
      point.y = field.sampleWorld(point.x, point.z);
      const [gridX, gridZ] = field.worldToGrid(point.x, point.z);
      return { point, gridX: Math.round(gridX), gridZ: Math.round(gridZ) };
    }
  }
  return null;
}
