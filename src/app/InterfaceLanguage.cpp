#include "InterfaceLanguage.h"
#include <QList>
#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QVariant>
#include <QWidget>
#include <QPointer>
#ifndef DESKTOPTOOL_NATIVE_ONLY
#include <QWebEnginePage>
#include <QWebEngineView>
#endif
namespace {
QString currentLanguage = QStringLiteral("zh-CN"); // 主设置提供的界面语言。
#ifndef DESKTOPTOOL_NATIVE_ONLY
QList<QPointer<QWebEngineView>> views; // 弱引用避免影响窗口生命周期。
#endif
// 控件属性仅来自显式绑定，不遍历用户输入控件或数据对象。
struct TextBinding {
    QPointer<QObject> object; // 对象销毁后自动失效。
    QByteArray property; // text、toolTip、windowTitle 等界面属性。
    QString source; // 原始程序文案，参数已经按原样插入。
    QString rendered; // 上次界面值，避免覆盖后来写入的用户内容或新状态。
};
QList<TextBinding> textBindings; // 已登记的原生界面文案。
// 本地字典不访问网络，与网页共享同一份审校文案。
const QJsonObject& catalog()
{
    static const QJsonObject value = []() {
        QFile file(QStringLiteral(":/web/i18n-en.json"));
        if (!file.open(QIODevice::ReadOnly)) return QJsonObject{};
        return QJsonDocument::fromJson(file.readAll()).object();
    }();
    return value;
}
// 只匹配程序模板中的编号参数，用户参数不会再进入翻译流程。
QString translateTemplate(const QString& source, const QString& key, const QString& translated)
{
    const QRegularExpression placeholder(QStringLiteral("%[1-9][0-9]*"));
    QString pattern = QStringLiteral("^");
    QStringList parameterSlots;
    qsizetype offset = 0;
    auto matches = placeholder.globalMatch(key);
    while (matches.hasNext()) {
        const auto match = matches.next();
        pattern += QRegularExpression::escape(key.mid(offset, match.capturedStart() - offset));
        pattern += QStringLiteral("([\\s\\S]*?)");
        parameterSlots.append(match.captured());
        offset = match.capturedEnd();
    }
    if (parameterSlots.isEmpty()) return {};
    pattern += QRegularExpression::escape(key.mid(offset)) + QStringLiteral("$");
    const auto match = QRegularExpression(pattern).match(source);
    if (!match.hasMatch()) return {};
    QString result;
    offset = 0;
    matches = placeholder.globalMatch(translated);
    while (matches.hasNext()) {
        const auto token = matches.next();
        result += translated.mid(offset, token.capturedStart() - offset);
        const qsizetype index = parameterSlots.indexOf(token.captured());
        result += index >= 0 ? match.captured(index + 1) : token.captured();
        offset = token.capturedEnd();
    }
    return result + translated.mid(offset);
}
#ifndef DESKTOPTOOL_NATIVE_ONLY
// 仅向随程序打包的三个界面传递固定语言标识。
void applyLanguage(QWebEngineView* view)
{
    if (!view || view->url().scheme() != QStringLiteral("qrc")) return;
    const QString path = view->url().path();
    if (path != QStringLiteral("/web/index.html") && path != QStringLiteral("/web/bookmarks.html")
        && path != QStringLiteral("/web/terrain.html")) return;
    view->page()->runJavaScript(QStringLiteral("window.desktopI18n && window.desktopI18n.setLanguage('%1');")
        .arg(currentLanguage));
}
#endif
}
QString InterfaceLanguage::text(const QString& source)
{
    if (currentLanguage != QStringLiteral("en")) return source;
    const auto& dictionary = catalog();
    if (dictionary.contains(source)) return dictionary.value(source).toString();
    for (auto it = dictionary.begin(); it != dictionary.end(); ++it) {
        if (!it.key().contains(QLatin1Char('%'))) continue;
        const QString translated = translateTemplate(source, it.key(), it.value().toString());
        if (!translated.isEmpty()) return translated;
    }
    return source;
}
void InterfaceLanguage::bind(QObject* object, const char* property, const QString& source)
{
    for (auto it = textBindings.begin(); it != textBindings.end();) {
        if (it->object.isNull()) { it = textBindings.erase(it); continue; }
        if (it->object == object && it->property == property) {
            it->source = source;
            it->rendered = text(source);
            object->setProperty(property, it->rendered);
            return;
        }
        ++it;
    }
    textBindings.append({object, property, source, text(source)});
    object->setProperty(property, text(source));
}
#ifndef DESKTOPTOOL_NATIVE_ONLY
void InterfaceLanguage::registerView(QWebEngineView* view)
{
    views.append(view);
    QObject::connect(view, &QWebEngineView::loadFinished, view, [view](bool loaded) {
        if (loaded) applyLanguage(view);
    });
}
#endif
void InterfaceLanguage::setLanguage(const QString& language)
{
    currentLanguage = language == QStringLiteral("en") ? language : QStringLiteral("zh-CN");
    for (auto it = textBindings.begin(); it != textBindings.end();) {
        if (!it->object || it->object->property(it->property.constData()).toString() != it->rendered) {
            it = textBindings.erase(it);
            continue;
        }
        it->rendered = text(it->source);
        it->object->setProperty(it->property.constData(), it->rendered);
        ++it;
    }
    // 自绘截图和录屏工具条在下一次绘制时读取当前语言。
    for (auto* widget : QApplication::topLevelWidgets()) widget->update();
#ifndef DESKTOPTOOL_NATIVE_ONLY
    for (auto it = views.begin(); it != views.end();) {
        if (it->isNull()) it = views.erase(it);
        else { applyLanguage(it->data()); ++it; }
    }
#endif
}
