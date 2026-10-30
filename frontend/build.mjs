/*
 * 地形前端构建脚本：分别以 IIFE 单文件形式打包两个入口，
 * 产物输出到 web/dist，由 Qt 资源清单（app.qrc）打包进程序，运行时不依赖 Node.js 与开发服务器。
 *   dist/terrain-editor.js  → 独立编辑窗口页面（含 Three.js）
 *   dist/terrain-page.js    → 主窗口地形页（项目列表 + 三维预览，含 Three.js）
 */
import { build } from "vite";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";

const root = dirname(fileURLToPath(import.meta.url));

/** @type {import("vite").UserConfig} */
const shared = {
  root,
  configFile: false,
  logLevel: "info",
  build: {
    target: "chrome118",
    minify: "esbuild",
    sourcemap: false,
    outDir: resolve(root, "../web/dist"),
    emptyOutDir: true,
    assetsInlineLimit: 0,
    rollupOptions: { output: { inlineDynamicImports: true } }
  }
};

// 两个入口分别构建：IIFE 输出要求单一入口块，避免 qrc 上的 ES module 加载差异。
await build({ ...shared, build: { ...shared.build, rollupOptions: { ...shared.build.rollupOptions, input: resolve(root, "src/entries/editor-entry.ts"), output: { ...shared.build.rollupOptions.output, entryFileNames: "terrain-editor.js", format: "iife" } } } });
await build({ ...shared, build: { ...shared.build, emptyOutDir: false, rollupOptions: { ...shared.build.rollupOptions, input: resolve(root, "src/entries/page-entry.ts"), output: { ...shared.build.rollupOptions.output, entryFileNames: "terrain-page.js", format: "iife" } } } });
console.log("地形前端构建完成 → web/dist");
