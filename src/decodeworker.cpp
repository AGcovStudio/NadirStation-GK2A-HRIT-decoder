#include "decodeworker.h"

#include "gk2ahritimages.h"
#include "iqreader.h"
#include "qtpath.h"
#include "repair.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QThread>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <regex>
#include <string>

namespace {

// The decryption key is not part of this program: it is issued to registered users by the satellite operator.
// Look for the file the user chose, then next to the program, then next to the recording.
QString findKeyFile(const QString& chosen, const QString& inputPath, const QString& outputDir) {
    QStringList candidates;
    if (!chosen.isEmpty()) candidates << chosen;
    const QDir app(QCoreApplication::applicationDirPath());
    candidates << app.filePath(QStringLiteral("EncryptionKeyMessage.bin"))
               << app.filePath(QStringLiteral("resources/gk2a/EncryptionKeyMessage.bin"))
               << QFileInfo(inputPath).dir().filePath(QStringLiteral("EncryptionKeyMessage.bin"))
               << QDir(outputDir).filePath(QStringLiteral("EncryptionKeyMessage.bin"));
    for (const QString& c : candidates)
        if (QFileInfo(c).isFile()) return QDir::toNativeSeparators(QFileInfo(c).absoluteFilePath());
    return {};
}

// "IMG_FD_143_SW038_20261103_062903_05.hrit" -> "20261103_062903": which observation a segment belongs to.
std::string observationStamp(const std::string& fileName) {
    static const std::regex re("_(\\d{8}_\\d{6})_");
    std::smatch m;
    return std::regex_search(fileName, m, re) ? m[1].str() : std::string();
}

}  // namespace

void DecodeWorker::run(const QString& inputPath, double manualRateHz, bool preferManual, const QString& outputDir,
                       const QString& keyPath) {
    using clock = std::chrono::steady_clock;
    stop_.store(false);

    hritapp::IqReader reader;
    std::string error;
    if (!reader.open(toFsPath(inputPath), manualRateHz, error, preferManual)) {
        emit finished(false, QString::fromUtf8(error.c_str()));
        return;
    }
    const hritapp::IqInfo info = reader.info();
    const double seconds = static_cast<double>(info.frames) / info.sampleRate;
    emit inputInfo(QStringLiteral("%1 · %2 MS/s（%3）· %4 秒")
                       .arg(QString::fromUtf8(hritapp::formatName(info.format)))
                       .arg(info.sampleRate / 1e6, 0, 'f', 3)
                       .arg(QString::fromUtf8(info.rateSource.c_str()))
                       .arg(seconds, 0, 'f', 1),
                   info.sampleRate);

    hritapp::HritPipeline pipe;
    if (!pipe.configure(info.sampleRate, error)) {
        emit finished(false, QString::fromUtf8(error.c_str()));
        return;
    }
    if (!QDir().mkpath(outputDir)) {
        emit finished(false, QStringLiteral("无法创建输出文件夹：%1").arg(outputDir));
        return;
    }

    nadirstation::gk2a::HritImages images;
    const QString key = findKeyFile(keyPath, inputPath, outputDir);
    if (key.isEmpty())
        emit logLine(QStringLiteral("没有找到密钥文件 EncryptionKeyMessage.bin：加密的图像段无法解密。"
                                    "请在左侧“密钥文件”里选择你自己的密钥文件。"));
    else
        emit logLine(QStringLiteral("使用密钥文件：%1").arg(key));   // the path only; key bytes are never logged
    emit logLine(QStringLiteral("开始解码：%1 MS/s，约 %2 秒的数据。").arg(info.sampleRate / 1e6, 0, 'f', 3).arg(seconds, 0, 'f', 1));

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

    // A recording is read much faster than real time, so the image worker (JPEG2000 / JPEG decoding) is the
    // slow stage: wait for it instead of letting segments be dropped.
    // Segments that came out with holes (frames the first pass could not decode) are remembered so they can be
    // rebuilt if the repair pass below finds the missing frames.
    std::map<std::string, std::size_t> damaged;   // file name -> packets lost
    std::string lastStamp;
    pipe.setCollectFrames(true);
    const std::function<void(nadirstation::gk2a::LritFile&&)> onFile = [&](nadirstation::gk2a::LritFile&& file) {
        if (file.bytes.size() > nadirstation::gk2a::LritFileAssembler::kMaxFileBytes) return;
        if (!file.holes.empty() || file.lostPackets > 0) damaged[file.fileName] = std::max<std::size_t>(file.lostPackets, 1);
        lastStamp = observationStamp(file.fileName);
        for (int tries = 0; tries < 400 && !stop_.load(); ++tries) {
            if (images.queuedBytes() > (24u << 20)) {
                QThread::msleep(25);
                continue;
            }
            if (images.submit(std::move(file), outputDir, key)) return;   // on refusal `file` is left untouched
            QThread::msleep(25);
        }
    };

    std::vector<std::complex<float>> block;
    constexpr std::size_t kBlockFrames = 65536;
    auto lastUi = clock::now() - std::chrono::seconds(1);
    auto lastLog = clock::now();
    bool stopped = false;
    while (reader.read(block, kBlockFrames) > 0) {
        if (stop_.load()) {
            stopped = true;
            break;
        }
        const auto& symbols = pipe.process(block, onFile);
        const auto now = clock::now();
        if (now - lastUi >= std::chrono::milliseconds(150)) {
            lastUi = now;
            emit progress(static_cast<int>(1000.0 * static_cast<double>(reader.position()) / static_cast<double>(info.frames)));
            emit statsChanged(pipe.stats());
            const std::size_t n = std::min<std::size_t>(symbols.size(), 2500);
            if (n > 0) emit constellation(std::vector<std::complex<float>>(symbols.end() - static_cast<std::ptrdiff_t>(n), symbols.end()));
            publish();
        }
        if (now - lastLog >= std::chrono::seconds(5)) {
            lastLog = now;
            const auto s = pipe.stats();
            emit logLine(QStringLiteral("RS 通过 %1 / 同步 %2 · 数据包 %3 · 完成文件 %4 · 信噪比 %5 dB · 载波质量 %6%")
                             .arg(s.cadusRsOk).arg(s.cadusWithSync).arg(s.packets).arg(s.filesCompleted)
                             .arg(s.snrDb, 0, 'f', 1).arg(s.lockQuality * 100.0f, 0, 'f', 0));
        }
    }

    pipe.finish(onFile);
    const auto lastStats = pipe.stats();
    emit statsChanged(lastStats);
    emit progress(stopped ? static_cast<int>(1000.0 * static_cast<double>(reader.position()) / static_cast<double>(info.frames)) : 1000);
    // Repair pass: a frame that fails Reed-Solomon in one receiver setting very often passes in another. Go back
    // to the recording around each gap, decode it again with other settings, and rebuild the damaged segments.
    if (!stopped && !damaged.empty()) {
        emit logLine(QStringLiteral("有 %1 个分段因丢帧受损，正在回头重解丢失的帧…").arg(damaged.size()));
        auto frames = std::move(pipe.collectedFrames());
        const auto rep = hritapp::repairFrames(
            reader, frames, [this] { return stop_.load(); },
            [this](const std::string& m) { emit logLine(QString::fromUtf8(m.c_str())); });
        if (rep.framesRecovered > 0) {
            int rebuilt = 0, skipped = 0;
            hritapp::HritPipeline::assembleFrames(frames, [&](nadirstation::gk2a::LritFile&& f) {
                const auto it = damaged.find(f.fileName);
                if (it == damaged.end()) return;                                  // it was fine the first time
                if (f.lostPackets >= it->second && !f.holes.empty()) return;      // nothing gained
                if (observationStamp(f.fileName) != lastStamp) { ++skipped; return; }   // an earlier observation is already closed
                onFile(std::move(f));
                ++rebuilt;
            });
            emit logLine(QStringLiteral("已用找回的帧重新合成 %1 个分段%2。")
                             .arg(rebuilt)
                             .arg(skipped > 0 ? QStringLiteral("（另有 %1 个属于较早的观测，未能重新合成）").arg(skipped) : QString()));
        }
    }
    emit logLine(QStringLiteral("数据读取完毕，正在合成并保存图像（大幅图像可能需要几十秒）…"));
    images.drain();
    images.requestFinalize();
    images.drain();
    publish();

    QString summary = QStringLiteral("%1：RS 通过 %2 / 同步 %3 · 数据包 %4 · 完成 %5 个文件 · 输出 %6 次观测")
                          .arg(stopped ? QStringLiteral("已停止") : QStringLiteral("解码完成"))
                          .arg(lastStats.cadusRsOk).arg(lastStats.cadusWithSync).arg(lastStats.packets).arg(lastStats.filesCompleted)
                          .arg(observations);
    if (lastStats.cadusRsOk == 0)
        summary += QStringLiteral("\n没有解出任何有效帧：请确认采样率、信号强度（GK-2A HRIT 下行 1695.4 MHz，约 4.5 MHz 宽）和录音文件。");
    else if (observations == 0 && !stopped)
        summary += QStringLiteral("\n录音里的数据还不够拼出一幅图（一幅全圆盘约需 10 分钟以上的完整接收），或缺少密钥文件。");
    emit finished(true, summary);
}
