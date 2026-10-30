#pragma once
#include <QString>
class QWebEngineView;
class QObject;
// 同步受信任的内置网页语言，不向用户浏览的外部网页注入脚本。
namespace InterfaceLanguage {
// 翻译明确标记的程序文案；模板参数中的用户路径和名称保持原样。
QString text(const QString& source);
// 绑定原生控件的文案属性，语言变化时原位更新。
void bind(QObject* object, const char* property, const QString& source);
// 登记内置网页；首次加载和后续重载均使用当前语言。
void registerView(QWebEngineView* view);
// 更新已加载的内置网页，不刷新页面或改写用户数据。
void setLanguage(const QString& language);
}
