// mainwindow.h - the whole window of the GK-2A HRIT decoder: pick a recording or a live SDR, press
// start, watch it lock, open the images.
#pragma once
#include "decodeworker.h"
#include "sdrsource.h"

#include <QImage>
#include <QMainWindow>
#include <QThread>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QWidget;
class ConstellationWidget;
class LiveDecodeWorker;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

signals:
    void startRequested(const QString& inputPath, double manualRateHz, bool preferManual, const QString& outputDir,
                        const QString& keyPath, bool dcBlock, double carrierOffsetHintHz);
    void startLiveRequested(const hritapp::SdrConfig& config, const QString& outputDir, const QString& keyPath,
                            bool dcBlock, double carrierOffsetHintHz);

protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void browseInput();
    void browseOutput();
    void browseKey();
    void openOutputFolder();
    void toggleRun();
    void inspectInput();
    void onSourceChanged(int index);
    void refreshDevices();
    void showAgreement();
    void showAbout();
    void onLog(const QString& text);
    void onProgress(int permille);
    void onStats(const hritapp::PipelineStats& stats);
    void onLivePreview(const QImage& image, const QString& stamp);
    void onObservationDone(const QString& stamp, const QStringList& paths);
    void onFinished(bool ok, const QString& summary);

private:
    void buildUi();
    void setInputPath(const QString& path);
    void setRunning(bool running);
    void updatePreview();
    hritapp::SdrConfig liveConfig() const;
    QString outputRoot() const;

    // input
    QComboBox* sourceCombo_ = nullptr;
    QGroupBox* fileBox_ = nullptr;
    QGroupBox* liveBox_ = nullptr;
    QComboBox* deviceCombo_ = nullptr;
    QPushButton* refreshDevicesBtn_ = nullptr;
    QWidget* rtlGainRow_ = nullptr;
    QWidget* hackLnaRow_ = nullptr;
    QWidget* hackVgaRow_ = nullptr;
    QWidget* ppmRow_ = nullptr;
    QLabel* liveHint_ = nullptr;
    QDoubleSpinBox* liveFreq_ = nullptr;
    QDoubleSpinBox* liveRate_ = nullptr;
    QDoubleSpinBox* liveGain_ = nullptr;
    QSpinBox* liveLna_ = nullptr;
    QSpinBox* liveVga_ = nullptr;
    QSpinBox* livePpm_ = nullptr;
    QCheckBox* liveAmp_ = nullptr;
    QCheckBox* autoGain_ = nullptr;
    QCheckBox* biasTee_ = nullptr;
    QLineEdit* inputEdit_ = nullptr;
    QLabel* inputInfo_ = nullptr;
    QCheckBox* manualRate_ = nullptr;
    QCheckBox* dcBlock_ = nullptr;
    QDoubleSpinBox* carrierHint_ = nullptr;
    QDoubleSpinBox* rateSpin_ = nullptr;
    QLineEdit* outputEdit_ = nullptr;
    QLineEdit* keyEdit_ = nullptr;
    QPushButton* startBtn_ = nullptr;
    QPushButton* openOutBtn_ = nullptr;
    QProgressBar* progress_ = nullptr;
    // results
    ConstellationWidget* constellation_ = nullptr;
    QLabel* lockValue_ = nullptr;
    QLabel* qualityValue_ = nullptr;
    QLabel* offsetValue_ = nullptr;
    QLabel* snrValue_ = nullptr;
    QLabel* rsValue_ = nullptr;
    QLabel* packetValue_ = nullptr;
    QLabel* fileValue_ = nullptr;
    QLabel* crcValue_ = nullptr;
    QLabel* previewLabel_ = nullptr;
    QLabel* previewCaption_ = nullptr;
    QListWidget* products_ = nullptr;
    QPlainTextEdit* log_ = nullptr;

    QThread thread_;
    DecodeWorker* worker_ = nullptr;
    LiveDecodeWorker* liveWorker_ = nullptr;
    QImage lastPreview_;
    bool running_ = false;
    bool stopping_ = false;
    bool inputValid_ = false;
    bool liveMode_ = false;
};
