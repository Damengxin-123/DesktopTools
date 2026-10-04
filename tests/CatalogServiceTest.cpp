#include "services/SettingsService.h"
#include "services/ShortcutService.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace
{
// 在发布构建中也执行断言并输出失败行号。
bool check(bool condition, const char* expression, int line)
{
    if (!condition)
        qCritical().noquote() << "FAIL" << line << expression;
    return condition;
}

// 测试断言失败时立即退出当前测试用例。
#define REQUIRE(expression) do { if (!check((expression), #expression, __LINE__)) return false; } while (false)

// 将测试夹具写入临时目录，不接触实际用户数据。
bool writeFile(const QString& path, const QByteArray& content)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}

// 读取持久化文件，以检查失败操作是否更改原始内容。
QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

// 提取统一返回结果中的成功标志。
bool ok(const QVariantMap& result)
{
    return result.value(QStringLiteral("ok")).toBool();
}

// 提取统一返回结果中的业务数据对象。
QVariantMap resultData(const QVariantMap& result)
{
    return result.value(QStringLiteral("data")).toMap();
}

// 为测试构建一个完整快捷方式对象。
QVariantMap shortcut(const QString& title, const QString& target, const QString& category = QStringLiteral("default"), int type = 2)
{
    return {{QStringLiteral("title"), title}, {QStringLiteral("target"), target},
            {QStringLiteral("categoryId"), category}, {QStringLiteral("type"), type}};
}

// 验证旧配置导入、歧义行警告、原文件保留及稳定标识。
bool legacyImport()
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("shortcut/shortcuts_config.txt"));
    const QByteArray original = QStringLiteral("工作目录,C:/Work,0\nQt文档,https://doc.qt.io,1,开发\n"
                                               "含,逗号,C:/broken.txt,2,开发\n无效条目\n"
                                               "重复标题,C:/Work,0\n").toUtf8();
    REQUIRE(writeFile(path, original));
    ShortcutService service(directory.path());
    REQUIRE(ok(service.snapshot()));
    const QVariantMap state = resultData(service.snapshot());
    const QVariantList items = state.value(QStringLiteral("items")).toList();
    REQUIRE(items.size() == 2);
    REQUIRE(state.value(QStringLiteral("categories")).toList().size() == 2);
    REQUIRE(state.value(QStringLiteral("warnings")).toStringList().size() == 3);
    REQUIRE(readFile(path) == original);
    REQUIRE(QFileInfo::exists(directory.filePath(QStringLiteral("shortcut/shortcuts.v2.json"))));
    ShortcutService reopened(directory.path());
    REQUIRE(ok(reopened.snapshot()));
    REQUIRE(resultData(reopened.snapshot()) == state);
    REQUIRE(!items.first().toMap().value(QStringLiteral("id")).toString().isEmpty());
    return true;
}

// 验证分类和条目增删改、逗号数据、排序、重复校验及信号时机。
bool catalogCrud()
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    ShortcutService service(directory.path());
    int changes = 0;
    QObject::connect(&service, &ShortcutService::changed, [&changes]() { ++changes; });
    const QVariantMap created = service.saveCategory({}, QStringLiteral("开发,工具"));
    REQUIRE(ok(created));
    const QString category = resultData(created).value(QStringLiteral("id")).toString();
    REQUIRE(!category.isEmpty());
    REQUIRE(changes == 1);
    {
        ShortcutService reopened(directory.path());
        REQUIRE(resultData(reopened.snapshot()).value(QStringLiteral("categories")).toList().size() == 2);
        REQUIRE(resultData(reopened.snapshot()).value(QStringLiteral("items")).toList().isEmpty());
    }
    const QVariantMap first = service.saveShortcut(shortcut(QStringLiteral("文档,一"), QStringLiteral("C:/文档,资料/a.txt"), category));
    const QVariantMap second = service.saveShortcut(shortcut(QStringLiteral("文档二"), QStringLiteral("C:/b.txt"), category));
    REQUIRE(ok(first) && ok(second));
    const QString firstId = resultData(first).value(QStringLiteral("id")).toString();
    const QString secondId = resultData(second).value(QStringLiteral("id")).toString();
    const QVariantMap beforeFailure = resultData(service.snapshot());
    const int beforeSignals = changes;
    REQUIRE(!ok(service.saveShortcut(shortcut(QStringLiteral("文档二"), QStringLiteral("C:/different.txt"), category))));
    REQUIRE(!ok(service.saveShortcut(shortcut(QStringLiteral("其他标题"), QStringLiteral("C:/b.txt"), category))));
    REQUIRE(!ok(service.saveShortcut(shortcut(QStringLiteral("类型错误"), QStringLiteral("C:/type.txt"), category, 3))));
    QVariantMap fractional = shortcut(QStringLiteral("小数类型"), QStringLiteral("C:/fraction.txt"), category);
    fractional.insert(QStringLiteral("type"), 1.5);
    REQUIRE(!ok(service.saveShortcut(fractional)));
    REQUIRE(!ok(service.saveShortcut(shortcut(QStringLiteral("未知分类"), QStringLiteral("C:/unknown.txt"), QStringLiteral("missing")))));
    REQUIRE(!ok(service.removeShortcuts({firstId, QStringLiteral("missing")})));
    REQUIRE(!ok(service.removeCategory(QStringLiteral("default"))));
    REQUIRE(!ok(service.saveCategory(QStringLiteral("default"), QStringLiteral("改名"))));
    REQUIRE(changes == beforeSignals);
    REQUIRE(resultData(service.snapshot()) == beforeFailure);
    REQUIRE(ok(service.moveShortcut(secondId, category, firstId)));
    REQUIRE(resultData(service.snapshot()).value(QStringLiteral("items")).toList().first().toMap().value(QStringLiteral("id")) == secondId);
    REQUIRE(!ok(service.moveShortcut(firstId, QStringLiteral("default"), secondId)));
    REQUIRE(ok(service.moveShortcut(secondId, QStringLiteral("default"), {})));
    REQUIRE(ok(service.saveCategory(category, QStringLiteral("新的,分类"))));
    QVariantMap updated = resultData(first);
    updated.insert(QStringLiteral("title"), QStringLiteral("新的,标题"));
    REQUIRE(ok(service.saveShortcut(updated)));
    REQUIRE(ok(service.removeCategory(category)));
    const QVariantList movedItems = resultData(service.snapshot()).value(QStringLiteral("items")).toList();
    REQUIRE(movedItems.size() == 2);
    for (const QVariant& item : movedItems)
        REQUIRE(item.toMap().value(QStringLiteral("categoryId")).toString() == QStringLiteral("default"));
    ShortcutService reopened(directory.path());
    REQUIRE(resultData(reopened.snapshot()) == resultData(service.snapshot()));
    REQUIRE(ok(service.removeShortcuts({firstId, secondId})));
    REQUIRE(resultData(service.snapshot()).value(QStringLiteral("items")).toList().isEmpty());
    return true;
}

// 验证删除分类发生重复冲突时，分类及条目完整保留。
bool categoryConflict()
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    ShortcutService service(directory.path());
    const QString category = resultData(service.saveCategory({}, QStringLiteral("其他"))).value(QStringLiteral("id")).toString();
    REQUIRE(!category.isEmpty());
    REQUIRE(ok(service.saveShortcut(shortcut(QStringLiteral("同名"), QStringLiteral("C:/one.txt")))));
    REQUIRE(ok(service.saveShortcut(shortcut(QStringLiteral("同名"), QStringLiteral("C:/two.txt"), category))));
    const QVariantMap before = service.snapshot();
    REQUIRE(!ok(service.removeCategory(category)));
    REQUIRE(service.snapshot() == before);
    return true;
}

// 验证损坏 JSON、错误版本和悬空分类引用均不会被空配置覆盖。
bool damagedCatalog()
{
    const QList<QByteArray> examples{
        QByteArray("{broken"), QByteArray("{\"version\":99,\"categories\":[],\"items\":[]}"),
        QByteArray("{\"version\":2,\"categories\":[{\"id\":\"default\",\"name\":\"wrong\"}],\"items\":[]}"),
        QJsonDocument(QJsonObject{
            {QStringLiteral("version"), 2},
            {QStringLiteral("categories"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("default")}, {QStringLiteral("name"), QStringLiteral("默认分类")}}}},
            {QStringLiteral("items"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("item")}, {QStringLiteral("categoryId"), QStringLiteral("missing")},
                {QStringLiteral("title"), QStringLiteral("标题")}, {QStringLiteral("target"), QStringLiteral("C:/a.txt")}, {QStringLiteral("type"), 2}}}}
        }).toJson()
    };
    for (const QByteArray& original : examples)
    {
        QTemporaryDir directory;
        REQUIRE(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("shortcut/shortcuts.v2.json"));
        REQUIRE(writeFile(path, original));
        REQUIRE(writeFile(directory.filePath(QStringLiteral("shortcut/shortcuts_config.txt")), QByteArray("old,C:/old,0")));
        ShortcutService service(directory.path());
        REQUIRE(!ok(service.snapshot()));
        REQUIRE(!ok(service.saveCategory({}, QStringLiteral("覆盖尝试"))));
        REQUIRE(!ok(service.saveShortcut(shortcut(QStringLiteral("覆盖尝试"), QStringLiteral("C:/overwrite.txt")))));
        REQUIRE(!ok(service.removeShortcuts({})));
        REQUIRE(readFile(path) == original);
    }
    return true;
}

// 验证保存失败时内存状态和变更信号均保持不变。
bool writeFailure()
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    REQUIRE(writeFile(directory.filePath(QStringLiteral("shortcut")), QByteArray("block directory")));
    ShortcutService service(directory.path());
    const QVariantMap before = service.snapshot();
    int changes = 0;
    QObject::connect(&service, &ShortcutService::changed, [&changes]() { ++changes; });
    REQUIRE(!ok(service.saveCategory({}, QStringLiteral("不能写入"))));
    REQUIRE(service.snapshot() == before);
    REQUIRE(changes == 0);
    return true;
}

// 验证旧设置兼容、未知字段保留、数值范围及损坏文件保护。
bool settingsCompatibility()
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("setting/system_config.json"));
    REQUIRE(writeFile(path, QByteArray("{\"tree_view_font_size\":20,\"hotkey_modifier\":3,\"hotkey_key\":75,\"custom\":{\"keep\":true}}")));
    SettingsService service(directory.path());
    REQUIRE(ok(service.snapshot()));
    REQUIRE(resultData(service.snapshot()).value(QStringLiteral("fontSize")).toInt() == 20);
    REQUIRE(resultData(service.snapshot()).value(QStringLiteral("hotkeyModifier")).toInt() == 3);
    REQUIRE(resultData(service.snapshot()).value(QStringLiteral("hotkeyKey")).toInt() == 75);
    REQUIRE(QDir::isAbsolutePath(resultData(service.snapshot()).value(QStringLiteral("downloadDirectory")).toString()));
    int changes = 0;
    QObject::connect(&service, &SettingsService::changed, [&changes]() { ++changes; });
    REQUIRE(ok(service.save({{QStringLiteral("fontSize"), 24}, {QStringLiteral("downloadDirectory"), directory.path()}})));
    REQUIRE(changes == 1);
    const QByteArray beforeFailure = readFile(path);
    REQUIRE(QJsonDocument::fromJson(beforeFailure).object().value(QStringLiteral("custom")).toObject().value(QStringLiteral("keep")).toBool());
    REQUIRE(!ok(service.save({{QStringLiteral("fontSize"), 7}})));
    REQUIRE(!ok(service.save({{QStringLiteral("fontSize"), 12.5}})));
    REQUIRE(!ok(service.save({{QStringLiteral("fontSize"), QStringLiteral("20")}})));
    REQUIRE(!ok(service.save({{QStringLiteral("hotkeyModifier"), 16}})));
    REQUIRE(!ok(service.save({{QStringLiteral("hotkeyKey"), 255}})));
    REQUIRE(!ok(service.save({{QStringLiteral("hotkeyKey"), 0}})));
    REQUIRE(!ok(service.save({{QStringLiteral("hotkeyKey"), 0xA2}})));
    REQUIRE(!ok(service.save({{QStringLiteral("screenshotHotkeyModifier"), 16}})));
    REQUIRE(!ok(service.save({{QStringLiteral("screenshotHotkeyKey"), 0}})));
    REQUIRE(!ok(service.save({{QStringLiteral("screenshotHotkeyKey"), 0x11}})));
    REQUIRE(!ok(service.save({{QStringLiteral("unknown"), 1}})));
    REQUIRE(readFile(path) == beforeFailure);
    REQUIRE(changes == 1);
    REQUIRE(resultData(service.snapshot()).value(QStringLiteral("screenshotHotkeyModifier")).toInt() == 3);
    REQUIRE(resultData(service.snapshot()).value(QStringLiteral("screenshotHotkeyKey")).toInt() == 0x41);
    REQUIRE(ok(service.save({{QStringLiteral("screenshotHotkeyModifier"), 5},
        {QStringLiteral("screenshotHotkeyKey"), 0x53}})));
    REQUIRE(changes == 2);
    REQUIRE(resultData(service.snapshot()).value(QStringLiteral("screenshotHotkeyModifier")).toInt() == 5);
    REQUIRE(QJsonDocument::fromJson(readFile(path)).object().value(QStringLiteral("screenshot_hotkey_key")).toInt() == 0x53);
    SettingsService reopened(directory.path());
    REQUIRE(reopened.snapshot() == service.snapshot());
    REQUIRE(!ok(SettingsService::validate({{QStringLiteral("fontSize"), 16}})));
    REQUIRE(writeFile(path, QByteArray("{broken")));
    SettingsService broken(directory.path());
    REQUIRE(!ok(broken.snapshot()));
    REQUIRE(!ok(broken.save({{QStringLiteral("fontSize"), 16}, {QStringLiteral("hotkeyModifier"), 0}, {QStringLiteral("hotkeyKey"), 0x77}})));
    REQUIRE(readFile(path) == QByteArray("{broken"));
    REQUIRE(writeFile(path, QByteArray("{\"hotkey_key\":16}")));
    SettingsService badValues(directory.path());
    REQUIRE(!ok(badValues.snapshot()));
    REQUIRE(!ok(badValues.save({{QStringLiteral("hotkeyKey"), 0x77}})));
    REQUIRE(readFile(path) == QByteArray("{\"hotkey_key\":16}"));
    return true;
}

// 使用隔离存储验证自启增删、路径引用、系统状态回读和保存失败保护。
bool autoStartSettings()
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString root = directory.filePath(QStringLiteral("中文 数据"));
    REQUIRE(QDir().mkpath(root));
    QSettings startup(directory.filePath(QStringLiteral("startup.ini")), QSettings::IniFormat);
    startup.setValue(QStringLiteral("OtherApp"), QStringLiteral("keep"));
    SettingsService service(root, nullptr, &startup);
    REQUIRE(!resultData(service.snapshot()).value(QStringLiteral("autoStart")).toBool());
    REQUIRE(ok(service.save({{QStringLiteral("autoStart"), true}, {QStringLiteral("downloadDirectory"), root}})));
    const QString expected = QStringLiteral("\"%1\" --data-dir \"%2\"")
        .arg(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()), root);
    startup.sync();
    REQUIRE(startup.value(QStringLiteral("DesktopTool")).toString() == expected);
    REQUIRE(resultData(service.snapshot()).value(QStringLiteral("autoStart")).toBool());
    SettingsService reopened(root, nullptr, &startup);
    REQUIRE(resultData(reopened.snapshot()).value(QStringLiteral("autoStart")).toBool());
    const QString config = QDir(root).filePath(QStringLiteral("setting/system_config.json"));
    const QByteArray original = readFile(config);
    REQUIRE(QJsonDocument::fromJson(original).object().value(QStringLiteral("auto_start")).toBool());
    REQUIRE(!ok(service.save({{QStringLiteral("autoStart"), QStringLiteral("false")}})));
    REQUIRE(!ok(service.save({{QStringLiteral("autoStart"), 1}})));
    REQUIRE(readFile(config) == original);
    REQUIRE(startup.value(QStringLiteral("DesktopTool")).toString() == expected);

    // 设置文件不可写时不能提前移除已经存在的自启项。
    REQUIRE(QFile::rename(config, config + QStringLiteral(".saved")));
    REQUIRE(QDir().mkdir(config));
    REQUIRE(!ok(service.save({{QStringLiteral("autoStart"), false}})));
    startup.sync();
    REQUIRE(startup.value(QStringLiteral("DesktopTool")).toString() == expected);
    REQUIRE(QDir().rmdir(config));
    REQUIRE(QFile::rename(config + QStringLiteral(".saved"), config));
    REQUIRE(ok(service.save({{QStringLiteral("autoStart"), false}})));
    startup.sync();
    REQUIRE(!startup.contains(QStringLiteral("DesktopTool")));
    REQUIRE(startup.value(QStringLiteral("OtherApp")).toString() == QStringLiteral("keep"));

    // 外部移除系统启动项后，快照必须覆盖配置文件中的旧勾选状态。
    REQUIRE(ok(service.save({{QStringLiteral("autoStart"), true}})));
    startup.remove(QStringLiteral("DesktopTool"));
    startup.sync();
    REQUIRE(!resultData(service.snapshot()).value(QStringLiteral("autoStart")).toBool());

    // 模拟自启存储不可写，配置文件和成功信号不能发生变化。
    const QString blockedPath = directory.filePath(QStringLiteral("blocked.ini"));
    REQUIRE(QDir().mkdir(blockedPath));
    QSettings blocked(blockedPath, QSettings::IniFormat);
    SettingsService failing(root, nullptr, &blocked);
    const QByteArray beforeFailure = readFile(config);
    int changes = 0;
    QObject::connect(&failing, &SettingsService::changed, [&changes]() { ++changes; });
    REQUIRE(!ok(failing.save({{QStringLiteral("autoStart"), true}})));
    REQUIRE(readFile(config) == beforeFailure);
    REQUIRE(changes == 0);
    return true;
}

// 验证下载目录默认值、部分更新、写权限探测及离线目录的设置恢复。
bool downloadDirectorySettings()
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString config = directory.filePath(QStringLiteral("setting/system_config.json"));
    const QString downloadPath = directory.filePath(QStringLiteral("下载 文件"));
    REQUIRE(QDir().mkdir(downloadPath));
    SettingsService service(directory.path());
    QString expectedDefault = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (expectedDefault.isEmpty())
        expectedDefault = QDir::home().filePath(QStringLiteral("Downloads"));
    expectedDefault = QDir::cleanPath(QDir::fromNativeSeparators(expectedDefault));
    REQUIRE(resultData(service.snapshot()).value(QStringLiteral("downloadDirectory")).toString() == expectedDefault);

    int changes = 0;
    QObject::connect(&service, &SettingsService::changed, [&changes]() { ++changes; });
    REQUIRE(ok(service.save({{QStringLiteral("downloadDirectory"), QDir::toNativeSeparators(downloadPath)}})));
    REQUIRE(changes == 1);
    REQUIRE(resultData(service.snapshot()).value(QStringLiteral("downloadDirectory")).toString() == downloadPath);
    REQUIRE(QJsonDocument::fromJson(readFile(config)).object().value(QStringLiteral("download_directory")).toString() == downloadPath);
    REQUIRE(QDir(downloadPath).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    REQUIRE(ok(service.save({{QStringLiteral("fontSize"), 22}})));
    REQUIRE(resultData(service.snapshot()).value(QStringLiteral("downloadDirectory")).toString() == downloadPath);
    REQUIRE(changes == 2);

    const QByteArray original = readFile(config);
    const QVariantMap originalState = service.snapshot();
    const QString missing = directory.filePath(QStringLiteral("误写的目录"));
    REQUIRE(!ok(service.save({{QStringLiteral("downloadDirectory"), QStringLiteral("relative/downloads")}})));
    REQUIRE(!ok(service.save({{QStringLiteral("downloadDirectory"), missing}})));
    REQUIRE(!QFileInfo::exists(missing));
    REQUIRE(!ok(service.save({{QStringLiteral("downloadDirectory"), config}})));
    REQUIRE(!ok(service.save({{QStringLiteral("downloadDirectory"), 123}})));
    REQUIRE(!ok(service.save({{QStringLiteral("downloadDirectory"), downloadPath + QChar::Null}})));
    REQUIRE(service.snapshot() == originalState);
    REQUIRE(readFile(config) == original);
    REQUIRE(changes == 2);

    // 模拟已保存目录被移除，仍可进入设置并选择新的有效目录。
    REQUIRE(QDir().rmdir(downloadPath));
    SettingsService reopened(directory.path());
    REQUIRE(ok(reopened.snapshot()));
    REQUIRE(resultData(reopened.snapshot()).value(QStringLiteral("downloadDirectory")).toString() == downloadPath);
    REQUIRE(ok(reopened.save({{QStringLiteral("downloadDirectory"), directory.path()}})));
    REQUIRE(resultData(reopened.snapshot()).value(QStringLiteral("downloadDirectory")).toString() == directory.path());

    // 配置字段类型损坏时保护原文件，而不是把数字静默当成路径。
    REQUIRE(writeFile(config, QByteArray("{\"download_directory\":123}")));
    SettingsService invalid(directory.path());
    REQUIRE(!ok(invalid.snapshot()));
    REQUIRE(!ok(invalid.save({{QStringLiteral("downloadDirectory"), directory.path()}})));
    REQUIRE(readFile(config) == QByteArray("{\"download_directory\":123}"));
    return true;
}
}

// 运行不依赖界面与 QtTest 的服务回归测试。
int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    const bool passed = legacyImport() && catalogCrud() && categoryConflict()
        && damagedCatalog() && writeFailure() && settingsCompatibility() && downloadDirectorySettings()
        && autoStartSettings();
    if (passed)
        qInfo() << "CatalogServiceTest: all 8 scenarios passed";
    return passed ? 0 : 1;
}
