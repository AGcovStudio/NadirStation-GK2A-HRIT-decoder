#include "mainwindow.h"

#include "appinfo.h"
#include "constellationwidget.h"
#include "iqreader.h"
#include "qtpath.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
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
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTextBrowser>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {

bool isSupportedRecording(const QString& path) {
    const QString ext = QFileInfo(path).suffix().toLower();
    return ext == QLatin1String("cs8") || ext == QLatin1String("cs16") || ext == QLatin1String("wav");
}

QString humanSize(qint64 bytes) {
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    return mb >= 1024.0 ? QStringLiteral("%1 GB").arg(mb / 1024.0, 0, 'f', 2) : QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
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
    resize(1240, 780);
    setMinimumSize(1200, 740);
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
    thread_.start();

    QSettings settings;
    outputEdit_->setText(settings.value(QStringLiteral("outputDir"), outputRoot()).toString());
    keyEdit_->setText(settings.value(QStringLiteral("keyFile")).toString());
    rateSpin_->setValue(settings.value(QStringLiteral("manualRateMsps"), 6.0).toDouble());
    statusBar()->showMessage(QStringLiteral("就绪：把 .cs8 / .cs16 / .wav 录音拖进窗口，或点“浏览”选择。"));
}

MainWindow::~MainWindow() {
    if (worker_) worker_->requestStop();
    thread_.quit();
    thread_.wait();
    delete worker_;   // its thread has finished, so deleting it from here is safe
}

void MainWindow::buildUi() {
    auto* central = new QWidget;
    auto* root = new QHBoxLayout(central);
    root->setContentsMargins(12, 12, 12, 8);
    root->setSpacing(12);

    // ---- left: what to decode ---------------------------------------------------------------------------
    auto* left = new QWidget;
    left->setFixedWidth(344);
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(10);

    auto* title = new QLabel(QStringLiteral("GK-2A HRIT 解码器"));
    title->setObjectName(QStringLiteral("AppTitle"));
    auto* subtitle = new QLabel(QStringLiteral("离线解码基带录音 → LRIT 文件 → 云图与 L2 产品"));
    subtitle->setObjectName(QStringLiteral("Muted"));
    subtitle->setWordWrap(true);
    leftLayout->addWidget(title);
    leftLayout->addWidget(subtitle);

    auto* inputBox = new QGroupBox(QStringLiteral("1  录音文件"));
    auto* inputLayout = new QVBoxLayout(inputBox);
    auto* inputRow = new QHBoxLayout;
    inputEdit_ = new QLineEdit;
    inputEdit_->setPlaceholderText(QStringLiteral("拖入 .cs8 / .cs16 / .wav，或点右侧“浏览”"));
    auto* browseIn = new QPushButton(QStringLiteral("浏览…"));
    inputRow->addWidget(inputEdit_, 1);
    inputRow->addWidget(browseIn);
    inputLayout->addLayout(inputRow);
    inputInfo_ = new QLabel(QStringLiteral("尚未选择文件。"));
    inputInfo_->setObjectName(QStringLiteral("Muted"));
    inputInfo_->setWordWrap(true);
    inputLayout->addWidget(inputInfo_);
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
    auto* rateHint = new QLabel(QStringLiteral("cs8 / cs16 文件本身不带采样率：会先看同名 .txt 里的 rate、再看文件名里的“…SPS”，都没有时用这里的值。WAV 用文件头里的采样率。采样率须不低于 3.15 MS/s。"));
    rateHint->setObjectName(QStringLiteral("Muted"));
    rateHint->setWordWrap(true);
    inputLayout->addWidget(rateHint);
    leftLayout->addWidget(inputBox);

    auto* outBox = new QGroupBox(QStringLiteral("2  输出文件夹"));
    auto* outLayout = new QVBoxLayout(outBox);
    auto* outRow = new QHBoxLayout;
    outputEdit_ = new QLineEdit;
    auto* browseOut = new QPushButton(QStringLiteral("浏览…"));
    outRow->addWidget(outputEdit_, 1);
    outRow->addWidget(browseOut);
    outLayout->addLayout(outRow);
    openOutBtn_ = new QPushButton(QStringLiteral("打开输出文件夹"));
    outLayout->addWidget(openOutBtn_);
    leftLayout->addWidget(outBox);

    auto* keyBox = new QGroupBox(QStringLiteral("3  密钥文件（加密的图像段需要）"));
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

    auto* topRow = new QHBoxLayout;
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

    root->addWidget(left);
    root->addWidget(right, 1);
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
    const QString typed = outputEdit_ ? outputEdit_->text().trimmed() : QString();
    if (!typed.isEmpty()) return typed;
    return QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath(QStringLiteral("NadirStation_GK2A-HRIT"));
}

void MainWindow::browseInput() {
    QSettings settings;
    const QString start = settings.value(QStringLiteral("inputDir"), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).toString();
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择基带录音"), start,
                                                      QStringLiteral("基带录音 (*.cs8 *.cs16 *.wav);;所有文件 (*)"));
    if (path.isEmpty()) return;
    settings.setValue(QStringLiteral("inputDir"), QFileInfo(path).absolutePath());
    setInputPath(path);
}

void MainWindow::browseOutput() {
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择输出文件夹"), outputRoot());
    if (!dir.isEmpty()) outputEdit_->setText(QDir::toNativeSeparators(dir));
}

void MainWindow::browseKey() {
    const QString start = keyEdit_->text().isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) : keyEdit_->text();
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择密钥文件"), start,
                                                      QStringLiteral("密钥文件 (*.bin);;所有文件 (*)"));
    if (!path.isEmpty()) keyEdit_->setText(QDir::toNativeSeparators(path));
}

void MainWindow::openOutputFolder() {
    const QString dir = outputRoot();
    QDir().mkpath(dir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void MainWindow::setInputPath(const QString& path) {
    inputEdit_->setText(QDir::toNativeSeparators(path));
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
    const bool isWav = (info.format == hritapp::IqFormat::Wav);
    manualRate_->setEnabled(!isWav);
    if (isWav && manualRate_->isChecked()) {
        QSignalBlocker block(manualRate_);
        manualRate_->setChecked(false);
        rateSpin_->setEnabled(false);
    }
    const bool rateUnknown = (!isWav && info.rateSource == "手动设置" && !preferManual);
    if (rateUnknown && !manualRate_->isChecked()) {
        QSignalBlocker block(manualRate_);
        manualRate_->setChecked(true);
        rateSpin_->setEnabled(true);
    } else if (!rateUnknown && !preferManual) {
        QSignalBlocker block(rateSpin_);
        rateSpin_->setValue(info.sampleRate / 1e6);   // show what was detected
    }
    QString text = QStringLiteral("%1 · %2 MS/s（%3）· %4 秒 · %5")
                       .arg(QString::fromUtf8(hritapp::formatName(info.format)))
                       .arg(info.sampleRate / 1e6, 0, 'f', 3)
                       .arg(QString::fromUtf8(info.rateSource.c_str()))
                       .arg(static_cast<double>(info.frames) / info.sampleRate, 0, 'f', 1)
                       .arg(humanSize(QFileInfo(path).size()));
    if (rateUnknown) text += QStringLiteral("\n文件没有标明采样率，暂用上面的值，请核对后再开始。");
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
            startBtn_->setEnabled(false);
            startBtn_->setText(QStringLiteral("正在停止…"));
            onLog(QStringLiteral("已请求停止，正在保存已收到的内容…"));
        }
        return;
    }
    const QString path = inputEdit_->text().trimmed();
    if (path.isEmpty() || !QFileInfo(path).isFile() || !isSupportedRecording(path)) {
        QMessageBox::information(this, QStringLiteral("还没有选好录音"), QStringLiteral("请先选择一个 .cs8、.cs16 或 .wav 录音文件。"));
        return;
    }
    inspectInput();
    if (!inputValid_) {
        QMessageBox::warning(this, QStringLiteral("录音不能用"), inputInfo_->text());
        return;
    }
    const QString out = outputRoot();
    outputEdit_->setText(QDir::toNativeSeparators(out));
    QSettings settings;
    settings.setValue(QStringLiteral("outputDir"), outputEdit_->text());
    settings.setValue(QStringLiteral("keyFile"), keyEdit_->text());
    settings.setValue(QStringLiteral("manualRateMsps"), rateSpin_->value());

    log_->clear();
    products_->clear();
    previewLabel_->setPixmap(QPixmap());
    previewLabel_->setText(QStringLiteral("等待图像数据…"));
    previewCaption_->clear();
    lastPreview_ = QImage();
    progress_->setValue(0);
    onStats(hritapp::PipelineStats{});
    setRunning(true);
    emit startRequested(path, rateSpin_->value() * 1e6, manualRate_->isChecked(), out, keyEdit_->text().trimmed());
}

void MainWindow::setRunning(bool running) {
    running_ = running;
    stopping_ = false;
    startBtn_->setEnabled(true);
    startBtn_->setText(running ? QStringLiteral("停止") : QStringLiteral("开始解码"));
    inputEdit_->setEnabled(!running);
    outputEdit_->setEnabled(!running);
    keyEdit_->setEnabled(!running);
    manualRate_->setEnabled(!running);
    rateSpin_->setEnabled(!running && manualRate_->isChecked());
    statusBar()->showMessage(running ? QStringLiteral("解码中…") : QStringLiteral("就绪"));
}

void MainWindow::onLog(const QString& text) {
    log_->appendPlainText(QStringLiteral("[%1] %2").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")), text));
}

void MainWindow::onProgress(int permille) {
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
    progress_->setValue(ok ? progress_->value() : 0);
    onLog(summary);
    statusBar()->showMessage(summary.section(QLatin1Char('\n'), 0, 0));
    if (!ok) QMessageBox::warning(this, QStringLiteral("解码没有开始"), summary);
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
    const QString path = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("EULA.txt"));
    if (QFileInfo::exists(path) && QDesktopServices::openUrl(QUrl::fromLocalFile(path))) return;
    QMessageBox::information(this, QStringLiteral("用户协议与免责声明"),
                             QStringLiteral("找不到 EULA.txt。请重新安装本软件，或在官网阅读完整协议。"));
}

void MainWindow::showAbout() {
    QMessageBox::about(this, QStringLiteral("关于"),
        QStringLiteral("<h3>%1</h3>"
                       "<p>版本 %2<br>开发者：%3</p>"
                       "<p>离线解码 GK-2A（千里眼-2A）HRIT 下行的基带录音：QPSK 解调 → Viterbi / Reed-Solomon → LRIT 文件 → 云图与 L2 产品。仅读取 .cs8 / .cs16 / .wav 文件，不控制任何无线电硬件，不联网。</p>"
                       "<p>本软件是业余爱好者的学习研究项目，不是气象业务系统，也与韩国气象厅、国家气象卫星中心无任何隶属或授权关系。使用前请阅读“用户协议与免责声明”。</p>"
                       "<p style='color:#8a919c'>第三方：Qt 6（LGPL v3）、OpenJPEG（BSD-2）、libjpeg-turbo（BSD/IJG/zlib）、Natural Earth（公有领域，国界为中国视角版本）。</p>")
            .arg(QString::fromUtf8(appinfo::kProductName), QString::fromUtf8(appinfo::kVersion), QString::fromUtf8(appinfo::kDeveloper)));
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (running_) {
        const auto answer = QMessageBox::question(this, QStringLiteral("正在解码"),
                                                  QStringLiteral("解码还在进行。退出会先保存已收到的内容，可能需要几十秒。确定退出吗？"));
        if (answer != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        worker_->requestStop();
    }
    QSettings settings;
    settings.setValue(QStringLiteral("outputDir"), outputEdit_->text());
    settings.setValue(QStringLiteral("keyFile"), keyEdit_->text());
    settings.setValue(QStringLiteral("manualRateMsps"), rateSpin_->value());
    event->accept();
}
