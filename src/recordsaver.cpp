#include "recordsaver.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>

namespace hritapp {

RecordSaver::RecordSaver(const QString& root, bool saveHrit, bool saveCadu)
    : root_(root), saveHrit_(saveHrit), saveCadu_(saveCadu) {
    if (!saveHrit_ && !saveCadu_) return;
    const QString stamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    if (saveCadu_) {
        const QString dir = QDir(root_).filePath(QStringLiteral("CADU"));
        if (!QDir().mkpath(dir)) {
            error_ = QStringLiteral("无法创建 CADU 输出目录：%1").arg(dir);
            saveCadu_ = false;
        } else {
            caduPath_ = QDir(dir).filePath(QStringLiteral("%1.cadu").arg(stamp));
            caduFile_.setFileName(caduPath_);
            if (!caduFile_.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                error_ = QStringLiteral("无法创建 CADU 文件：%1").arg(caduPath_);
                caduPath_.clear();
                saveCadu_ = false;
            }
        }
    }
}

RecordSaver::~RecordSaver() { close(); }

void RecordSaver::close() {
    if (caduFile_.isOpen()) {
        caduFile_.flush();
        caduFile_.close();
    }
}

bool RecordSaver::saveCadu(const std::vector<std::uint8_t>& bytes) {
    if (!saveCadu_ || !caduFile_.isOpen() || bytes.size() != 1024) return true;
    if (caduFile_.write(reinterpret_cast<const char*>(bytes.data()), 1024) != 1024) {
        error_ = QStringLiteral("写 CADU 失败：%1").arg(caduPath_);
        saveCadu_ = false;
        return false;
    }
    ++caduFrames_;
    return true;
}

bool RecordSaver::saveHrit(const nadirstation::gk2a::LritFile& file) {
    if (!saveHrit_) return true;
    if (file.bytes.empty()) return true;
    const QString dir = QDir(root_).filePath(QStringLiteral("LRIT"));
    if (!QDir().mkpath(dir)) {
        error_ = QStringLiteral("无法创建 LRIT 输出目录：%1").arg(dir);
        saveHrit_ = false;
        return false;
    }
    QString name = QFileInfo(QString::fromStdString(file.fileName)).fileName();
    if (name.isEmpty()) name = QStringLiteral("file_%1_%2.hrit").arg(file.apid).arg(hritFiles_ + 1);
    const QString path = QDir(dir).filePath(name);
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) ||
        out.write(reinterpret_cast<const char*>(file.bytes.data()), static_cast<qint64>(file.bytes.size())) !=
            static_cast<qint64>(file.bytes.size()) ||
        !out.commit()) {
        error_ = QStringLiteral("写 LRIT 文件失败：%1").arg(path);
        saveHrit_ = false;
        return false;
    }
    ++hritFiles_;
    return true;
}

}  // namespace hritapp
