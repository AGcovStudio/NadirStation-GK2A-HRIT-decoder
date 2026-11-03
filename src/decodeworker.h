// decodeworker.h - runs one decode of a recording on its own thread and reports to the window.
#pragma once
#include "pipeline.h"

#include <QImage>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>
#include <atomic>
#include <complex>
#include <vector>

Q_DECLARE_METATYPE(std::vector<std::complex<float>>)
Q_DECLARE_METATYPE(hritapp::PipelineStats)

class DecodeWorker : public QObject {
    Q_OBJECT
public:
    explicit DecodeWorker(QObject* parent = nullptr) : QObject(parent) {}
    // Thread-safe; the run slot notices it within one block and still writes out what has been received.
    void requestStop() { stop_.store(true); }

public slots:
    void run(const QString& inputPath, double manualRateHz, bool preferManual, const QString& outputDir,
             const QString& keyPath);

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
