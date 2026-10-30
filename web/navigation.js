/* 主侧栏的折叠、功能顺序和可见性；设置草稿与已保存配置分开管理。 */
(function installNavigation() {
    "use strict";
    // 固定功能目录，设置入口始终保留，避免隐藏后无法恢复。
    const buttons = Array.from(document.querySelectorAll(".main-nav .nav-button"));
    const features = buttons.filter(button => button.dataset.page !== "settings").map(button => ({
        id: button.dataset.page, // 稳定功能标识。
        get name() { return button.querySelector(".nav-text")?.textContent.trim() || button.textContent.replace(/\d+$/, "").trim(); }, // 显示名称。
        button // 对应的原有导航按钮，保留事件和计数节点。
    }));
    let draft = []; // 设置页正在编辑的顺序及可见性。
    let collapsed = false; // 当前侧栏是否收起。
    let saving = false; // 防止连续点击产生乱序写入。
    const toggle = document.getElementById("sidebar-toggle");
    const list = document.getElementById("feature-settings-list");
    // 兼容旧配置与后续新增功能，缺失项按默认顺序追加。
    function normalize(items) {
        const result = [];
        for (const item of Array.isArray(items) ? items : []) {
            if (features.some(feature => feature.id === item.id) && !result.some(entry => entry.id === item.id))
                result.push({ id: item.id, visible: item.visible !== false });
        }
        for (const feature of features)
            if (!result.some(item => item.id === feature.id))
                result.push({ id: feature.id, visible: true });
        return result;
    }
    // 统一更新折叠按钮语义与侧栏样式。
    function setCollapsed(value) {
        collapsed = value;
        document.body.classList.toggle("sidebar-collapsed", value);
        window.desktopI18n.attr(toggle, "title", window.desktopI18n.live(() => value ? window.desktopI18n.t("展开工具栏") : window.desktopI18n.t("收起工具栏")));
        window.desktopI18n.attr(toggle, "aria-label", window.desktopI18n.live(() => toggle.title));
        toggle.setAttribute("aria-expanded", String(!value));
        document.dispatchEvent(new Event("desktop-navigation-layout"));
    }
    // 通知主设置表单存在未保存修改。
    function changed() { document.dispatchEvent(new Event("desktop-navigation-dirty")); }
    // 绘制可键盘操作的排序和显示开关。
    function render() {
        list.replaceChildren();
        draft.forEach((item, index) => {
            const feature = features.find(entry => entry.id === item.id);
            const row = document.createElement("div");
            row.className = "feature-setting-row";
            row.dataset.feature = item.id;
            const label = document.createElement("label");
            const input = document.createElement("input");
            input.type = "checkbox";
            input.checked = item.visible;
            window.desktopI18n.attr(input, "aria-label", window.desktopI18n.live(() => window.desktopI18n.t("显示") + feature.name));
            input.addEventListener("change", () => { item.visible = input.checked; changed(); });
            label.append(input, window.desktopIcons.create(item.id), window.desktopI18n.textNode(window.desktopI18n.live(() => feature.name)));
            row.append(label);
            for (const [offset, name, icon] of [[-1, window.desktopI18n.t("上移"), "up"], [1, window.desktopI18n.t("下移"), "down"]]) {
                const button = document.createElement("button");
                button.type = "button";
                button.className = "icon-button";
                window.desktopI18n.attr(button, "title", window.desktopI18n.live(() => name + feature.name));
                window.desktopI18n.attr(button, "aria-label", window.desktopI18n.live(() => button.title));
                button.disabled = index + offset < 0 || index + offset >= draft.length;
                button.append(window.desktopIcons.create(icon));
                button.addEventListener("click", () => {
                    [draft[index], draft[index + offset]] = [draft[index + offset], draft[index]];
                    render();
                    changed();
                    list.children[index + offset].querySelectorAll("button")[offset < 0 ? 0 : 1].focus();
                });
                row.append(button);
            }
            list.append(row);
        });
    }
    // 应用后端设置并重新创建设置草稿；设置入口永远置于功能列表底部。
    function apply(settings) {
        draft = normalize(settings.navigationItems);
        const nav = document.querySelector(".main-nav");
        for (const item of draft) {
            const button = features.find(feature => feature.id === item.id).button;
            button.hidden = !item.visible;
            nav.append(button);
        }
        nav.append(buttons.find(button => button.dataset.page === "settings"));
        if (!saving)
            setCollapsed(settings.sidebarCollapsed === true);
        render();
        document.dispatchEvent(new Event("desktop-navigation-applied"));
    }
    // 折叠是即时操作，单独持久化，不提交设置页的其他未保存字段。
    toggle.addEventListener("click", async () => {
        if (saving)
            return;
        saving = true;
        toggle.disabled = true;
        const previous = collapsed;
        setCollapsed(!collapsed);
        try {
            await window.desktopBridge.call("saveSettings", { sidebarCollapsed: collapsed });
            document.dispatchEvent(new CustomEvent("desktop-sidebar-saved", { detail: collapsed }));
        }
        catch (error) {
            setCollapsed(previous);
            document.dispatchEvent(new CustomEvent("desktop-navigation-error", { detail: error }));
        }
        finally {
            saving = false;
            toggle.disabled = false;
        }
    });
    window.desktopNavigation = Object.freeze({ apply, values: () => ({ navigationItems: draft.map(item => ({ ...item })), sidebarCollapsed: collapsed }) });
})();
