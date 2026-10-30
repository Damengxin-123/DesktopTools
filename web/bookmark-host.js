/* 将主页面书签区域映射到原生子控件，保留主导航与浏览器安全隔离。 */
(function installBookmarkHost() {
    "use strict";
    const host = document.getElementById("page-bookmarks"); // 原生浏览器在页面中的占位区域。
    let scheduled = false; // 合并同一帧内的布局变化。
    // 按可视区域同步原生子控件的位置和可见性。
    function sync() {
        scheduled = false;
        const visible = !host.hidden && !document.getElementById("workspace").inert;
        const rect = host.getBoundingClientRect();
        window.desktopBridge.call("setBookmarkViewport", visible, Math.round(rect.left), Math.round(rect.top), Math.round(rect.width), Math.round(rect.height))
            .catch(error => { window.desktopI18n.text(document.getElementById("bookmark-host-error"), window.desktopI18n.live(() => error.message)); });
    }
    // 下一帧读取完成布局后的边界，避免高频桥接调用。
    function schedule() { if (!scheduled) {
        scheduled = true;
        requestAnimationFrame(sync);
    } }
    new ResizeObserver(schedule).observe(host);
    new MutationObserver(schedule).observe(host, { attributes: true, attributeFilter: ["hidden"] });
    window.addEventListener("resize", schedule);
    document.addEventListener("desktop-navigation-layout", schedule);
})();
