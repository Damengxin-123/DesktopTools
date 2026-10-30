/* 关卡布局项目列表；矢量编辑在原生独立窗口中完成。 */
(function installLevelLayouts() {
    "use strict";
    const page = document.getElementById("page-level-layout"); // 功能页容器。
    const list = document.getElementById("level-layout-list"); // 已保存项目列表。
    const error = document.getElementById("level-layout-error"); // 操作错误信息。
    let refreshing = false; // 防止重复读取项目目录。
    let signature = ""; // 上次已显示的目录快照。
    // 显示失败原因，始终使用文本节点防止项目名称注入页面。
    function report(reason) { error.textContent = reason.message || String(reason); error.hidden = false; }
    // 获取目录快照，仅在项目变化时重建列表。
    async function refresh() {
        if (refreshing) return;
        refreshing = true;
        try {
            const items = await window.desktopBridge.call("getLevelLayouts");
            document.getElementById("level-layout-count").textContent = String(items.length);
            const next = JSON.stringify(items);
            if (signature === next) return;
            signature = next;
            list.replaceChildren();
            if (!items.length) {
                const empty = document.createElement("p");
                empty.className = "empty-state";
                empty.textContent = "还没有关卡布局。新建项目并保存后，将自动出现在这里。也可以导入已有 .level.json 项目。";
                list.append(empty);
            }
            for (const item of items) {
                const row = document.createElement("article");
                row.className = "level-project-card";
                const title = document.createElement("h2"); title.textContent = item.title;
                const details = document.createElement("p");
                details.className = "muted";
                details.textContent = `${item.objects} 个矢量对象 · ${new Date(item.updated).toLocaleString()}`;
                const open = document.createElement("button");
                open.className = "button secondary"; open.textContent = "打开编辑";
                open.addEventListener("click", () => launch(item.id));
                row.addEventListener("dblclick", event => { if (event.target !== open) launch(item.id); });
                row.append(title, details, open); list.append(row);
            }
        } catch (reason) { report(reason); }
        finally { refreshing = false; }
    }
    // 打开或激活对应编辑窗口。
    async function launch(id) {
        error.hidden = true;
        try { await window.desktopBridge.call("openLevelLayout", id); }
        catch (reason) { report(reason); }
    }
    document.getElementById("new-level-layout").addEventListener("click", () => launch(""));
    document.getElementById("import-level-layout").addEventListener("click", async () => {
        error.hidden = true;
        try { await window.desktopBridge.call("importLevelLayout"); await refresh(); }
        catch (reason) { report(reason); }
    });
    document.getElementById("refresh-level-layout").addEventListener("click", refresh);
    // 同时覆盖独立进程保存的项目；只在功能页可见时轮询。
    new MutationObserver(() => { if (!page.hidden) refresh(); }).observe(page, { attributes: true, attributeFilter: ["hidden"] });
    window.setInterval(() => { if (!page.hidden && !document.hidden) refresh(); }, 4000);
    window.addEventListener("focus", () => { if (!page.hidden) refresh(); });
    window.desktopBridge.connect().then(refresh).catch(report);
})();
