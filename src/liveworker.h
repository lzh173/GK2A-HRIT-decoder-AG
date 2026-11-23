// liveworker.h - decodes a live SDR stream with the same receive chain as a recording.
#pragma once
#include "pipeline.h"
#include "sdrsource.h"

#include <QImage>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>
#include <atomic>

Q_DECLARE_METATYPE(hritapp::SdrConfig)

class LiveDecodeWorker : public QObject {
    Q_OBJECT
public:
    explicit LiveDecodeWorker(QObject* parent = nullptr) : QObject(parent) {}
    // Thread-safe; the run slot notices it within one read timeout and still finalises the images.
    void requestStop() { stop_.store(true); }

public slots:
    void run(const hritapp::SdrConfig& config, const QString& outputDir, const QString& keyPath, bool dcBlock);

signals:
    void inputInfo(const QString& description, double sampleRate);
    void logLine(const QString& text);
    void progress(int permille);
    void statsChanged(const hritapp::PipelineStats& stats);
    void constellation(const std::vector<std::complex<float>>& points);
    void livePreview(const QImage& image, const QString& stamp);
    void observationDone(const QString& stamp, const QStringList& paths);
    void finished(bool ok, const QString& summary);

private:
    std::atomic<bool> stop_{false};
};
