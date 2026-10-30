/**
 * Qt 桥接封装：QWebChannel 的类型化访问入口。
 * 仅暴露后端注册的方法调用；不执行任何字符串拼接脚本。
 */
/** 后端统一返回结构（与 C++ ServiceResult 对应）。 */
export interface BackendResult<T = unknown> {
    ok: boolean;
    data?: T;
    error?: string;
}
/** 声明 QWebChannel 提供的全局对象。 */
interface QtWebChannelGlobal {
    qt?: {
        webChannelTransport?: unknown;
    };
    QWebChannel?: (transport: unknown, callback: (channel: {
        objects: Record<string, unknown>;
    }) => void) => void;
}
interface ChannelBackend {
    [method: string]: ((...args: unknown[]) => void) | unknown;
}
export class QtBridge {
    private backend: ChannelBackend | null = null;
    private connecting: Promise<ChannelBackend> | null = null;
    /** 建立与 Qt 后端的连接；失败时抛出可显示的中文错误。 */
    connect(): Promise<ChannelBackend> {
        if (this.backend)
            return Promise.resolve(this.backend);
        if (this.connecting)
            return this.connecting;
        this.connecting = new Promise<ChannelBackend>((resolve, reject) => {
            const global = window as unknown as QtWebChannelGlobal;
            if (!global.qt || !global.qt.webChannelTransport || typeof global.QWebChannel !== "function") {
                reject(new Error(window.desktopI18n.t("请从桌面程序打开地形编辑窗口。")));
                return;
            }
            const timer = window.setTimeout(() => reject(new Error(window.desktopI18n.t("连接桌面程序超时，请重新打开窗口。"))), 15000);
            global.QWebChannel!(global.qt.webChannelTransport!, (channel) => {
                window.clearTimeout(timer);
                const backend = channel.objects.backend as ChannelBackend | undefined;
                if (!backend) {
                    reject(new Error(window.desktopI18n.t("未找到地形后端接口。")));
                    return;
                }
                this.backend = backend;
                resolve(backend);
            });
        }).catch((error) => {
            this.connecting = null;
            throw error;
        });
        return this.connecting;
    }
    /** 订阅后端信号（如 terrainChanged）；回调异常由调用方处理。 */
    async on(signalName: string, callback: (...args: unknown[]) => void): Promise<void> {
        const backend = await this.connect();
        const signal = backend[signalName];
        if (signal && typeof (signal as {
            connect?: unknown;
        }).connect === "function") {
            (signal as {
                connect: (cb: (...args: unknown[]) => void) => void;
            }).connect(callback);
        }
    }
    /** 调用后端方法并校验统一返回结构。 */
    async call<T = unknown>(method: string, ...args: unknown[]): Promise<T> {
        const backend = await this.connect();
        const target = backend[method];
        if (typeof target !== "function")
            throw new Error(window.desktopI18n.format("桌面程序缺少接口：{0}", method));
        return await new Promise<T>((resolve, reject) => {
            try {
                (target as (...callArgs: unknown[]) => void)(...args, (result: BackendResult<T> | undefined) => {
                    if (!result || typeof result.ok !== "boolean")
                        reject(new Error(window.desktopI18n.t("桌面程序返回了无法识别的数据。")));
                    else if (!result.ok)
                        reject(window.desktopI18n.failure(result.error || "操作失败。"));
                    else
                        resolve(result.data as T);
                });
            }
            catch (error) {
                reject(error instanceof Error ? error : new Error(String(error)));
            }
        });
    }
}
/** 字节 → Base64（分块传输用）。 */
export function bytesToBase64(bytes: Uint8Array): string {
    let binary = "";
    const blockSize = 0x8000;
    for (let offset = 0; offset < bytes.length; offset += blockSize) {
        binary += String.fromCharCode(...bytes.subarray(offset, offset + blockSize));
    }
    return btoa(binary);
}
/** Base64 → 字节。 */
export function base64ToBytes(base64: string): Uint8Array {
    const binary = atob(base64);
    const bytes = new Uint8Array(binary.length);
    for (let index = 0; index < binary.length; ++index)
        bytes[index] = binary.charCodeAt(index);
    return bytes;
}
