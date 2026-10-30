/** 内置页面的语言接口；只对程序文案调用，用户数据始终作为原样参数。 */
interface DesktopInterfaceLanguage {
    /** 翻译明确的程序文案。 */
    t(source: string): string;
    /** 将后端错误保留为可切换语言的异常。 */
    failure(source: string): Error;
    /** 按编号插入原样参数。 */
    format(source: string, ...args: unknown[]): string;
    /** 延迟文案以便原位切换；具有字符串转换能力，供既有 UI 辅助函数使用。 */
    live(render: () => unknown): string;
    /** 绑定文本，不翻译普通字符串。 */
    text(node: Node | null, message: unknown): void;
    /** 绑定提示和可访问属性。 */
    attr(node: Element | null, name: string, message: unknown): void;
    /** 创建绑定文本节点。 */
    textNode(message: unknown): Text;
}
/** 主窗口和独立地形窗口共同加载的界面语言运行时。 */
interface Window { desktopI18n: DesktopInterfaceLanguage; }
