// recordsaver.h - optional raw LRIT/HRIT file and CADU output.
#pragma once

#include "gk2adatalink.h"

#include <QFile>
#include <QString>

#include <cstdint>
#include <vector>

namespace hritapp {

// Writes the same useful by-products as a normal HRIT decoder:
//   <root>/LRIT/<original>.hrit     assembled LRIT files, byte-for-byte
//   <root>/CADU/<timestamp>.cadu    concatenated 1024-byte CADUs that had a sync marker
// Directories are created lazily; both features are optional.
class RecordSaver {
public:
    RecordSaver(const QString& root, bool saveHrit, bool saveCadu);
    ~RecordSaver();

    bool saveHrit(const nadirstation::gk2a::LritFile& file);
    bool saveCadu(const std::vector<std::uint8_t>& bytes);
    void close();

    std::uint64_t hritFiles() const { return hritFiles_; }
    std::uint64_t caduFrames() const { return caduFrames_; }
    QString caduPath() const { return caduPath_; }
    const QString& lastError() const { return error_; }

private:
    QString root_;
    bool saveHrit_ = false;
    bool saveCadu_ = false;
    QFile caduFile_;
    QString caduPath_;
    std::uint64_t hritFiles_ = 0;
    std::uint64_t caduFrames_ = 0;
    QString error_;
};

}  // namespace hritapp
