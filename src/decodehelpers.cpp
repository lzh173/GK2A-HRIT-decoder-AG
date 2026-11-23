#include "decodehelpers.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <regex>

namespace hritapp {

QString findKeyFile(const QString& chosen, const QString& inputPath, const QString& outputDir) {
    QStringList candidates;
    if (!chosen.isEmpty()) candidates << chosen;
    const QDir app(QCoreApplication::applicationDirPath());
    candidates << app.filePath(QStringLiteral("EncryptionKeyMessage.bin"))
               << app.filePath(QStringLiteral("resources/gk2a/EncryptionKeyMessage.bin"));
    if (!inputPath.isEmpty())
        candidates << QFileInfo(inputPath).dir().filePath(QStringLiteral("EncryptionKeyMessage.bin"));
    candidates << QDir(outputDir).filePath(QStringLiteral("EncryptionKeyMessage.bin"));
    for (const QString& c : candidates)
        if (QFileInfo(c).isFile()) return QDir::toNativeSeparators(QFileInfo(c).absoluteFilePath());
    return {};
}

std::string observationStamp(const std::string& fileName) {
    static const std::regex re("_(\\d{8}_\\d{6})_");
    std::smatch m;
    return std::regex_search(fileName, m, re) ? m[1].str() : std::string();
}

}  // namespace hritapp
