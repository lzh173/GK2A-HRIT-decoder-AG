#include "liveworker.h"

#include "androidoutput.h"
#include "decodehelpers.h"
#include "gk2ahritimages.h"
#include "recordsaver.h"

#include <QDir>
#include <QThread>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

void LiveDecodeWorker::run(const hritapp::SdrConfig& config, const QString& outputDir, const QString& keyPath,
                           bool dcBlock, double carrierOffsetHintHz, bool saveHritFiles, bool saveCadu) {
    using clock = std::chrono::steady_clock;
    stop_.store(false);

#ifdef Q_OS_ANDROID
    QDir(outputDir).removeRecursively();   // staging only: the previous run was already exported
#endif
    if (!QDir().mkpath(outputDir)) {
        emit finished(false, QStringLiteral("无法创建输出文件夹：%1").arg(outputDir));
        return;
    }

    hritapp::SdrSource source;
    std::string error;
    if (!source.open(config, error)) {
        emit finished(false, QString::fromUtf8(error.c_str()));
        return;
    }
    const double rate = source.sampleRate();
    if (!hritapp::sdrSampleRateOk(rate)) {
        source.close();
        emit finished(false, QStringLiteral("采样率 %1 MS/s 太低：GK-2A HRIT 至少需要 3.15 MS/s。")
                                  .arg(rate / 1e6, 0, 'f', 3));
        return;
    }

    hritapp::HritPipeline pipe;
    if (!pipe.configure(rate, error, dcBlock, false, carrierOffsetHintHz)) {
        source.close();
        emit finished(false, QString::fromUtf8(error.c_str()));
        return;
    }
    hritapp::RecordSaver saver(outputDir, saveHritFiles, saveCadu);
    pipe.setCaduSink([&saver](const std::vector<std::uint8_t>& bytes) { saver.saveCadu(bytes); });
    if (saveHritFiles) emit logLine(QStringLiteral("原始 HRIT 文件输出到：%1").arg(QDir(outputDir).filePath(QStringLiteral("LRIT"))));
    if (saveCadu) emit logLine(QStringLiteral("CADU 输出到：%1").arg(saver.caduPath()));
    if (!saver.lastError().isEmpty()) emit logLine(saver.lastError());

    nadirstation::gk2a::HritImages images;
    const QString key = hritapp::findKeyFile(keyPath, QString(), outputDir);
    if (key.isEmpty())
        emit logLine(QStringLiteral("没有找到密钥文件 EncryptionKeyMessage.bin：加密的图像段无法解密。"));
    else
        emit logLine(QStringLiteral("使用密钥文件：%1").arg(key));

    emit inputInfo(QStringLiteral("%1 · %2 MS/s · 中心 %3 MHz")
                       .arg(QString::fromUtf8(hritapp::sdrKindDescription(config.kind).c_str()))
                       .arg(rate / 1e6, 0, 'f', 3)
                       .arg(config.centerFrequency / 1e6, 0, 'f', 4),
                   rate);
    if (config.kind == hritapp::SdrKind::HackRf)
        emit logLine(QStringLiteral("实时接收：HackRF · %1 MS/s · 中心 %2 MHz · LNA %3 dB · VGA %4 dB · AMP %5 · 偏置 %6")
                         .arg(rate / 1e6, 0, 'f', 3)
                         .arg(config.centerFrequency / 1e6, 0, 'f', 4)
                         .arg(config.lnaGainDb)
                         .arg(config.vgaGainDb)
                         .arg(config.amplifier ? QStringLiteral("开") : QStringLiteral("关"))
                         .arg(config.biasTee ? QStringLiteral("开") : QStringLiteral("关")));
    else
        emit logLine(QStringLiteral("实时接收：RTL-SDR · %1 MS/s · 中心 %2 MHz · %3 · PPM %4 · 偏置 %5")
                         .arg(rate / 1e6, 0, 'f', 3)
                         .arg(config.centerFrequency / 1e6, 0, 'f', 4)
                         .arg(config.autoGain ? QStringLiteral("自动增益")
                                              : QStringLiteral("增益 %1 dB").arg(config.gainDb, 0, 'f', 1))
                         .arg(config.ppm)
                         .arg(config.biasTee ? QStringLiteral("开") : QStringLiteral("关")));
    emit logLine(dcBlock ? QStringLiteral("直流抑制：开（500 Hz 高通，去除中心尖峰 / LO 泄漏）。")
                         : QStringLiteral("直流抑制：关。"));

    int observations = 0;
    const auto publish = [&] {
        auto update = images.takeUpdate();
        if (!update.message.isEmpty()) emit logLine(update.message);
        if (!update.live.isNull()) emit livePreview(update.live, update.stamp);
        for (auto& done : images.takeCompleted()) {
            if (!done.message.isEmpty()) emit logLine(done.message);
            if (!done.paths.isEmpty()) {
                ++observations;
                emit observationDone(done.stamp, done.paths);
            }
        }
    };

    const std::function<void(nadirstation::gk2a::LritFile&&)> onFile =
        [&](nadirstation::gk2a::LritFile&& file) {
            if (file.bytes.size() > nadirstation::gk2a::LritFileAssembler::kMaxFileBytes) return;
            saver.saveHrit(file);
            for (int tries = 0; tries < 400 && !stop_.load(); ++tries) {
                if (images.queuedBytes() > (24u << 20)) {
                    QThread::msleep(25);
                    continue;
                }
                if (images.submit(std::move(file), outputDir, key)) return;   // on refusal `file` is untouched
                QThread::msleep(25);
            }
        };

    std::vector<std::complex<float>> block;
    std::vector<std::complex<float>> lastConstellation;
    constexpr std::size_t kBlockFrames = 65536;
    const auto started = clock::now();
    auto lastUi = started - std::chrono::seconds(1);
    auto lastLog = started;
    bool failed = false;
    std::string readError;

    while (!stop_.load()) {
        readError.clear();
        const std::size_t n = source.read(block, kBlockFrames, 200, readError);
        const auto now = clock::now();
        if (!readError.empty()) {
            emit logLine(QString::fromUtf8(readError.c_str()));
            failed = true;
            break;
        }
        if (n > 0) {
            const auto& symbols = pipe.process(block, onFile);
            if (!symbols.empty()) {
                const std::size_t take = std::min<std::size_t>(symbols.size(), 2500);
                lastConstellation.assign(symbols.end() - static_cast<std::ptrdiff_t>(take), symbols.end());
            }
        }
        if (now - lastUi >= std::chrono::milliseconds(150)) {
            lastUi = now;
            emit progress(-1);   // live: the window shows an indeterminate bar
            emit statsChanged(pipe.stats());
            if (!lastConstellation.empty()) emit constellation(lastConstellation);
            publish();
        }
        if (now - lastLog >= std::chrono::seconds(5)) {
            lastLog = now;
            const auto s = pipe.stats();
            emit logLine(QStringLiteral("实时 %1 秒 · 接收 %2 MSamples · 丢弃 %3 · RS 通过 %4 / 同步 %5 · 数据包 %6 · 完成文件 %7")
                             .arg(std::chrono::duration<double>(now - started).count(), 0, 'f', 0)
                             .arg(source.samplesReceived() / 1e6, 0, 'f', 1)
                             .arg(source.samplesDropped() / 1e6, 0, 'f', 2)
                             .arg(s.cadusRsOk)
                             .arg(s.cadusWithSync)
                             .arg(s.packets)
                             .arg(s.filesCompleted));
        }
    }

    pipe.finish(onFile);
    source.close();
    emit statsChanged(pipe.stats());
    emit logLine(QStringLiteral("数据流已停止，正在合成并保存图像…"));
    images.drain();
    images.requestFinalize();
    images.drain();
    publish();

    saver.close();
    if (saveHritFiles)
        emit logLine(QStringLiteral("原始 HRIT 文件：%1 个，位于 %2")
                         .arg(saver.hritFiles())
                         .arg(QDir(outputDir).filePath(QStringLiteral("LRIT"))));
    if (saveCadu)
        emit logLine(QStringLiteral("CADU：%1 帧，%2").arg(saver.caduFrames()).arg(saver.caduPath()));
    if (!saver.lastError().isEmpty()) emit logLine(saver.lastError());

    const auto stats = pipe.stats();
    const double seconds = std::chrono::duration<double>(clock::now() - started).count();
    QString summary = QStringLiteral("%1：实时 %2 秒 · 接收 %3 MSamples · 丢弃 %4 MSamples · RS 通过 %5 / 同步 %6 · "
                                     "数据包 %7 · 完成文件 %8 · 输出 %9 次观测")
                          .arg(failed ? QStringLiteral("接收中断") : (stop_.load() ? QStringLiteral("已停止")
                                                                                  : QStringLiteral("接收结束")))
                          .arg(seconds, 0, 'f', 1)
                          .arg(source.samplesReceived() / 1e6, 0, 'f', 1)
                          .arg(source.samplesDropped() / 1e6, 0, 'f', 2)
                          .arg(stats.cadusRsOk)
                          .arg(stats.cadusWithSync)
                          .arg(stats.packets)
                          .arg(stats.filesCompleted)
                          .arg(observations);
    if (stats.cadusRsOk == 0)
        summary += QStringLiteral("\n没有解出任何有效帧：请确认中心频率（GK-2A HRIT 1695.4 MHz）、采样率、增益和天线。");
    else if (observations == 0)
        summary += QStringLiteral("\n还没有拼出完整图像：接收时间可能不够（一幅全圆盘约需 10 分钟以上），或缺少密钥文件。");
#ifdef Q_OS_ANDROID
    emit logLine(QStringLiteral("正在把结果导出到所选目录…"));
    QString exportError;
    if (!hritapp::AndroidOutput::instance().exportDirectory(outputDir, exportError)) {
        summary += QStringLiteral("\n导出到所选目录失败：") + exportError;
        emit finished(false, summary);
        return;
    }
    emit logLine(QStringLiteral("已导出到所选目录。"));
#endif
    emit finished(!failed, summary);
}
