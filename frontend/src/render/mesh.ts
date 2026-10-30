/**
 * 地形三维网格：BufferGeometry 构建、局部顶点/法线/颜色更新。
 * 局部绘制只刷新受影响的行区间，避免每次鼠标事件重建整张网格。
 */
import * as THREE from "three";
import { HeightField } from "../core/heightfield";

/** 一段网格矩形（顶点坐标，含端点）。 */
export interface GridRect {
  x0: number;
  z0: number;
  x1: number;
  z1: number;
}

/** 由高度场构建地形网格几何体；索引与静态属性只构建一次。 */
export function buildTerrainGeometry(field: HeightField, min = 0, max = 1): THREE.BufferGeometry {
  const geometry = new THREE.BufferGeometry();
  const vertexCount = field.gridWidth * field.gridDepth;
  const positions = new Float32Array(vertexCount * 3);
  const normals = new Float32Array(vertexCount * 3);
  const colors = new Float32Array(vertexCount * 3);
  for (let z = 0; z < field.gridDepth; ++z) {
    for (let x = 0; x < field.gridWidth; ++x) {
      const index = z * field.gridWidth + x;
      positions[index * 3] = x * field.spacing;
      positions[index * 3 + 1] = field.heights[index];
      positions[index * 3 + 2] = z * field.spacing;
    }
  }
  geometry.setAttribute("position", new THREE.BufferAttribute(positions, 3));
  geometry.setAttribute("normal", new THREE.BufferAttribute(normals, 3));
  geometry.setAttribute("color", new THREE.BufferAttribute(colors, 3));
  // 三角形索引：WebGL2 下使用 Uint32 支持百万级顶点。
  const quadCount = (field.gridWidth - 1) * (field.gridDepth - 1);
  const indices = new Uint32Array(quadCount * 6);
  let cursor = 0;
  for (let z = 0; z < field.gridDepth - 1; ++z) {
    for (let x = 0; x < field.gridWidth - 1; ++x) {
      const a = z * field.gridWidth + x;
      const b = a + 1;
      const c = a + field.gridWidth;
      const d = c + 1;
      indices[cursor++] = a; indices[cursor++] = c; indices[cursor++] = b;
      indices[cursor++] = b; indices[cursor++] = c; indices[cursor++] = d;
    }
  }
  geometry.setIndex(new THREE.BufferAttribute(indices, 1));
  geometry.boundingSphere = new THREE.Sphere(
    new THREE.Vector3(field.sizeWidth / 2, 0, field.sizeDepth / 2),
    Math.hypot(field.sizeWidth, field.sizeDepth) * 1.5 + 100);
  refreshRegion(field, geometry,
    { x0: 0, z0: 0, x1: field.gridWidth - 1, z1: field.gridDepth - 1 }, min, max);
  return geometry;
}

/** 用当前高度范围整体重着色：范围变化后避免只刷新局部出现矩形色差接缝。 */
export function refreshAllColors(field: HeightField, geometry: THREE.BufferGeometry,
                                 min: number, max: number): void {
  const color = geometry.getAttribute("color") as THREE.BufferAttribute;
  const tint = new THREE.Color();
  const total = field.gridWidth * field.gridDepth;
  for (let index = 0; index < total; ++index) {
    heightColor(field.heights[index], min, max, tint);
    color.setXYZ(index, tint.r, tint.g, tint.b);
  }
  if (typeof color.clearUpdateRanges === "function") color.clearUpdateRanges();
  if (typeof color.addUpdateRange === "function") color.addUpdateRange(0, total * 3);
  color.needsUpdate = true;
}

/** 按高度映射顶点颜色：低处草绿 → 中部土褐 → 高处岩灰，帮助阅读地形形状。 */
function heightColor(height: number, min: number, max: number, out: THREE.Color): void {
  const t = max > min ? (height - min) / (max - min) : 0.5;
  if (t < 0.5) {
    out.setRGB(0.32 + t * 0.5, 0.45 + t * 0.25, 0.2 + t * 0.12);
  } else {
    const u = (t - 0.5) * 2;
    out.setRGB(0.57 + u * 0.33, 0.57 - u * 0.2, 0.32 - u * 0.14);
  }
}

/**
 * 局部刷新矩形区域的顶点高度、法线与颜色。
 * 法线需要相邻一圈高度，因此刷新范围外扩一格。
 */
export function refreshRegion(field: HeightField, geometry: THREE.BufferGeometry,
                              rect: GridRect, min = 0, max = 1): void {
  const position = geometry.getAttribute("position") as THREE.BufferAttribute;
  const normal = geometry.getAttribute("normal") as THREE.BufferAttribute;
  const color = geometry.getAttribute("color") as THREE.BufferAttribute;
  const x0 = Math.max(0, rect.x0 - 1);
  const z0 = Math.max(0, rect.z0 - 1);
  const x1 = Math.min(field.gridWidth - 1, rect.x1 + 1);
  const z1 = Math.min(field.gridDepth - 1, rect.z1 + 1);
  const tint = new THREE.Color();
  const face = new THREE.Vector3();
  for (let z = z0; z <= z1; ++z) {
    for (let x = x0; x <= x1; ++x) {
      const index = z * field.gridWidth + x;
      const height = field.heights[index];
      position.setXYZ(index, x * field.spacing, height, z * field.spacing);
      const left = field.get(x - 1, z);
      const right = field.get(x + 1, z);
      const up = field.get(x, z - 1);
      const down = field.get(x, z + 1);
      face.set(left - right, 2 * field.spacing, up - down).normalize();
      normal.setXYZ(index, face.x, face.y, face.z);
      heightColor(height, min, max, tint);
      color.setXYZ(index, tint.r, tint.g, tint.b);
    }
  }
  markRange(position, x0, x1, z0, z1, field.gridWidth, 3);
  markRange(normal, x0, x1, z0, z1, field.gridWidth, 3);
  markRange(color, x0, x1, z0, z1, field.gridWidth, 3);
}

/** 标记属性的局部更新区间（按行切分），只在受影响区间重新上传显存。 */
function markRange(attribute: THREE.BufferAttribute, x0: number, x1: number,
                   z0: number, z1: number, gridWidth: number, itemSize: number): void {
  if (typeof attribute.clearUpdateRanges === "function") attribute.clearUpdateRanges();
  for (let z = z0; z <= z1; ++z) {
    const startVertex = z * gridWidth + x0;
    const vertexCount = x1 - x0 + 1;
    if (typeof attribute.addUpdateRange === "function") {
      attribute.addUpdateRange(startVertex * itemSize, vertexCount * itemSize);
    } else {
      // 兼容旧接口：整块上传。
      attribute.needsUpdate = true;
      return;
    }
  }
  attribute.needsUpdate = true;
}
