/**
 * 相机控制：右键旋转、中键平移、滚轮缩放。
 * 左键保留给画笔（含 Alt 反向），因此不使用默认的左键旋转方案。
 */
import * as THREE from "three";
import { HeightField } from "../core/heightfield";

export class TerrainCamera {
  /** 环绕目标点（地形上的注视点）。 */
  private target = new THREE.Vector3();
  private yaw = Math.PI * 0.25;
  private pitch = 0.72;
  private distance = 200;
  private minDistance = 4;
  private maxDistance = 8000;

  /** 根据当前状态更新相机位姿。 */
  apply(camera: THREE.PerspectiveCamera, aspect: number): void {
    const sinPitch = Math.sin(this.pitch);
    const offset = new THREE.Vector3(
      Math.cos(this.yaw) * sinPitch, Math.cos(this.pitch), Math.sin(this.yaw) * sinPitch
    ).multiplyScalar(this.distance);
    camera.position.copy(this.target).add(offset);
    camera.lookAt(this.target);
    camera.updateProjectionMatrix();
    void aspect;
  }

  /**
   * 环绕旋转（屏幕像素增量），遵循主流 3D 软件的“抓住地形拖动”方向：
   * 向右拖 → 地形随光标向右转；向下拖 → 抬起视角看到更多顶部。
   */
  rotate(dxPixels: number, dyPixels: number): void {
    this.yaw += dxPixels * 0.005;
    this.pitch = THREE.MathUtils.clamp(this.pitch - dyPixels * 0.005, 0.08, Math.PI / 2 - 0.04);
  }

  /**
   * 平移：内容跟随光标（与地图类软件一致）。
   * 向右拖 → 地形跟着向右移（注视点向左）；向下拖 → 地形向下移（注视点向远处）。
   */
  pan(dxPixels: number, dyPixels: number, viewportHeight: number): void {
    const scale = this.distance * 1.4 / Math.max(1, viewportHeight);
    const forward = new THREE.Vector3(-Math.cos(this.yaw), 0, -Math.sin(this.yaw));
    const right = new THREE.Vector3(-forward.z, 0, forward.x);
    this.target.addScaledVector(right, -dxPixels * scale);
    this.target.addScaledVector(forward, dyPixels * scale);
  }

  /** 缩放（dolly）：factor < 1 拉近。 */
  zoom(factor: number): void {
    this.distance = THREE.MathUtils.clamp(this.distance * factor,
      this.minDistance, this.maxDistance);
  }

  /** 调整缩放范围以匹配地形尺寸。 */
  setBounds(field: HeightField): void {
    this.minDistance = Math.max(2, field.spacing * 2);
    this.maxDistance = Math.max(field.sizeWidth, field.sizeDepth) * 4 + 200;
    this.distance = THREE.MathUtils.clamp(this.distance, this.minDistance, this.maxDistance);
  }

  /** 初始取景：目标置于地形中心，距离按视角覆盖整个地形。 */
  fitTerrain(field: HeightField): void {
    this.target.set(field.sizeWidth / 2, 0, field.sizeDepth / 2);
    this.distance = Math.max(field.sizeWidth, field.sizeDepth) * 1.35 + field.spacing * 8;
    this.pitch = 0.72;
    this.yaw = Math.PI * 0.25;
    this.setBounds(field);
  }

  /** 平移时把注视点约束在地形附近，避免丢失模型。 */
  clampTarget(field: HeightField): void {
    const margin = Math.max(field.sizeWidth, field.sizeDepth) * 0.6;
    this.target.x = THREE.MathUtils.clamp(this.target.x, -margin, field.sizeWidth + margin);
    this.target.z = THREE.MathUtils.clamp(this.target.z, -margin, field.sizeDepth + margin);
    this.target.y = 0;
  }
}
