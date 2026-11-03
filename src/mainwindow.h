// mainwindow.h - the whole window of the GK-2A HRIT decoder: pick a recording, press start, watch it lock,
// open the images.
#pragma once
#include "decodeworker.h"

#include <QImage>
#include <QMainWindow>
#include <QThread>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class ConstellationWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

signals:
    void startRequested(const QString& inputPath, double manualRateHz, bool preferManual, const QString& outputDir,
                        const QString& keyPath);

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
    QString outputRoot() const;

    // input
    QLineEdit* inputEdit_ = nullptr;
    QLabel* inputInfo_ = nullptr;
    QCheckBox* manualRate_ = nullptr;
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
    QImage lastPreview_;
    bool running_ = false;
    bool stopping_ = false;
    bool inputValid_ = false;
};
