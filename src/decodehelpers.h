// decodehelpers.h - small pieces shared by the file and live decode workers.
#pragma once
#include <QString>
#include <string>

namespace hritapp {

// The decryption key is not part of this program: it is issued to registered users by the operator.
// Look for the file the user chose, then next to the program, then next to the recording (if any),
// then in the output folder.
QString findKeyFile(const QString& chosen, const QString& inputPath, const QString& outputDir);

// "IMG_FD_143_SW038_20261103_062903_05.hrit" -> "20261103_062903": which observation a segment
// belongs to.
std::string observationStamp(const std::string& fileName);

}  // namespace hritapp
