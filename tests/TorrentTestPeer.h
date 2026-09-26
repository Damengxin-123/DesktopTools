#pragma once

#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QStringList>
#include <QThread>
#include <QVariantList>
#include <QVariantMap>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/load_torrent.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

/** 仅在回环地址做种的测试辅助对象，提供两份确定内容的文件与真实磁力链接。 */
class TorrentTestPeer final
{
public:
    /** 在临时目录生成文件并本地做种；contentSeed 改变确定内容，便于并发验证不同 info-hash。 */
    explicit TorrentTestPeer(const QString& sourceDirectory, int contentSeed = 0)
    {
        namespace lt = libtorrent;
        try {
            const QString source = QDir(sourceDirectory).absolutePath();
            if (!QDir().mkpath(QDir(source).filePath(QStringLiteral("bundle")))) {
                m_error = QStringLiteral("无法创建本地做种测试目录。");
                return;
            }
            const QStringList names{QStringLiteral("bundle/first.bin"), QStringLiteral("bundle/second.bin")};
            std::vector<lt::create_file_entry> entries; // 明确列出两个文件，避免递归加入其他测试产物。
            for (int index = 0; index < names.size(); ++index) {
                QByteArray contents(FileSize, '\0');
                for (qsizetype offset = 0; offset < contents.size(); ++offset)
                    contents[offset] = static_cast<char>((offset * 31 + (index + 1) * 47 + contentSeed) & 0xff);
                QFile file(QDir(source).filePath(names[index]));
                if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
                    || file.write(contents) != contents.size() || !file.flush()) {
                    m_error = QStringLiteral("无法写入本地做种文件：") + file.errorString();
                    return;
                }
                file.close();
                entries.emplace_back(names[index].toUtf8().toStdString(), FileSize);
            }

            // 文件大小均为分片大小的整数倍，验证未选文件时不受边界共享分片影响。
            lt::create_torrent torrent(std::move(entries), 16 * 1024, lt::create_torrent::v1_only);
            lt::error_code error;
            lt::set_piece_hashes(torrent, source.toUtf8().toStdString(), error);
            if (error) {
                m_error = QStringLiteral("无法计算做种文件哈希：") + QString::fromStdString(error.message());
                return;
            }
            const std::vector<char> encoded = torrent.generate_buf();
            auto parameters = lt::load_torrent_buffer(lt::span<char const>(encoded.data(), encoded.size()), error, lt::load_torrent_limits{});
            if (error || !parameters.ti) {
                m_error = QStringLiteral("无法读取测试种子元数据：") + QString::fromStdString(error.message());
                return;
            }
            const auto& layout = parameters.ti->layout();
            for (const lt::file_index_t index : layout.file_range()) {
                m_files.append(QVariantMap{
                    {QStringLiteral("index"), static_cast<int>(index)}, // libtorrent 原始文件索引。
                    {QStringLiteral("path"), QDir::fromNativeSeparators(QString::fromStdString(layout.file_path(index)))}, // 与网页一致、相对 sourceDirectory 的正斜线路径。
                    {QStringLiteral("size"), QVariant::fromValue<qint64>(layout.file_size(index))} // 原始文件字节数。
                });
            }
            parameters.save_path = source.toUtf8().toStdString();
            parameters.flags |= lt::torrent_flags::seed_mode | lt::torrent_flags::disable_pex;
            parameters.flags &= ~(lt::torrent_flags::paused | lt::torrent_flags::auto_managed);
            const QString magnet = QString::fromStdString(lt::make_magnet_uri(parameters));

            lt::settings_pack settings;
            settings.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:0");
            settings.set_str(lt::settings_pack::outgoing_interfaces, "127.0.0.1");
            settings.set_bool(lt::settings_pack::enable_dht, false);
            settings.set_bool(lt::settings_pack::enable_lsd, false);
            settings.set_bool(lt::settings_pack::enable_upnp, false);
            settings.set_bool(lt::settings_pack::enable_natpmp, false);
            settings.set_bool(lt::settings_pack::enable_outgoing_utp, false);
            settings.set_bool(lt::settings_pack::enable_incoming_utp, false);
            settings.set_bool(lt::settings_pack::allow_multiple_connections_per_ip, true);
            settings.set_int(lt::settings_pack::stop_tracker_timeout, 1);
            m_session = std::make_unique<lt::session>(lt::session_params(std::move(settings)));
            m_handle = m_session->add_torrent(std::move(parameters), error);
            if (error || !m_handle.is_valid()) {
                m_error = QStringLiteral("无法启动本地做种任务：") + QString::fromStdString(error.message());
                m_session.reset();
                return;
            }
            QElapsedTimer deadline;
            deadline.start();
            unsigned short port = 0; // 操作系统实际分配的回环 TCP 端口。
            while (deadline.elapsed() < 5000) {
                port = m_session->listen_port();
                if (port != 0) break;
                QThread::msleep(10);
            }
            if (port == 0 || magnet.isEmpty()) {
                m_error = QStringLiteral("本地做种端口未就绪，无法生成测试磁力链接。");
                m_session.reset();
                return;
            }
            m_magnet = magnet + QStringLiteral("&x.pe=127.0.0.1:%1").arg(port);
        } catch (const std::exception& error) {
            m_error = QStringLiteral("创建本地做种测试环境失败：") + QString::fromUtf8(error.what());
            m_session.reset();
        }
    }

    /** 销毁会话并等待网络与磁盘线程结束，不删除测试拥有的源文件。 */
    ~TorrentTestPeer() = default;
    /** 做种会话拥有唯一生命周期，禁止复制后共享临时目录句柄。 */
    TorrentTestPeer(const TorrentTestPeer&) = delete;
    /** 禁止复制赋值覆盖仍在运行的测试会话。 */
    TorrentTestPeer& operator=(const TorrentTestPeer&) = delete;
    /** 是否已经取得可连接的回环端口并成功加载种子。 */
    bool isValid() const { return m_error.isEmpty() && m_session && m_handle.is_valid() && !m_magnet.isEmpty(); }
    /** 返回构造期间发生的具体错误，供测试断言打印。 */
    QString error() const { return m_error; }
    /** 返回包含本机 x.pe 节点提示的 v1 磁力链接。 */
    QString magnet() const { return m_magnet; }
    /** 返回两个文件的原始索引、相对路径和预期大小。 */
    QVariantList files() const { return m_files; }

private:
    static constexpr int FileSize = 256 * 1024; ///< 每份确定内容测试文件的长度。
    QString m_error; ///< 初始化失败原因；空值表示没有记录错误。
    QString m_magnet; ///< 已就绪的本机磁力链接。
    QVariantList m_files; ///< 从元数据 layout 读取的权威文件清单。
    std::unique_ptr<libtorrent::session> m_session; ///< 拥有独立线程的本地做种会话。
    libtorrent::torrent_handle m_handle; ///< 当前测试种子的弱引用句柄，先于会话释放。
};
