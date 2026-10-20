#include "Host.h"
#include "AppStorage.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUrl>

namespace {
// 凭据与 Windows 版一样留在应用数据目录，只是改用文件权限约束；不依赖桌面密钥环进程。
QString credentialPath() {
    return AppStorage::dataDirectory() + "/account.credential";
}

// 按 XDG 顺序查找桌面条目；用户目录优先，系统目录兜底。
QStringList desktopDirectories() {
    QStringList directories;
    for (const auto &base : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) {
        directories << base + "/applications";
    }
    directories << QStringLiteral("/usr/local/share/applications")
                << QStringLiteral("/usr/share/applications");
    directories.removeDuplicates();
    return directories;
}

// 只按桌面条目判断协议是否已注册，不猜测客户端安装路径，也不自行解析启动命令。
bool orpheusRegistered() {
    for (const auto &directory : desktopDirectories()) {
        QDirIterator iterator(directory, {"*.desktop"}, QDir::Files);
        while (iterator.hasNext()) {
            QFile file(iterator.next());
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                continue;
            }
            if (file.readAll().contains("x-scheme-handler/orpheus")) {
                return true;
            }
        }
    }
    return false;
}
}

QByteArray platformCredential() {
    QFile file(credentialPath());
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

QString platformSaveCredential(const QByteArray &bytes) {
    const auto directory = QFileInfo(credentialPath()).absolutePath();
    if (!QDir().mkpath(directory)) {
        return QStringLiteral("登录成功，但 Linux 凭据当前不可写");
    }
    // 创建时就限定 0600，避免登录态在写入期间对其他用户可读。
    const auto path = QFile::encodeName(credentialPath());
    const auto handle = ::open(path.constData(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (handle < 0) {
        return QStringLiteral("登录成功，但 Linux 凭据当前不可写");
    }
    // 已有的旧文件不受创建模式位影响，登录前遗留的宽权限同样收紧。
    ::fchmod(handle, 0600);
    const auto written = ::write(handle, bytes.constData(), size_t(bytes.size()));
    ::close(handle);
    return written == bytes.size() ? QString() : QStringLiteral("登录成功，但 Linux 凭据当前不可写");
}

void platformDeleteCredential() {
    QFile::remove(credentialPath());
}

void platformPlay(const QString &id, bool activate, std::function<void(QString)> done) {
    Q_UNUSED(activate)
    if (!orpheusRegistered()) {
        done(QStringLiteral("未安装或未注册网易云音乐客户端，请安装官方客户端后重试"));
        return;
    }
    // 与 macOS、Windows 逐字一致的协议载荷；播放器始终是已安装的官方客户端。
    const auto payload =
        QString("{\"type\":\"song\",\"id\":\"%1\",\"cmd\":\"play\"}").arg(id).toUtf8().toBase64();
    const auto url = QUrl(QString("orpheus://") + QString::fromLatin1(payload));
    if (QDesktopServices::openUrl(url)) {
        done(QStringLiteral("已交给网易云；若冷启动未切歌，可手动重试"));
    } else {
        done(QStringLiteral("交付失败：无法启动网易云客户端"));
    }
}
