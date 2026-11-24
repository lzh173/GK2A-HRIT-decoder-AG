#include "mainwindow.h"

#include "androidoutput.h"
#include "appinfo.h"
#include "constellationwidget.h"
#include "iqreader.h"
#include "liveworker.h"
#include "qtpath.h"

#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

#ifdef Q_OS_ANDROID
#include <QJniEnvironment>
#include <QJniObject>
#endif

namespace {

bool isSupportedRecording(const QString& path) {
    hritapp::IqFormat format;
    // Looks at the header first (ZIQ_ / ZIQ2 / RIFF), then at the extension.
    return hritapp::formatForPath(toFsPath(path), format);
}

QString humanSize(qint64 bytes) {
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    return mb >= 1024.0 ? QStringLiteral("%1 GB").arg(mb / 1024.0, 0, 'f', 2) : QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
}

// Android's document picker can hand back a content:// URI. The decoder works on a filesystem
// path, so copy the selected document into the app cache once and open that copy.
QString materializeInputFile(const QString& path) {
#ifdef Q_OS_ANDROID
    if (path.startsWith(QLatin1String("content://")) || !QFileInfo(path).isFile()) {
        QFile in(path);
        if (!in.open(QIODevice::ReadOnly)) return {};
        const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        if (cache.isEmpty()) return {};
        QDir().mkpath(cache);
        QString name = QFileInfo(path).fileName();
        if (name.isEmpty()) name = QStringLiteral("recording");
        const QString dst = QDir(cache).filePath(name);
        QFile::remove(dst);
        if (!in.copy(dst)) return {};
        return dst;
    }
#endif
    return path;
}

QLabel* valueLabel() {
    auto* l = new QLabel(QStringLiteral("—"));
    l->setObjectName(QStringLiteral("Value"));
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return l;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("%1 %2").arg(QString::fromUtf8(appinfo::kProductName), QString::fromUtf8(appinfo::kVersion)));
    setWindowIcon(QIcon(QStringLiteral(":/icon.png")));
    setAcceptDrops(true);
#ifdef Q_OS_ANDROID
    resize(420, 760);
    setMinimumSize(360, 640);
#else
    resize(1240, 780);
    setMinimumSize(1200, 740);
#endif
    buildUi();

    worker_ = new DecodeWorker;
    worker_->moveToThread(&thread_);
    connect(this, &MainWindow::startRequested, worker_, &DecodeWorker::run);
    connect(worker_, &DecodeWorker::inputInfo, this, [this](const QString& text, double) { inputInfo_->setText(text); });
    connect(worker_, &DecodeWorker::logLine, this, &MainWindow::onLog);
    connect(worker_, &DecodeWorker::progress, this, &MainWindow::onProgress);
    connect(worker_, &DecodeWorker::statsChanged, this, &MainWindow::onStats);
    connect(worker_, &DecodeWorker::constellation, this,
            [this](const std::vector<std::complex<float>>& points) { constellation_->setPoints(points); });
    connect(worker_, &DecodeWorker::livePreview, this, &MainWindow::onLivePreview);
    connect(worker_, &DecodeWorker::observationDone, this, &MainWindow::onObservationDone);
    connect(worker_, &DecodeWorker::finished, this, &MainWindow::onFinished);

    liveWorker_ = new LiveDecodeWorker;
    liveWorker_->moveToThread(&thread_);
    connect(this, &MainWindow::startLiveRequested, liveWorker_, &LiveDecodeWorker::run);
    connect(liveWorker_, &LiveDecodeWorker::inputInfo, this,
            [this](const QString& text, double) { inputInfo_->setText(text); });
    connect(liveWorker_, &LiveDecodeWorker::logLine, this, &MainWindow::onLog);
    connect(liveWorker_, &LiveDecodeWorker::progress, this, &MainWindow::onProgress);
    connect(liveWorker_, &LiveDecodeWorker::statsChanged, this, &MainWindow::onStats);
    connect(liveWorker_, &LiveDecodeWorker::constellation, this,
            [this](const std::vector<std::complex<float>>& points) { constellation_->setPoints(points); });
    connect(liveWorker_, &LiveDecodeWorker::livePreview, this, &MainWindow::onLivePreview);
    connect(liveWorker_, &LiveDecodeWorker::observationDone, this, &MainWindow::onObservationDone);
    connect(liveWorker_, &LiveDecodeWorker::finished, this, &MainWindow::onFinished);
    thread_.start();

    QSettings settings;
#ifdef Q_OS_ANDROID
    outputEdit_->setText(hritapp::AndroidOutput::instance().hasTreeUri()
                             ? hritapp::AndroidOutput::instance().displayName()
                             : QStringLiteral("尚未选择输出目录"));
    connect(&hritapp::AndroidOutput::instance(), &hritapp::AndroidOutput::treeUriChanged, this,
            [this](const QString& display) {
                outputEdit_->setText(display.isEmpty() ? QStringLiteral("尚未选择输出目录") : display);
            });
#else
    outputEdit_->setText(settings.value(QStringLiteral("outputDir"), outputRoot()).toString());
#endif
    keyEdit_->setText(settings.value(QStringLiteral("keyFile")).toString());
    rateSpin_->setValue(settings.value(QStringLiteral("manualRateMsps"), 6.0).toDouble());
    dcBlock_->setChecked(settings.value(QStringLiteral("dcBlock"), true).toBool());
    carrierHint_->setValue(settings.value(QStringLiteral("carrierHintKHz"), 0.0).toDouble());
    saveHritFiles_->setChecked(settings.value(QStringLiteral("saveHritFiles"), true).toBool());
    saveCadu_->setChecked(settings.value(QStringLiteral("saveCadu"), false).toBool());

    const int source = std::clamp(settings.value(QStringLiteral("source"), 0).toInt(), 0, 2);
    {
        QSignalBlocker block(sourceCombo_);
        sourceCombo_->setCurrentIndex(source);
    }
    onSourceChanged(source);
    liveFreq_->setValue(settings.value(QStringLiteral("liveFreqMHz"), 1695.4).toDouble());
    liveRate_->setValue(settings.value(QStringLiteral("liveRateMsps"), liveRate_->value()).toDouble());
    liveGain_->setValue(settings.value(QStringLiteral("liveGainDb"), liveGain_->value()).toDouble());
    liveLna_->setValue(settings.value(QStringLiteral("liveLnaDb"), liveLna_->value()).toInt());
    liveVga_->setValue(settings.value(QStringLiteral("liveVgaDb"), liveVga_->value()).toInt());
    livePpm_->setValue(settings.value(QStringLiteral("livePpm"), 0).toInt());
    liveAmp_->setChecked(settings.value(QStringLiteral("liveAmp"), false).toBool());
    autoGain_->setChecked(settings.value(QStringLiteral("liveAutoGain"), autoGain_->isChecked()).toBool());
    biasTee_->setChecked(settings.value(QStringLiteral("liveBiasTee"), false).toBool());
    statusBar()->showMessage(QStringLiteral("就绪：选择文件录音，或从 HackRF / RTL-SDR 实时接收。"));
}

MainWindow::~MainWindow() {
    if (worker_) worker_->requestStop();
    if (liveWorker_) liveWorker_->requestStop();
    thread_.quit();
    thread_.wait();
    delete worker_;       // its thread has finished, so deleting it from here is safe
    delete liveWorker_;
}

void MainWindow::buildUi() {
    auto* central = new QWidget;
#ifdef Q_OS_ANDROID
    auto* root = new QVBoxLayout(central);
#else
    auto* root = new QHBoxLayout(central);
#endif
    root->setContentsMargins(12, 12, 12, 8);
    root->setSpacing(12);

    // ---- left: what to decode ---------------------------------------------------------------------------
    auto* left = new QWidget;
#ifndef Q_OS_ANDROID
    left->setFixedWidth(344);
#endif
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(10);

    auto* title = new QLabel(QStringLiteral("GK-2A HRIT 解码器"));
    title->setObjectName(QStringLiteral("AppTitle"));
    auto* subtitle = new QLabel(QStringLiteral("解码基带录音或 HackRF / RTL-SDR 实时流 → LRIT 文件 → 云图与 L2 产品"));
    subtitle->setObjectName(QStringLiteral("Muted"));
    subtitle->setWordWrap(true);
    leftLayout->addWidget(title);
    leftLayout->addWidget(subtitle);

    auto* sourceBox = new QGroupBox(QStringLiteral("1  信号源"));
    auto* sourceLayout = new QVBoxLayout(sourceBox);
    sourceCombo_ = new QComboBox;
    sourceCombo_->addItem(QStringLiteral("文件录音"));
    sourceCombo_->addItem(QStringLiteral("HackRF（USB 实时）"));
    sourceCombo_->addItem(QStringLiteral("RTL-SDR（USB 实时）"));
    sourceLayout->addWidget(sourceCombo_);
    leftLayout->addWidget(sourceBox);

    fileBox_ = new QGroupBox(QStringLiteral("2  录音文件"));
    auto* inputLayout = new QVBoxLayout(fileBox_);
    auto* inputRow = new QHBoxLayout;
    inputEdit_ = new QLineEdit;
    inputEdit_->setPlaceholderText(QStringLiteral("拖入 cs8 / cs16 / cs32 / cf32 / cu8 / wav / ziq / ziq2，或点右侧“浏览”"));
    auto* browseIn = new QPushButton(QStringLiteral("浏览…"));
    inputRow->addWidget(inputEdit_, 1);
    inputRow->addWidget(browseIn);
    inputLayout->addLayout(inputRow);
    inputInfo_ = new QLabel(QStringLiteral("尚未选择文件。"));
    inputInfo_->setObjectName(QStringLiteral("Muted"));
    inputInfo_->setWordWrap(true);
    auto* rateRow = new QHBoxLayout;
    manualRate_ = new QCheckBox(QStringLiteral("手动指定采样率"));
    rateSpin_ = new QDoubleSpinBox;
    rateSpin_->setDecimals(6);
    rateSpin_->setRange(0.5, 100.0);
    rateSpin_->setValue(6.0);
    rateSpin_->setSuffix(QStringLiteral(" MS/s"));
    rateSpin_->setEnabled(false);
    rateRow->addWidget(manualRate_);
    rateRow->addWidget(rateSpin_, 1);
    inputLayout->addLayout(rateRow);
    auto* rateHint = new QLabel(QStringLiteral("cs8 / cs16 / cs32 / cf32 / cu8 本身不带采样率：会先看同名 .txt 里的 rate、再看文件名里的“…SPS”（常见的“…SPS”命名也认），都没有时用这里的值。WAV / ZIQ / ZIQ2 用文件头里的采样率。采样率须不低于 3.15 MS/s。"));
    rateHint->setObjectName(QStringLiteral("Muted"));
    rateHint->setWordWrap(true);
    inputLayout->addWidget(rateHint);
    leftLayout->addWidget(fileBox_);

    liveBox_ = new QGroupBox(QStringLiteral("2  实时接收参数"));
    auto* liveLayout = new QVBoxLayout(liveBox_);
    auto* deviceRow = new QHBoxLayout;
    deviceCombo_ = new QComboBox;
    refreshDevicesBtn_ = new QPushButton(QStringLiteral("刷新"));
    deviceRow->addWidget(new QLabel(QStringLiteral("设备")));
    deviceRow->addWidget(deviceCombo_, 1);
    deviceRow->addWidget(refreshDevicesBtn_);
    liveLayout->addLayout(deviceRow);
    const auto makeRow = [](const QString& text, QWidget* widget) {
        auto* row = new QWidget;
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->addWidget(new QLabel(text));
        h->addWidget(widget, 1);
        return row;
    };
    liveFreq_ = new QDoubleSpinBox;
    liveFreq_->setDecimals(4);
    liveFreq_->setRange(1.0, 6000.0);
    liveFreq_->setValue(1695.4);
    liveFreq_->setSuffix(QStringLiteral(" MHz"));
    liveLayout->addWidget(makeRow(QStringLiteral("中心频率"), liveFreq_));
    liveRate_ = new QDoubleSpinBox;
    liveRate_->setDecimals(3);
    liveRate_->setRange(1.0, 20.0);
    liveRate_->setValue(6.0);
    liveRate_->setSuffix(QStringLiteral(" MS/s"));
    liveLayout->addWidget(makeRow(QStringLiteral("采样率"), liveRate_));
    liveGain_ = new QDoubleSpinBox;
    liveGain_->setDecimals(1);
    liveGain_->setRange(0.0, 50.0);
    liveGain_->setValue(40.0);
    liveGain_->setSuffix(QStringLiteral(" dB"));
    rtlGainRow_ = makeRow(QStringLiteral("调谐增益"), liveGain_);
    liveLayout->addWidget(rtlGainRow_);
    liveLna_ = new QSpinBox;
    liveLna_->setRange(0, 40);
    liveLna_->setSingleStep(8);
    liveLna_->setValue(32);
    liveLna_->setSuffix(QStringLiteral(" dB"));
    hackLnaRow_ = makeRow(QStringLiteral("LNA"), liveLna_);
    liveLayout->addWidget(hackLnaRow_);
    liveVga_ = new QSpinBox;
    liveVga_->setRange(0, 62);
    liveVga_->setSingleStep(2);
    liveVga_->setValue(40);
    liveVga_->setSuffix(QStringLiteral(" dB"));
    hackVgaRow_ = makeRow(QStringLiteral("VGA"), liveVga_);
    liveLayout->addWidget(hackVgaRow_);
    auto* liveFlags = new QHBoxLayout;
    autoGain_ = new QCheckBox(QStringLiteral("自动增益"));
    liveAmp_ = new QCheckBox(QStringLiteral("RF 放大 +14 dB"));
    biasTee_ = new QCheckBox(QStringLiteral("Bias-Tee"));
    liveFlags->addWidget(autoGain_);
    liveFlags->addWidget(liveAmp_);
    liveFlags->addWidget(biasTee_);
    liveLayout->addLayout(liveFlags);
    livePpm_ = new QSpinBox;
    livePpm_->setRange(-100, 100);
    livePpm_->setSuffix(QStringLiteral(" ppm"));
    ppmRow_ = makeRow(QStringLiteral("频率校正"), livePpm_);
    liveLayout->addWidget(ppmRow_);
    liveHint_ = new QLabel(QStringLiteral("HackRF 建议 6 MS/s；RTL-SDR 最高 3.2 MS/s。库文件（hackrf.dll / rtlsdr.dll）需放在程序目录或 PATH。"));
    liveHint_->setObjectName(QStringLiteral("Muted"));
    liveHint_->setWordWrap(true);
    liveLayout->addWidget(liveHint_);
    leftLayout->addWidget(liveBox_);
    leftLayout->addWidget(inputInfo_);

    dcBlock_ = new QCheckBox(QStringLiteral("去除直流偏置（推荐）"));
    dcBlock_->setChecked(true);
    dcBlock_->setToolTip(QStringLiteral("在解调前用 500 Hz 高通去掉 I/Q 的直流偏置：SDR 的中心尖峰、本振泄漏和 ADC 偏置都会让 AGC 和载波环偏离。\n"
                                        "HRIT 信号从直流一直铺到约 2.25 MHz，500 Hz 的拐点对信号几乎没有影响。只有确认录音本身没有直流问题时才需要关掉。"));
    leftLayout->addWidget(dcBlock_);

    auto* carrierRow = new QHBoxLayout;
    auto* carrierLabel = new QLabel(QStringLiteral("起始频偏（锁定 ±5 kHz）"));
    carrierHint_ = new QDoubleSpinBox;
    carrierHint_->setRange(-1500.0, 1500.0);
    carrierHint_->setDecimals(1);
    carrierHint_->setSingleStep(5.0);
    carrierHint_->setSuffix(QStringLiteral(" kHz"));
    carrierHint_->setValue(0.0);
    carrierHint_->setToolTip(QStringLiteral("0 = 自动搜索（±250 kHz）。\n"
                                            "非 0 = 从这个频偏开始，只在该值 ±5 kHz 内搜索并锁定；用于已知频偏的录音，"
                                            "或自动搜索被干扰空载波带偏时。可以超过自动的 ±250 kHz 上限。"));
    carrierRow->addWidget(carrierLabel);
    carrierRow->addWidget(carrierHint_, 1);
    leftLayout->addLayout(carrierRow);

    saveHritFiles_ = new QCheckBox(QStringLiteral("保存原始 HRIT 文件"));
    saveHritFiles_->setChecked(true);
    saveHritFiles_->setToolTip(QStringLiteral("在输出目录的 LRIT/ 下保存重组后的原始 .hrit 文件（与常见 HRIT 解码器一致），"
                                              "而不只是生成图片。"));
    leftLayout->addWidget(saveHritFiles_);
    saveCadu_ = new QCheckBox(QStringLiteral("保存 CADU 帧（1024 字节/帧）"));
    saveCadu_->setChecked(false);
    saveCadu_->setToolTip(QStringLiteral("把每个带同步字的 1024 字节 CADU 追加写入输出目录的 CADU/*.cadu，"
                                         "便于外部解码或分析。文件会比较大。"));
    leftLayout->addWidget(saveCadu_);

    auto* outBox = new QGroupBox(QStringLiteral("3  输出文件夹"));
    auto* outLayout = new QVBoxLayout(outBox);
    auto* outRow = new QHBoxLayout;
    outputEdit_ = new QLineEdit;
    auto* browseOut = new QPushButton(QStringLiteral("浏览…"));
    outRow->addWidget(outputEdit_, 1);
    outRow->addWidget(browseOut);
    outLayout->addLayout(outRow);
    openOutBtn_ = new QPushButton(QStringLiteral("打开输出文件夹"));
    outLayout->addWidget(openOutBtn_);
#ifdef Q_OS_ANDROID
    browseOut->setText(QStringLiteral("选择目录…"));
    openOutBtn_->setVisible(false);
    outputEdit_->setReadOnly(true);
    auto* outHint = new QLabel(QStringLiteral("选择手机上的目录；解码完成后会自动把结果导出到该目录。"));
    outHint->setObjectName(QStringLiteral("Muted"));
    outHint->setWordWrap(true);
    outLayout->addWidget(outHint);
#endif
    leftLayout->addWidget(outBox);

    auto* keyBox = new QGroupBox(QStringLiteral("4  密钥文件（加密的图像段需要）"));
    auto* keyLayout = new QVBoxLayout(keyBox);
    auto* keyRow = new QHBoxLayout;
    keyEdit_ = new QLineEdit;
    keyEdit_->setPlaceholderText(QStringLiteral("EncryptionKeyMessage.bin"));
    auto* browseKey = new QPushButton(QStringLiteral("浏览…"));
    keyRow->addWidget(keyEdit_, 1);
    keyRow->addWidget(browseKey);
    keyLayout->addLayout(keyRow);
    auto* keyHint = new QLabel(QStringLiteral("本软件不含密钥。该文件由卫星运营方发给注册用户，请使用你自己合法取得的那份。留空时会在程序目录、录音所在目录里找。"));
    keyHint->setObjectName(QStringLiteral("Muted"));
    keyHint->setWordWrap(true);
    keyLayout->addWidget(keyHint);
    leftLayout->addWidget(keyBox);

    leftLayout->addStretch(1);
    startBtn_ = new QPushButton(QStringLiteral("开始解码"));
    startBtn_->setObjectName(QStringLiteral("Primary"));
    startBtn_->setMinimumHeight(38);
    progress_ = new QProgressBar;
    progress_->setRange(0, 1000);
    progress_->setValue(0);
    progress_->setTextVisible(false);
    progress_->setFixedHeight(8);
    leftLayout->addWidget(startBtn_);
    leftLayout->addWidget(progress_);

    // ---- right: what is happening and what came out ----------------------------------------------------
    auto* right = new QWidget;
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(10);

    auto* topRow = new QBoxLayout(QBoxLayout::LeftToRight);
#ifdef Q_OS_ANDROID
    topRow->setDirection(QBoxLayout::TopToBottom);
#endif
    topRow->setSpacing(10);
    auto* constBox = new QGroupBox(QStringLiteral("星座图"));
    auto* constLayout = new QVBoxLayout(constBox);
    constellation_ = new ConstellationWidget;
    constellation_->setAnnotation(QStringLiteral("QPSK"), QStringLiteral("GK-2A HRIT · 3 Msym/s · RRC 0.5"));
    constellation_->setFixedSize(288, 288);
    constLayout->addWidget(constellation_);
    topRow->addWidget(constBox);

    auto* previewBox = new QGroupBox(QStringLiteral("实时预览"));
    auto* previewLayout = new QVBoxLayout(previewBox);
    previewLabel_ = new QLabel(QStringLiteral("等待图像数据…"));
    previewLabel_->setAlignment(Qt::AlignCenter);
    previewLabel_->setFixedSize(288, 288);          // square frame; the picture is fitted inside it
    previewLabel_->setObjectName(QStringLiteral("Preview"));
    previewCaption_ = new QLabel;
    previewCaption_->setObjectName(QStringLiteral("Muted"));
    previewLayout->addWidget(previewLabel_);
    previewLayout->addWidget(previewCaption_);
    topRow->addWidget(previewBox);

    auto* statBox = new QGroupBox(QStringLiteral("链路状态"));
    auto* grid = new QGridLayout(statBox);
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(8);
    const auto addStat = [&](int row, const QString& name, QLabel*& value) {
        auto* n = new QLabel(name);
        n->setObjectName(QStringLiteral("Muted"));
        value = valueLabel();
        grid->addWidget(n, row, 0);
        grid->addWidget(value, row, 1);
    };
    addStat(0, QStringLiteral("帧同步 / 映射"), lockValue_);
    addStat(1, QStringLiteral("载波质量"), qualityValue_);
    addStat(2, QStringLiteral("频偏"), offsetValue_);
    addStat(3, QStringLiteral("信噪比（估计）"), snrValue_);
    addStat(4, QStringLiteral("RS 通过 / 同步帧"), rsValue_);
    addStat(5, QStringLiteral("LRIT 数据包"), packetValue_);
    addStat(6, QStringLiteral("完成的文件（图像段）"), fileValue_);
    addStat(7, QStringLiteral("CRC 失败"), crcValue_);
    grid->setRowStretch(8, 1);
    grid->setColumnStretch(1, 1);
    topRow->addWidget(statBox, 1);
    rightLayout->addLayout(topRow);

    auto* productBox = new QGroupBox(QStringLiteral("已输出的观测（双击打开文件夹）"));
    auto* productLayout = new QVBoxLayout(productBox);
    products_ = new QListWidget;
    productLayout->addWidget(products_);
    rightLayout->addWidget(productBox, 1);

    auto* logBox = new QGroupBox(QStringLiteral("日志"));
    auto* logLayout = new QVBoxLayout(logBox);
    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(2000);
    log_->setFixedHeight(110);
    logLayout->addWidget(log_);
    rightLayout->addWidget(logBox);

#ifdef Q_OS_ANDROID
    auto* leftScroll = new QScrollArea;
    leftScroll->setWidgetResizable(true);
    leftScroll->setWidget(left);
    auto* rightScroll = new QScrollArea;
    rightScroll->setWidgetResizable(true);
    rightScroll->setWidget(right);
    auto* tabs = new QTabWidget;
    tabs->addTab(leftScroll, QStringLiteral("解码"));
    tabs->addTab(rightScroll, QStringLiteral("状态"));
    root->addWidget(tabs);
#else
    root->addWidget(left);
    root->addWidget(right, 1);
#endif
    setCentralWidget(central);

    // ---- bottom strip: credit and agreement -------------------------------------------------------------
    auto* credit = new QLabel(QStringLiteral("开发者：%1 · v%2").arg(QString::fromUtf8(appinfo::kDeveloper), QString::fromUtf8(appinfo::kVersion)));
    credit->setObjectName(QStringLiteral("Credit"));
    auto* agreementBtn = new QPushButton(QStringLiteral("用户协议与免责声明"));
    agreementBtn->setObjectName(QStringLiteral("Link"));
    agreementBtn->setFlat(true);
    agreementBtn->setCursor(Qt::PointingHandCursor);
    auto* aboutBtn = new QPushButton(QStringLiteral("关于"));
    aboutBtn->setObjectName(QStringLiteral("Link"));
    aboutBtn->setFlat(true);
    aboutBtn->setCursor(Qt::PointingHandCursor);
    statusBar()->addPermanentWidget(credit);
    statusBar()->addPermanentWidget(agreementBtn);
    statusBar()->addPermanentWidget(aboutBtn);
    statusBar()->setSizeGripEnabled(false);

    connect(browseIn, &QPushButton::clicked, this, &MainWindow::browseInput);
    connect(sourceCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onSourceChanged);
    connect(refreshDevicesBtn_, &QPushButton::clicked, this, &MainWindow::refreshDevices);
    connect(browseOut, &QPushButton::clicked, this, &MainWindow::browseOutput);
    connect(browseKey, &QPushButton::clicked, this, &MainWindow::browseKey);
    connect(openOutBtn_, &QPushButton::clicked, this, &MainWindow::openOutputFolder);
    connect(startBtn_, &QPushButton::clicked, this, &MainWindow::toggleRun);
    connect(agreementBtn, &QPushButton::clicked, this, &MainWindow::showAgreement);
    connect(aboutBtn, &QPushButton::clicked, this, &MainWindow::showAbout);
    connect(inputEdit_, &QLineEdit::editingFinished, this, &MainWindow::inspectInput);
    connect(manualRate_, &QCheckBox::toggled, this, [this](bool on) {
        rateSpin_->setEnabled(on);
        inspectInput();
    });
    connect(rateSpin_, &QDoubleSpinBox::editingFinished, this, &MainWindow::inspectInput);
    connect(products_, &QListWidget::itemActivated, this, [](QListWidgetItem* item) {
        const QString dir = item->data(Qt::UserRole).toString();
        if (!dir.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
}

QString MainWindow::outputRoot() const {
#ifdef Q_OS_ANDROID
    // Decode into app-private staging; the worker exports the finished tree to the SAF directory.
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(base.isEmpty() ? QDir::homePath() : base).filePath(QStringLiteral("output"));
#else
    const QString typed = outputEdit_ ? outputEdit_->text().trimmed() : QString();
    if (!typed.isEmpty()) return typed;
    return QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath(QStringLiteral("NadirStation_GK2A-HRIT"));
#endif
}

void MainWindow::onSourceChanged(int index) {
    liveMode_ = index > 0;
    fileBox_->setVisible(!liveMode_);
    liveBox_->setVisible(liveMode_);
    if (!liveMode_) {
        inspectInput();
        return;
    }
    const hritapp::SdrKind kind = index == 1 ? hritapp::SdrKind::HackRf : hritapp::SdrKind::RtlSdr;
    liveRate_->setMaximum(hritapp::sdrMaxSampleRate(kind) / 1e6);
    liveRate_->setSingleStep(kind == hritapp::SdrKind::HackRf ? 1.0 : 0.1);
    liveRate_->setValue(hritapp::sdrDefaultSampleRate(kind) / 1e6);
    liveLna_->setValue(32);
    liveVga_->setValue(40);
    liveGain_->setValue(40.0);
    autoGain_->setChecked(kind == hritapp::SdrKind::RtlSdr);
    rtlGainRow_->setVisible(kind == hritapp::SdrKind::RtlSdr);
    hackLnaRow_->setVisible(kind == hritapp::SdrKind::HackRf);
    hackVgaRow_->setVisible(kind == hritapp::SdrKind::HackRf);
    ppmRow_->setVisible(kind == hritapp::SdrKind::RtlSdr);
    autoGain_->setVisible(kind == hritapp::SdrKind::RtlSdr);
    liveAmp_->setVisible(kind == hritapp::SdrKind::HackRf);
    biasTee_->setVisible(true);
    if (kind == hritapp::SdrKind::HackRf)
        liveHint_->setText(QStringLiteral("HackRF：建议 6 MS/s；1.7 GHz 接收建议外加 LNA / 滤波器。"
                                          "库文件 hackrf.dll 与驱动需放在程序目录或 PATH。"));
    else
        liveHint_->setText(QStringLiteral("RTL-SDR：最高 3.2 MS/s，1.7 GHz 灵敏度较低，建议外加 LNA / 滤波器。"
                                          "库文件 rtlsdr.dll 与 libusb-1.0.dll 需放在程序目录或 PATH。"));
    refreshDevices();
}

void MainWindow::refreshDevices() {
    if (!liveMode_) return;
#ifdef Q_OS_ANDROID
    // libusb's Android backend needs the JavaVM before the first libusb_init. Ask the user for USB
    // permission for known SDR VID/PIDs at the same time; the system dialog is asynchronous.
    hritapp::SdrSource::setAndroidJavaVm(QJniEnvironment::javaVM());
    QJniObject activity = QNativeInterface::QAndroidApplication::context();
    if (activity.isValid()) activity.callMethod<void>("requestSdrPermissions", "()V");
#endif
    const hritapp::SdrKind kind =
        sourceCombo_->currentIndex() == 1 ? hritapp::SdrKind::HackRf : hritapp::SdrKind::RtlSdr;
    deviceCombo_->clear();
    std::string error;
    const auto devices = hritapp::SdrSource::enumerate(kind, error);
    for (const auto& device : devices) {
        const int row = deviceCombo_->count();
        deviceCombo_->addItem(QString::fromStdString(device.name), device.index);
        deviceCombo_->setItemData(row, QString::fromStdString(device.serial), Qt::UserRole + 1);
    }
    if (devices.empty()) {
        deviceCombo_->addItem(QStringLiteral("未找到设备"));
        if (!error.empty()) {
            const QString message = QString::fromUtf8(error.c_str());
            liveHint_->setText(liveHint_->text().section(QLatin1Char('\n'), 0, 0) + QStringLiteral("\n") + message);
            onLog(QStringLiteral("SDR: %1").arg(message));
        }
    } else {
        liveHint_->setText(liveHint_->text().section(QLatin1Char('\n'), 0, 0) +
                           QStringLiteral("\n找到 %1 个设备。").arg(devices.size()));
    }
#ifdef Q_OS_ANDROID
    liveHint_->setText(liveHint_->text() +
                       QStringLiteral("\n首次使用请在系统弹窗中允许 USB 权限，然后再点一次“刷新”。"));
#endif
}

hritapp::SdrConfig MainWindow::liveConfig() const {
    hritapp::SdrConfig config;
    config.kind = sourceCombo_->currentIndex() == 1 ? hritapp::SdrKind::HackRf : hritapp::SdrKind::RtlSdr;
    config.deviceIndex = deviceCombo_->currentData().toInt();
    config.serial = deviceCombo_->currentData(Qt::UserRole + 1).toString().toStdString();
    config.sampleRate = liveRate_->value() * 1e6;
    config.centerFrequency = liveFreq_->value() * 1e6;
    config.gainDb = liveGain_->value();
    config.lnaGainDb = liveLna_->value();
    config.vgaGainDb = liveVga_->value();
    config.amplifier = liveAmp_->isChecked();
    config.autoGain = autoGain_->isChecked();
    config.biasTee = biasTee_->isChecked();
    config.ppm = livePpm_->value();
    return config;
}

void MainWindow::browseInput() {
    QSettings settings;
    const QString start = settings.value(QStringLiteral("inputDir"), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).toString();
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择基带录音"), start,
                                                      QStringLiteral("基带录音 (*.cs8 *.cs16 *.cs32 *.cf32 *.cu8 *.s8 *.s16 *.s32 *.f32 *.u8 *.wav *.w16 *.ziq *.ziq2);;所有文件 (*)"));
    if (path.isEmpty()) return;
    const QString local = materializeInputFile(path);
    if (local.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法读取文件"),
                             QStringLiteral("无法读取所选文件（Android 文件选择器返回的 URI 无法复制）。"));
        return;
    }
#ifndef Q_OS_ANDROID
    settings.setValue(QStringLiteral("inputDir"), QFileInfo(local).absolutePath());
#endif
    setInputPath(local);
}

void MainWindow::browseOutput() {
#ifdef Q_OS_ANDROID
    hritapp::AndroidOutput::instance().pickDirectory(this);
    return;
#else
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择输出文件夹"), outputRoot());
    if (!dir.isEmpty()) outputEdit_->setText(QDir::toNativeSeparators(dir));
#endif
}

void MainWindow::browseKey() {
    const QString start = keyEdit_->text().isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) : keyEdit_->text();
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择密钥文件"), start,
                                                      QStringLiteral("密钥文件 (*.bin);;所有文件 (*)"));
    if (!path.isEmpty()) {
        const QString local = materializeInputFile(path);
        if (local.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("无法读取文件"),
                                 QStringLiteral("无法读取所选密钥文件（Android 文件选择器返回的 URI 无法复制）。"));
            return;
        }
        keyEdit_->setText(QDir::toNativeSeparators(local));
    }
}

void MainWindow::openOutputFolder() {
    const QString dir = outputRoot();
    QDir().mkpath(dir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void MainWindow::setInputPath(const QString& path) {
    const QString local = materializeInputFile(path);
    if (local.isEmpty()) {
        inputInfo_->setText(QStringLiteral("无法读取所选文件（Android 文件选择器返回的 URI 无法复制）。"));
        return;
    }
    if (sourceCombo_->currentIndex() != 0) {
        QSignalBlocker block(sourceCombo_);
        sourceCombo_->setCurrentIndex(0);
        onSourceChanged(0);
    }
    inputEdit_->setText(QDir::toNativeSeparators(local));
    inspectInput();
}

// Read just the header of the chosen file and say what it is, so mistakes show before the long run.
void MainWindow::inspectInput() {
    if (running_) return;
    const QString path = inputEdit_->text().trimmed();
    inputValid_ = false;
    if (path.isEmpty()) {
        inputInfo_->setText(QStringLiteral("尚未选择文件。"));
        return;
    }
    if (!QFileInfo(path).isFile()) {
        inputInfo_->setText(QStringLiteral("找不到这个文件。"));
        return;
    }
    hritapp::IqReader reader;
    std::string error;
    const bool preferManual = manualRate_->isChecked();
    if (!reader.open(toFsPath(path), rateSpin_->value() * 1e6, error, preferManual)) {
        inputInfo_->setText(QString::fromUtf8(error.c_str()));
        return;
    }
    const hritapp::IqInfo info = reader.info();
    const bool hasRate = hritapp::formatCarriesRate(info.format);
    manualRate_->setEnabled(!hasRate);
    if (hasRate && manualRate_->isChecked()) {
        QSignalBlocker block(manualRate_);
        manualRate_->setChecked(false);
        rateSpin_->setEnabled(false);
    }
    const bool rateUnknown = (!hasRate && info.rateSource == "手动设置" && !preferManual);
    if (rateUnknown && !manualRate_->isChecked()) {
        QSignalBlocker block(manualRate_);
        manualRate_->setChecked(true);
        rateSpin_->setEnabled(true);
    } else if (!rateUnknown && !preferManual) {
        QSignalBlocker block(rateSpin_);
        rateSpin_->setValue(info.sampleRate / 1e6);   // show what was detected
    }
    const QString duration = info.framesExact
                                 ? QStringLiteral("%1 秒").arg(static_cast<double>(info.frames) / info.sampleRate, 0, 'f', 1)
                                 : QStringLiteral("时长未知");
    QString text = QStringLiteral("%1 · %2 MS/s（%3）· %4 · %5")
                       .arg(QString::fromUtf8(hritapp::describeFormat(info).c_str()))
                       .arg(info.sampleRate / 1e6, 0, 'f', 3)
                       .arg(QString::fromUtf8(info.rateSource.c_str()))
                       .arg(duration)
                       .arg(humanSize(QFileInfo(path).size()));
    if (rateUnknown) text += QStringLiteral("\n文件没有标明采样率，暂用上面的值，请核对后再开始。");
    if (!info.framesExact)
        text += QStringLiteral("\n这个格式的采样总数要到读完才知道：进度按文件位置显示，补帧（按样本位置回头重解）不可用。");
    if (info.sampleRate < hritapp::HritPipeline::kMinSampleRate)
        text += QStringLiteral("\n采样率太低：GK-2A HRIT 至少需要 3.15 MS/s。");
    inputInfo_->setText(text);
    inputValid_ = info.sampleRate >= hritapp::HritPipeline::kMinSampleRate;
}

void MainWindow::toggleRun() {
    if (running_) {
        if (!stopping_) {
            stopping_ = true;
            worker_->requestStop();
            liveWorker_->requestStop();
            startBtn_->setEnabled(false);
            startBtn_->setText(QStringLiteral("正在停止…"));
            onLog(QStringLiteral("已请求停止，正在保存已收到的内容…"));
        }
        return;
    }
#ifdef Q_OS_ANDROID
    if (!hritapp::AndroidOutput::instance().hasTreeUri()) {
        QMessageBox::information(this, QStringLiteral("还没有选择输出目录"),
                                 QStringLiteral("请先在“输出文件夹”里选择一个手机上的目录；结果会自动导出到那里。"));
        return;
    }
#endif
    if (liveMode_) {
        if (deviceCombo_->currentIndex() < 0 || deviceCombo_->currentText().startsWith(QStringLiteral("未找到"))) {
            QMessageBox::information(this, QStringLiteral("没有找到 SDR"),
                                     QStringLiteral("没有找到可用的 SDR 设备。请检查 USB 连接、驱动，以及程序目录或 PATH 中的运行库。"));
            return;
        }
        const double rate = liveRate_->value() * 1e6;
        if (!hritapp::sdrSampleRateOk(rate)) {
            QMessageBox::warning(this, QStringLiteral("采样率太低"),
                                 QStringLiteral("GK-2A HRIT 至少需要 3.15 MS/s；当前是 %1 MS/s。")
                                     .arg(liveRate_->value(), 0, 'f', 3));
            return;
        }
        const hritapp::SdrConfig config = liveConfig();
        const QString out = outputRoot();
#ifndef Q_OS_ANDROID
        outputEdit_->setText(QDir::toNativeSeparators(out));
#endif
        QSettings settings;
        settings.setValue(QStringLiteral("outputDir"), outputEdit_->text());
        settings.setValue(QStringLiteral("keyFile"), keyEdit_->text());
        settings.setValue(QStringLiteral("source"), sourceCombo_->currentIndex());
        settings.setValue(QStringLiteral("dcBlock"), dcBlock_->isChecked());
    settings.setValue(QStringLiteral("carrierHintKHz"), carrierHint_->value());
    settings.setValue(QStringLiteral("saveHritFiles"), saveHritFiles_->isChecked());
    settings.setValue(QStringLiteral("saveCadu"), saveCadu_->isChecked());
        settings.setValue(QStringLiteral("liveFreqMHz"), liveFreq_->value());
        settings.setValue(QStringLiteral("liveRateMsps"), liveRate_->value());
        settings.setValue(QStringLiteral("liveGainDb"), liveGain_->value());
        settings.setValue(QStringLiteral("liveLnaDb"), liveLna_->value());
        settings.setValue(QStringLiteral("liveVgaDb"), liveVga_->value());
        settings.setValue(QStringLiteral("livePpm"), livePpm_->value());
        settings.setValue(QStringLiteral("liveAmp"), liveAmp_->isChecked());
        settings.setValue(QStringLiteral("liveAutoGain"), autoGain_->isChecked());
        settings.setValue(QStringLiteral("liveBiasTee"), biasTee_->isChecked());

        log_->clear();
        products_->clear();
        previewLabel_->setPixmap(QPixmap());
        previewLabel_->setText(QStringLiteral("等待图像数据…"));
        previewCaption_->clear();
        lastPreview_ = QImage();
        progress_->setRange(0, 0);
        progress_->setValue(0);
        onStats(hritapp::PipelineStats{});
        setRunning(true);
        emit startLiveRequested(config, out, keyEdit_->text().trimmed(), dcBlock_->isChecked(),
                                carrierHint_->value() * 1000.0, saveHritFiles_->isChecked(), saveCadu_->isChecked());
        return;
    }

    const QString path = inputEdit_->text().trimmed();
    if (path.isEmpty() || !QFileInfo(path).isFile() || !isSupportedRecording(path)) {
        QMessageBox::information(this, QStringLiteral("还没有选好录音"),
                                 QStringLiteral("请先选择一个基带录音文件（cs8、cs16、cs32、cf32、cu8、wav、ziq 或 ziq2）。"));
        return;
    }
    inspectInput();
    if (!inputValid_) {
        QMessageBox::warning(this, QStringLiteral("录音不能用"), inputInfo_->text());
        return;
    }
    const QString out = outputRoot();
#ifndef Q_OS_ANDROID
    outputEdit_->setText(QDir::toNativeSeparators(out));
#endif
    QSettings settings;
#ifdef Q_OS_ANDROID
    settings.setValue(QStringLiteral("androidOutputTree"), hritapp::AndroidOutput::instance().displayName());
#else
    settings.setValue(QStringLiteral("outputDir"), outputEdit_->text());
#endif
    settings.setValue(QStringLiteral("keyFile"), keyEdit_->text());
    settings.setValue(QStringLiteral("manualRateMsps"), rateSpin_->value());
    settings.setValue(QStringLiteral("dcBlock"), dcBlock_->isChecked());
    settings.setValue(QStringLiteral("carrierHintKHz"), carrierHint_->value());
    settings.setValue(QStringLiteral("saveHritFiles"), saveHritFiles_->isChecked());
    settings.setValue(QStringLiteral("saveCadu"), saveCadu_->isChecked());
    settings.setValue(QStringLiteral("source"), sourceCombo_->currentIndex());

    log_->clear();
    products_->clear();
    previewLabel_->setPixmap(QPixmap());
    previewLabel_->setText(QStringLiteral("等待图像数据…"));
    previewCaption_->clear();
    lastPreview_ = QImage();
    progress_->setRange(0, 1000);
    progress_->setValue(0);
    onStats(hritapp::PipelineStats{});
    setRunning(true);
    emit startRequested(path, rateSpin_->value() * 1e6, manualRate_->isChecked(), out, keyEdit_->text().trimmed(),
                        dcBlock_->isChecked(), carrierHint_->value() * 1000.0, saveHritFiles_->isChecked(),
                        saveCadu_->isChecked());
}

void MainWindow::setRunning(bool running) {
    running_ = running;
    stopping_ = false;
    startBtn_->setEnabled(true);
    startBtn_->setText(running ? QStringLiteral("停止")
                               : (liveMode_ ? QStringLiteral("开始接收") : QStringLiteral("开始解码")));
    sourceCombo_->setEnabled(!running);
    fileBox_->setEnabled(!running);
    liveBox_->setEnabled(!running);
    outputEdit_->setEnabled(!running);
    keyEdit_->setEnabled(!running);
    manualRate_->setEnabled(!running);
    dcBlock_->setEnabled(!running);
    rateSpin_->setEnabled(!running && manualRate_->isChecked());
    statusBar()->showMessage(running ? (liveMode_ ? QStringLiteral("接收中…") : QStringLiteral("解码中…"))
                                     : QStringLiteral("就绪"));
}

void MainWindow::onLog(const QString& text) {
    log_->appendPlainText(QStringLiteral("[%1] %2").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")), text));
}

void MainWindow::onProgress(int permille) {
    if (permille < 0) {
        if (progress_->minimum() != 0 || progress_->maximum() != 0) progress_->setRange(0, 0);
        statusBar()->showMessage(QStringLiteral("实时接收中…"));
        return;
    }
    if (progress_->maximum() == 0) progress_->setRange(0, 1000);
    progress_->setValue(permille);
    statusBar()->showMessage(QStringLiteral("解码中… %1%").arg(permille / 10.0, 0, 'f', 1));
}

void MainWindow::onStats(const hritapp::PipelineStats& s) {
    const bool locked = s.locked;
    lockValue_->setText(locked ? QStringLiteral("已同步 · 映射 %1").arg(s.mapping) : QStringLiteral("搜索中"));
    lockValue_->setStyleSheet(locked ? QStringLiteral("color:#1b7f3b;") : QStringLiteral("color:#a86b00;"));
    qualityValue_->setText(QStringLiteral("%1%").arg(s.lockQuality * 100.0f, 0, 'f', 0));
    offsetValue_->setText(QStringLiteral("%1 Hz").arg(s.carrierOffsetHz, 0, 'f', 0));
    snrValue_->setText(QStringLiteral("%1 dB").arg(s.snrDb, 0, 'f', 1));
    const double rate = s.cadusWithSync ? 100.0 * static_cast<double>(s.cadusRsOk) / static_cast<double>(s.cadusWithSync) : 0.0;
    rsValue_->setText(QStringLiteral("%1 / %2（%3%）").arg(s.cadusRsOk).arg(s.cadusWithSync).arg(rate, 0, 'f', 0));
    packetValue_->setText(QString::number(s.packets));
    fileValue_->setText(QString::number(s.filesCompleted));
    crcValue_->setText(QString::number(s.crcFailures));
}

void MainWindow::onLivePreview(const QImage& image, const QString& stamp) {
    if (image.isNull()) return;
    lastPreview_ = image.width() > 1400 ? image.scaledToWidth(1400, Qt::SmoothTransformation) : image;
    previewCaption_->setText(QStringLiteral("观测 %1").arg(stamp));
    updatePreview();
}

void MainWindow::updatePreview() {
    if (lastPreview_.isNull()) return;
    const QSize box = previewLabel_->size() - QSize(8, 8);
    if (box.width() < 20 || box.height() < 20) return;
    previewLabel_->setPixmap(QPixmap::fromImage(lastPreview_.scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    updatePreview();
}

void MainWindow::onObservationDone(const QString& stamp, const QStringList& paths) {
    if (paths.isEmpty()) return;
    const QString dir = QFileInfo(paths.first()).absolutePath();
    auto* item = new QListWidgetItem(QStringLiteral("%1  ·  %2 个文件").arg(stamp).arg(paths.size()));
    item->setData(Qt::UserRole, dir);
    item->setToolTip(QDir::toNativeSeparators(dir) + QLatin1Char('\n') + paths.join(QLatin1Char('\n')));
    products_->addItem(item);
    onLog(QStringLiteral("观测 %1 已保存：%2").arg(stamp, QDir::toNativeSeparators(dir)));
}

void MainWindow::onFinished(bool ok, const QString& summary) {
    setRunning(false);
    if (progress_->maximum() == 0) progress_->setRange(0, 1000);
    progress_->setValue(ok ? progress_->value() : 0);
    onLog(summary);
    statusBar()->showMessage(summary.section(QLatin1Char('\n'), 0, 0));
    if (!ok) QMessageBox::warning(this, QStringLiteral("接收没有完成"), summary);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (running_ || !event->mimeData()->hasUrls()) return;
    for (const QUrl& url : event->mimeData()->urls())
        if (url.isLocalFile() && isSupportedRecording(url.toLocalFile())) {
            event->acceptProposedAction();
            return;
        }
}

void MainWindow::dropEvent(QDropEvent* event) {
    for (const QUrl& url : event->mimeData()->urls())
        if (url.isLocalFile() && isSupportedRecording(url.toLocalFile())) {
            setInputPath(url.toLocalFile());
            return;
        }
}

// The agreement is accepted in the installer; this just opens the same text (EULA.txt, installed next to the program).
void MainWindow::showAgreement() {
#ifdef Q_OS_ANDROID
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("用户协议与免责声明"));
    dialog.resize(360, 560);
    auto* layout = new QVBoxLayout(&dialog);
    auto* browser = new QTextBrowser(&dialog);
    browser->setSource(QUrl(QStringLiteral("qrc:/eula.html")));
    layout->addWidget(browser);
    auto* close = new QPushButton(QStringLiteral("关闭"), &dialog);
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    layout->addWidget(close);
    dialog.exec();
    return;
#else
    const QString path = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("EULA.txt"));
    if (QFileInfo::exists(path) && QDesktopServices::openUrl(QUrl::fromLocalFile(path))) return;
    QMessageBox::information(this, QStringLiteral("用户协议与免责声明"),
                             QStringLiteral("找不到 EULA.txt。请重新安装本软件，或在官网阅读完整协议。"));
#endif
}

void MainWindow::showAbout() {
    QMessageBox::about(this, QStringLiteral("关于"),
        QStringLiteral("<h3>%1</h3>"
                       "<p>版本 %2<br>开发者：%3</p>"
                       "<p>解码 GK-2A（千里眼-2A）HRIT 下行：可直接读取基带录音，也可从 HackRF / RTL-SDR 实时接收。QPSK 解调 → Viterbi / Reed-Solomon → LRIT 文件 → 云图与 L2 产品。支持 cs8 / cs16 / cs32 / cf32 / cu8 / wav / ziq / ziq2（按文件头自动识别）；只接收，不发射，不联网。</p>"
                       "<p>本软件是业余爱好者的学习研究项目，不是气象业务系统，也与韩国气象厅、国家气象卫星中心无任何隶属或授权关系。使用前请阅读“用户协议与免责声明”。</p>"
                       "<p style='color:#8a919c'>第三方：Qt 6（LGPL v3）、OpenJPEG（BSD-2）、libjpeg-turbo（BSD/IJG/zlib）、zstd（BSD-3，仅用于读取压缩 ZIQ）、Natural Earth（公有领域，国界为中国视角版本）。</p>")
            .arg(QString::fromUtf8(appinfo::kProductName), QString::fromUtf8(appinfo::kVersion), QString::fromUtf8(appinfo::kDeveloper)));
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (running_) {
        const auto answer = QMessageBox::question(this, QStringLiteral("正在接收"),
                                                  QStringLiteral("接收还在进行。退出会先保存已收到的内容，可能需要几十秒。确定退出吗？"));
        if (answer != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        worker_->requestStop();
        liveWorker_->requestStop();
    }
    QSettings settings;
    settings.setValue(QStringLiteral("outputDir"), outputEdit_->text());
    settings.setValue(QStringLiteral("keyFile"), keyEdit_->text());
    settings.setValue(QStringLiteral("manualRateMsps"), rateSpin_->value());
    settings.setValue(QStringLiteral("dcBlock"), dcBlock_->isChecked());
    settings.setValue(QStringLiteral("carrierHintKHz"), carrierHint_->value());
    settings.setValue(QStringLiteral("saveHritFiles"), saveHritFiles_->isChecked());
    settings.setValue(QStringLiteral("saveCadu"), saveCadu_->isChecked());
    settings.setValue(QStringLiteral("source"), sourceCombo_->currentIndex());
    settings.setValue(QStringLiteral("liveFreqMHz"), liveFreq_->value());
    settings.setValue(QStringLiteral("liveRateMsps"), liveRate_->value());
    settings.setValue(QStringLiteral("liveGainDb"), liveGain_->value());
    settings.setValue(QStringLiteral("liveLnaDb"), liveLna_->value());
    settings.setValue(QStringLiteral("liveVgaDb"), liveVga_->value());
    settings.setValue(QStringLiteral("livePpm"), livePpm_->value());
    settings.setValue(QStringLiteral("liveAmp"), liveAmp_->isChecked());
    settings.setValue(QStringLiteral("liveAutoGain"), autoGain_->isChecked());
    settings.setValue(QStringLiteral("liveBiasTee"), biasTee_->isChecked());
    event->accept();
}
