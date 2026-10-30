/**
 * 高度数据分块传输：读取与保存共用。
 * Float32Array 通过显式小端编码为字节再分块传输，避免把数百万个数字
 * 以 JSON 文本形式反复搬运，也不依赖平台字节序。
 */
import { bytesToBase64, base64ToBytes } from "./bridge";

/** 流式传输后端返回的分块描述。 */
export interface StreamInfo {
  token: string;
  bytes: number;
  chunkSize: number;
  [key: string]: unknown;
}

/** 把 Float32 高度编码为小端字节。 */
export function encodeFloatLeBytes(heights: Float32Array): Uint8Array {
  const out = new Uint8Array(heights.length * 4);
  const view = new DataView(out.buffer);
  for (let index = 0; index < heights.length; ++index) {
    view.setFloat32(index * 4, heights[index], true);
  }
  return out;
}

/** 把小端字节解码为 Float32。 */
export function decodeFloatLeBytes(bytes: Uint8Array): Float32Array {
  if (bytes.length % 4 !== 0) throw new Error("高度数据长度无效。");
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const out = new Float32Array(bytes.length / 4);
  for (let index = 0; index < out.length; ++index) {
    out[index] = view.getFloat32(index * 4, true);
  }
  return out;
}

/** 从后端按块读取全部高度字节（含文件头），返回去掉文件头的 Float32 数据。 */
export async function readHeightChunks(bridge: { call: <T>(method: string, ...args: unknown[]) => Promise<T> },
                                       info: StreamInfo, headerBytes: number): Promise<Float32Array> {
  const token = info.token;
  const total = info.bytes;
  const chunkSize = info.chunkSize;
  const buffer = new Uint8Array(total);
  try {
    let offset = 0;
    let index = 0;
    while (offset < total) {
      const chunk = await bridge.call<{ data: string; bytes: number }>("readHeightChunk", token, index);
      const bytes = base64ToBytes(chunk.data);
      if (offset + bytes.length > total) throw new Error("高度数据超出预期大小。");
      buffer.set(bytes, offset);
      offset += bytes.length;
      index += 1;
    }
  } finally {
    // 无论成功失败都释放服务端的读取流，避免占用传输配额。
    await bridge.call("cancelStream", token).catch(() => undefined);
  }
  return decodeFloatLeBytes(buffer.subarray(headerBytes));
}

/** 写入流只需要令牌与分块大小。 */
type ChunkedStream = { token: string; chunkSize: number };

/** 把 Float32 高度按块写给后端（不含文件头；文件头由服务端按网格参数生成）。 */
export async function writeHeightChunks(bridge: { call: <T>(method: string, ...args: unknown[]) => Promise<T> },
                                        begin: ChunkedStream, heights: Float32Array): Promise<void> {
  const token = begin.token;
  const chunkSize = begin.chunkSize;
  const payload = encodeFloatLeBytes(heights);
  try {
    const chunks = Math.ceil(payload.length / chunkSize);
    for (let index = 0; index < chunks; ++index) {
      const piece = payload.subarray(index * chunkSize, Math.min(payload.length, (index + 1) * chunkSize));
      await bridge.call("writeHeightChunk", token, index, bytesToBase64(piece));
    }
  } catch (error) {
    await bridge.call("cancelStream", token).catch(() => undefined);
    throw error;
  }
}

/** 把 16 位样本按块写给后端导出流。 */
export async function writeExportChunks(bridge: { call: <T>(method: string, ...args: unknown[]) => Promise<T> },
                                        begin: ChunkedStream, samples: Uint16Array): Promise<void> {
  const token = begin.token;
  const chunkSize = begin.chunkSize;
  // Uint16 → 小端字节视图。
  const payload = new Uint8Array(samples.length * 2);
  const view = new DataView(payload.buffer);
  for (let index = 0; index < samples.length; ++index) {
    view.setUint16(index * 2, samples[index], true);
  }
  try {
    const chunks = Math.ceil(payload.length / chunkSize);
    for (let index = 0; index < chunks; ++index) {
      const piece = payload.subarray(index * chunkSize, Math.min(payload.length, (index + 1) * chunkSize));
      await bridge.call("writeExportChunk", token, index, bytesToBase64(piece));
    }
  } catch (error) {
    await bridge.call("cancelStream", token).catch(() => undefined);
    throw error;
  }
}

/** 服务端 heightmap.bin 文件头长度：魔数 4 + 版本/列/行 3×4。 */
export const HEIGHT_HEADER_BYTES = 16;
