/* Qt 与页面之间的唯一调用入口，所有方法统一转换为 Promise。 */
(function installDesktopBridge() {
  "use strict";
  // 后端对象、正在执行的调用数量及连接中的 Promise。
  let backend = null;
  let pendingCount = 0;
  let connectionPromise = null;

  // 将后端错误对象转换为可显示的中文信息。
  function errorMessage(error) {
    if (typeof error === "string" && error.trim()) return error;
    if (error && typeof error.message === "string") return error.message;
    return "操作未能完成，请重试。";
  }

  // 通知页面更新操作中的状态。
  function publishBusyState() {
    document.dispatchEvent(new CustomEvent("desktop-busy", { detail: pendingCount > 0 }));
  }

  // 建立连接；普通浏览器中明确拒绝连接，不提供模拟保存。
  function connect() {
    if (backend) return Promise.resolve(backend);
    if (connectionPromise) return connectionPromise;
    connectionPromise = new Promise(function establishConnection(resolve, reject) {
      if (!window.qt || !window.qt.webChannelTransport || typeof window.QWebChannel !== "function") {
        reject(new Error("请从桌面程序打开此页面。"));
        return;
      }
      // 限时避免连接错误时页面永久停留在加载状态。
      const timer = window.setTimeout(function connectionTimedOut() {
        reject(new Error("连接桌面程序超时，请重新打开程序。"));
      }, 15000);
      try {
        new window.QWebChannel(window.qt.webChannelTransport, function channelConnected(channel) {
          window.clearTimeout(timer);
          if (!channel.objects.backend) {
            reject(new Error("未找到桌面程序的 backend 接口。"));
            return;
          }
          backend = channel.objects.backend;
          resolve(backend);
        });
      } catch (error) {
        window.clearTimeout(timer);
        reject(error);
      }
    }).catch(function resetFailedConnection(error) {
      connectionPromise = null;
      throw error;
    });
    return connectionPromise;
  }

  // 调用 Qt 方法，验证统一返回结构并跟踪全局加载状态。
  async function call(method, ...args) {
    const target = await connect();
    if (typeof target[method] !== "function") throw new Error("桌面程序缺少接口：" + method);
    pendingCount += 1;
    publishBusyState();
    try {
      return await new Promise(function invokeBackend(resolve, reject) {
        // 文件选择框可能长时间等待用户，因此不设置调用超时。
        try {
          target[method](...args, function backendReturned(result) {
            if (!result || typeof result.ok !== "boolean") reject(new Error("桌面程序返回了无法识别的数据。"));
            else if (!result.ok) reject(new Error(errorMessage(result.error)));
            else resolve(result.data);
          });
        } catch (error) {
          reject(error);
        }
      });
    } finally {
      pendingCount = Math.max(0, pendingCount - 1);
      publishBusyState();
    }
  }

  // 订阅 Qt 变更信号，回调的异常由页面调用方处理。
  async function on(signalName, callback) {
    const target = await connect();
    if (target[signalName] && typeof target[signalName].connect === "function") target[signalName].connect(callback);
  }

  // 提供连接、方法调用及信号订阅三个稳定接口。
  window.desktopBridge = Object.freeze({ connect, call, on });
})();
