#pragma once
#include "gk2adatalink.h"
#include <QImage>
#include <QString>
#include <QByteArray>
#include <QJsonObject>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <set>
#include <map>
#include <array>
#include <QStringList>

namespace nadirstation::gk2a {
// One image worker; bounded handoff and latest-only preview publication.
// Never queues QImages onto the GUI thread or allocates an 11000-square RGB disk.
class HritImages {
public:
    struct Update { QString stamp, path, message; QStringList paths; bool complete=false;
        QImage live; };   // 8-bit live VIS quicklook (received strip only), memory only, never written out
    HritImages();
    ~HritImages();
    bool submit(LritFile&& file,const QString& root,const QString& keyPath);
    Update takeUpdate();
    // Close the observation in progress: write the five channel images, the L2 products and
    // GK2A_Observation.json from whatever has arrived, and queue it as completed. Called when a
    // reception ends or decoding is stopped; a new observation time stamp does it implicitly, and
    // a full disk (all 5 x 10 segments) does it as soon as the last segment lands.
    void requestFinalize();
    // Observations finished since the last call, oldest first. Separate from takeUpdate(), which
    // is latest-only for the live preview and would otherwise let a later strip overwrite them.
    std::vector<Update> takeCompleted();
    void reset();
    void drain();
    size_t queuedBytes() const;
    static QImage decodePreview(const std::vector<uint8_t>& bytes,int width,int height,
                                int bits,QString& error,int compression=1,bool native=false,
                                std::size_t firstHole=~std::size_t(0));
private:
    struct Job {LritFile file;QString root,keyPath;uint64_t epoch;bool finalize=false;};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    size_t bytes_=0;
    uint64_t epoch_=0;
    bool stopping_=false,busy_=false;
    Update latest_;
    std::vector<Update> completed_;
    std::thread worker_;
    struct Channel { QImage preview; QImage full; std::set<int> segments; QString path;
        QByteArray table;              // image data function record (count-to-value text)
        double nav[4]={0,0,0,0};       // CFAC, LFAC, COFF, LOFF
        int navSource=0;               // 0 none, 1 binary record, 2 text record
        double mjd=0; };               // observation time from the header
    std::map<QString,Channel> channels_;
    QString stamp_;
    QString observationDir_;
    QString observationKeyPath_;
    bool finalized_=false;
    bool productsMade_=false;
    QStringList productPaths_;
    uint64_t imageEpoch_=~uint64_t(0);
    void run();
    Update process(Job& job);
    Update finalizeObservation();
    QStringList makeL2Products(const Channel* vi,const Channel* sw,const Channel* wv,
                               const Channel* ir,const Channel* sp,QJsonObject& meta);
    void pushCompleted(Update&& update);
};
}
