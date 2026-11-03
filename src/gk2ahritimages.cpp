#include "gk2ahritimages.h"
#include "des.h"
#include "gk2al2core.h"
#include <openjpeg.h>
#include <turbojpeg.h>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSaveFile>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QImageReader>
#include <QCoreApplication>
#include <QDateTime>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace nadirstation::gk2a {
namespace {
uint32_t be32(const uint8_t* p){return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3];}
unsigned be16(const uint8_t* p){return (unsigned(p[0])<<8)|p[1];}
struct MemoryStream {const uint8_t* data;size_t length,pos=0;};
OPJ_SIZE_T readJ2k(void* out,OPJ_SIZE_T n,void* user){
    auto& m=*static_cast<MemoryStream*>(user);
    n=std::min(n,m.length-m.pos);if(!n)return OPJ_SIZE_T(-1);
    std::memcpy(out,m.data+m.pos,n);m.pos+=n;return n;
}
OPJ_OFF_T skipJ2k(OPJ_OFF_T n,void* user){
    auto& m=*static_cast<MemoryStream*>(user);
    if(n<0 || uint64_t(n)>m.length-m.pos)return -1;
    m.pos+=size_t(n);return n;
}
OPJ_BOOL seekJ2k(OPJ_OFF_T pos,void* user){
    auto& m=*static_cast<MemoryStream*>(user);
    if(pos<0 || uint64_t(pos)>m.length)return OPJ_FALSE;
    m.pos=size_t(pos);return OPJ_TRUE;
}
void codecError(const char* s,void* user){*static_cast<QString*>(user)=QString::fromUtf8(s).left(250);}
bool saveBytes(const QString& path,const uint8_t* data,size_t size){
    QSaveFile f(path);return f.open(QIODevice::WriteOnly) && f.write(reinterpret_cast<const char*>(data),qint64(size))==qint64(size) && f.commit();
}
bool savePng(const QString& path,const QImage& image){
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly)&&image.save(&file,"PNG")&&file.commit();
}
// ---- GK-2A L2 glue -----------------------------------------------------------------------------------------
// The products themselves are made by gk2al2core.h (no Qt in it). What lives here is only what needs Qt:
// the land mask picture, the border file, the PNG writing and the JSON.

// Equirectangular land/sea picture (white = land) at full size, 8100 x 4050 in the bundled file.
QImage loadLandMask(){
    QImageReader reader(QDir(QCoreApplication::applicationDirPath()).filePath(
        QStringLiteral("resources/maps/landmask.jpg")));
    const QImage world=reader.read();
    return world.isNull()?QImage():world.convertToFormat(QImage::Format_Grayscale8);
}

// Coast and country borders from the bundled Natural Earth file (1:10m, China point-of-view version "_chn"), as (lon, lat) polylines.
std::vector<std::vector<std::pair<float,float>>> loadBorderLines(){
    std::vector<std::vector<std::pair<float,float>>> lines;
    QFile file(QDir(QCoreApplication::applicationDirPath()).filePath(
        QStringLiteral("resources/maps/ne_10m_admin_0_countries_chn.geojson")));
    if(!file.open(QIODevice::ReadOnly))return lines;
    const QJsonDocument doc=QJsonDocument::fromJson(file.readAll());
    const auto addRing=[&lines](const QJsonArray& ring){
        std::vector<std::pair<float,float>> line;line.reserve(size_t(ring.size()));
        for(const auto& v:ring){
            const QJsonArray p=v.toArray();
            if(p.size()>=2)line.emplace_back(float(p.at(0).toDouble()),float(p.at(1).toDouble()));
        }
        if(line.size()>=2)lines.push_back(std::move(line));
    };
    for(const auto& featureValue:doc.object().value(QStringLiteral("features")).toArray()){
        const QJsonObject geometry=featureValue.toObject().value(QStringLiteral("geometry")).toObject();
        const QString type=geometry.value(QStringLiteral("type")).toString();
        const QJsonArray coordinates=geometry.value(QStringLiteral("coordinates")).toArray();
        if(type==QLatin1String("Polygon")){
            for(const auto& ring:coordinates)addRing(ring.toArray());
        }else if(type==QLatin1String("MultiPolygon")){
            for(const auto& polygon:coordinates)for(const auto& ring:polygon.toArray())addRing(ring.toArray());
        }
    }
    return lines;
}

// "KEY = value" from the text navigation record (header type 130).
bool textNumber(const QString& text,const char* key,double& value){
    const QRegularExpression re(QStringLiteral("(?:^|[\\r\\n])\\s*%1\\s*=\\s*([-+0-9.eE]+)").arg(QLatin1String(key)));
    const auto m=re.match(text);
    if(!m.hasMatch())return false;
    bool ok=false;const double v=m.captured(1).toDouble(&ok);
    if(ok)value=v;
    return ok;
}

}
QImage HritImages::decodePreview(const std::vector<uint8_t>& data,int width,int height,int bits,QString& error,int compression,bool native,std::size_t firstHole){
    if(width<=0 || height<=0 || width>11000 || height>1100 || bits<8 || bits>12 ||
       uint64_t(width)*height>12100000 || data.size()>LritFileAssembler::kMaxFileBytes){
        error=QStringLiteral("HRIT image geometry/size exceeds limit");return {};
    }
    if(compression==0){
        const size_t pixels=size_t(width)*height;
        // NMSC's official HRIT sample stores uncompressed 10-bit counts in LE uint16 containers.
        // Also accept the bit-packed, MSB-first representation, based on exact payload length.
        const bool words=data.size()==pixels*2;
        const size_t packed=(pixels*size_t(bits)+7)/8;
        if(!words && data.size()!=packed){error=QStringLiteral("Uncompressed HRIT payload size mismatch");return {};}
        if(native){
            const unsigned top=(1u<<bits)-1;
            QImage full(width,height,QImage::Format_Grayscale16);if(full.isNull())return {};
            for(int y=0;y<height;++y){
                auto* dst=reinterpret_cast<uint16_t*>(full.scanLine(y));
                for(int x=0;x<width;++x){
                    const size_t index=size_t(y)*width+x;unsigned v=0;
                    if(words)v=data[index*2]|(unsigned(data[index*2+1])<<8);
                    else for(int b=0;b<bits;++b){const size_t bit=index*bits+b;v=(v<<1)|((data[bit/8]>>(7-bit%8))&1);}
                    if(v>top){error=QStringLiteral("Uncompressed HRIT counts exceed declared precision");return {};}
                    dst[x]=uint16_t(v*65535u/top);
                }
            }
            return full;
        }
        const int w=std::min(width,1375),h=(height*w+width-1)/width,maxValue=(1<<bits)-1;
        QImage img(w,h,QImage::Format_Grayscale8);if(img.isNull())return {};
        for(int y=0;y<h;++y)for(int x=0;x<w;++x){
            const size_t index=size_t(y*height/h)*width+x*width/w;unsigned v=0;
            if(words)v=data[index*2]|(unsigned(data[index*2+1])<<8);
            else for(int b=0;b<bits;++b){const size_t bit=index*bits+b;v=(v<<1)|((data[bit/8]>>(7-bit%8))&1);}
            if(v>unsigned(maxValue)){error=QStringLiteral("Uncompressed HRIT counts exceed declared precision");return {};}
            img.scanLine(y)[x]=uint8_t(v*255/maxValue);
        }
        return img;
    }
    // Accept the service's small compression envelope (including the documented 85-byte case).
    size_t offset=0; bool jpeg=false, j2k=false;
    for(;offset+4<data.size() && offset<=256;++offset){
        jpeg=data[offset]==0xff && data[offset+1]==0xd8 && data[offset+2]==0xff;
        j2k=data[offset]==0xff && data[offset+1]==0x4f && data[offset+2]==0xff && data[offset+3]==0x51;
        if(jpeg||j2k)break;
    }
    if(!jpeg&&!j2k){error=QStringLiteral("No JPEG/JPEG2000 marker (key or payload invalid)");return {};}
    const int maxValue=(1<<bits)-1;
    if(jpeg){
        std::unique_ptr<void,decltype(&tj3Destroy)> dec(tj3Init(TJINIT_DECOMPRESS),tj3Destroy);
        if(!dec){error=QStringLiteral("JPEG decoder allocation failed");return {};}
        tj3Set(dec.get(),TJPARAM_MAXMEMORY,native?512:64);
        tj3Set(dec.get(),TJPARAM_MAXPIXELS,12100000);
        // A segment with a lost packet is zero-filled there; keep decoding past the damage instead of failing.
        tj3Set(dec.get(),TJPARAM_STOPONWARNING,firstHole<data.size()?0:1);
        if(tj3DecompressHeader(dec.get(),data.data()+offset,data.size()-offset)<0){
            error=QString::fromUtf8(tj3GetErrorStr(dec.get()));return {};
        }
        if(tj3Get(dec.get(),TJPARAM_JPEGWIDTH)!=width || tj3Get(dec.get(),TJPARAM_JPEGHEIGHT)!=height ||
           tj3Get(dec.get(),TJPARAM_COLORSPACE)!=TJCS_GRAY){error=QStringLiteral("JPEG geometry differs from HRIT header");return {};}
        const int precision=tj3Get(dec.get(),TJPARAM_PRECISION);
        if(precision>12 || precision<8){error=QStringLiteral("Unsupported JPEG precision");return {};}
        int divisor=1;
        if(!tj3Get(dec.get(),TJPARAM_LOSSLESS)){
            // libjpeg-turbo's 1/2 scaling of a 275-line IR strip returns 138
            // rows, but its last row is averaged with a nonexistent line and
            // comes out at half brightness. Decode the small IR strip at its
            // native size, then let Qt resample across its actual 275 rows.
            divisor=(native||width<=2750)?1:8;
            if(tj3SetScalingFactor(dec.get(),tjscalingfactor{1,divisor})<0){error=QStringLiteral("JPEG scaling failed");return {};}
        }
        const int w=(width+divisor-1)/divisor,h=(height+divisor-1)/divisor;
        QImage img(w,h,native?QImage::Format_Grayscale16:QImage::Format_Grayscale8); if(img.isNull()){error=QStringLiteral("Image allocation failed");return {};}
        int result=0;
        if(precision<=8){
            if(native){
                std::vector<uint8_t> pixels(size_t(w)*h);
                result=tj3Decompress8(dec.get(),data.data()+offset,data.size()-offset,pixels.data(),w,TJPF_GRAY);
                if(result>=0)for(int y=0;y<h;++y){
                    auto* dst=reinterpret_cast<uint16_t*>(img.scanLine(y));
                    for(int x=0;x<w;++x)dst[x]=uint16_t(pixels[size_t(y)*w+x]*257u);
                }
            }else{
                result=tj3Decompress8(dec.get(),data.data()+offset,data.size()-offset,img.bits(),img.bytesPerLine(),TJPF_GRAY);
            }
        }else{
            std::vector<int16_t> pixels(size_t(w)*h);
            result=tj3Decompress12(dec.get(),data.data()+offset,data.size()-offset,pixels.data(),w,TJPF_GRAY);
            if(result>=0)for(int y=0;y<h;++y){
                if(native){
                    auto* dst=reinterpret_cast<uint16_t*>(img.scanLine(y));
                    for(int x=0;x<w;++x)dst[x]=uint16_t(std::clamp(int(pixels[size_t(y)*w+x])*65535/maxValue,0,65535));
                }else{
                    for(int x=0;x<w;++x)
                        img.scanLine(y)[x]=uint8_t(std::clamp(int(pixels[size_t(y)*w+x])*255/maxValue,0,255));
                }
            }
        }
        if(result<0){error=QString::fromUtf8(tj3GetErrorStr(dec.get()));return {};}
        if(native)return img;
        QImage preview=img.scaledToWidth(1375,Qt::SmoothTransformation);
        if(width==2750 && height==275 && img.width()==2750 && preview.height()==138){
            // Qt's smooth 275→138 resize averages the final source row with
            // black outside the image. Copy the actual source edge instead.
            const auto* src=img.constScanLine(img.height()-1);
            auto* dst=preview.scanLine(preview.height()-1);
            for(int x=0;x<1375;++x)dst[x]=uint8_t((unsigned(src[2*x])+src[2*x+1]+1)/2);
        }
        return preview;
    }
    // Validate SIZ BEFORE OpenJPEG allocates components/tiles, including its own geometry.
    if(data.size()-offset<45){error=QStringLiteral("Truncated JPEG2000 SIZ");return {};}
    const auto* s=data.data()+offset;
    const uint32_t x=be32(s+8),y=be32(s+12),x0=be32(s+16),y0=be32(s+20);
    const uint32_t tw=be32(s+24),th=be32(s+28),tx=be32(s+32),ty=be32(s+36);
    if(x0!=0 || y0!=0 || x!=uint32_t(width) || y!=uint32_t(height) ||
       be16(s+40)!=1 || be16(s+4)!=41 || (s[42]&0x80) || (s[42]&127)+1>12 ||
       s[43]!=1 || s[44]!=1 || tw==0 || th==0 || tx!=0 || ty!=0 || tw>x || th>y ||
       ((uint64_t(x)+tw-1)/tw)*((uint64_t(y)+th-1)/th)>256){
        error=QStringLiteral("JPEG2000 SIZ outside HRIT segment limits");return {};
    }
    MemoryStream mem{data.data()+offset,data.size()-offset};
    std::unique_ptr<opj_stream_t,decltype(&opj_stream_destroy)> stream(opj_stream_create(65536,OPJ_TRUE),opj_stream_destroy);
    std::unique_ptr<opj_codec_t,decltype(&opj_destroy_codec)> codec(opj_create_decompress(OPJ_CODEC_J2K),opj_destroy_codec);
    if(!stream||!codec){error=QStringLiteral("JPEG2000 decoder allocation failed");return {};}
    opj_stream_set_user_data(stream.get(),&mem,nullptr);opj_stream_set_user_data_length(stream.get(),mem.length);
    opj_stream_set_read_function(stream.get(),readJ2k);opj_stream_set_skip_function(stream.get(),skipJ2k);opj_stream_set_seek_function(stream.get(),seekJ2k);
    opj_set_error_handler(codec.get(),codecError,&error);
    opj_dparameters_t params;opj_set_default_decoder_parameters(&params);
    // Reduce reconstruction size; one worker/thread and a single bounded segment at a time.
    params.cp_reduce=0;
    if(!opj_setup_decoder(codec.get(),&params)){error=QStringLiteral("JPEG2000 setup failed");return {};}
    // A segment with zero-filled lost packets is still decoded; only the damaged code-blocks are wrong.
    opj_decoder_set_strict_mode(codec.get(),OPJ_FALSE);
    opj_codec_set_threads(codec.get(),1);
    opj_image_t* raw=nullptr;
    const bool headerOk=opj_read_header(stream.get(),codec.get(),&raw);
    std::unique_ptr<opj_image_t,decltype(&opj_image_destroy)> img(raw,opj_image_destroy);
    if(!headerOk||!img)return {};
    if(img->numcomps!=1 || img->comps[0].w!=unsigned(width) || img->comps[0].h!=unsigned(height))return {};
    opj_codestream_info_v2_t* info=opj_get_cstr_info(codec.get());
    unsigned reduce=0;
    if(info && info->m_default_tile_info.tccp_info) {
        const unsigned levels=info->m_default_tile_info.tccp_info[0].numresolutions;
        if(levels>0&&!native)reduce=std::min(width>2750?3u:1u,levels-1);
    }
    if(info)opj_destroy_cstr_info(&info);
    if(!opj_set_decoded_resolution_factor(codec.get(),reduce))return {};
    if(!opj_decode(codec.get(),stream.get(),img.get()) || !opj_end_decompress(codec.get(),stream.get()))return {};
    const auto& c=img->comps[0];
    if(!c.data)return {};
    if(native){
        if(c.w!=unsigned(width)||c.h!=unsigned(height))return {};
        QImage full(width,height,QImage::Format_Grayscale16);if(full.isNull())return {};
        for(int row=0;row<height;++row){
            auto* dst=reinterpret_cast<uint16_t*>(full.scanLine(row));
            for(int col=0;col<width;++col)
                dst[col]=uint16_t(std::clamp(int64_t(c.data[size_t(row)*c.w+col])*65535/maxValue,int64_t(0),int64_t(65535)));
        }
        return full;
    }
    const int w=std::min(width,1375),h=(height*w+width-1)/width;
    QImage preview(w,h,QImage::Format_Grayscale8);if(preview.isNull())return {};
    for(int row=0;row<h;++row)for(int col=0;col<w;++col){
        const auto value=c.data[size_t(row*c.h/h)*c.w+col*c.w/w];
        preview.scanLine(row)[col]=uint8_t(std::clamp(int64_t(value)*255/maxValue,int64_t(0),int64_t(255)));
    }
    return preview;
}
HritImages::HritImages():worker_([this]{run();}){}
HritImages::~HritImages(){
    {std::lock_guard lock(mutex_);stopping_=true;jobs_.clear();bytes_=0;}
    cv_.notify_all();worker_.join();
}
size_t HritImages::queuedBytes()const{std::lock_guard lock(mutex_);return bytes_;}
bool HritImages::submit(LritFile&& file,const QString& root,const QString& keyPath){
    std::lock_guard lock(mutex_);
    // The five AMI channels can finish their matching segments in one burst.
    // Keep a hard byte cap, but allow all five to wait for the single image worker.
    if(file.bytes.size()>LritFileAssembler::kMaxFileBytes || jobs_.size()>=8 ||
       bytes_+file.bytes.size()>64u*1024u*1024u){
        latest_.message=QStringLiteral("HRIT 图像队列满，分段已丢弃；请检查解码速度/内存上限");return false;
    }
    bytes_+=file.bytes.size();jobs_.push_back({std::move(file),root,keyPath,epoch_});cv_.notify_one();return true;
}
// A reset (link re-acquired, retune, new session) must not throw away an observation that is half received: its close-out is
// queued under the old epoch ahead of everything new, so the worker writes the channels that arrived before it starts over.
// Observations already finished but not yet announced stay in the completed queue.
void HritImages::reset(){
    std::lock_guard lock(mutex_);
    // Segments still waiting belong to the discarded link state; a close-out already waiting is kept (it is for the
    // observation in progress), and only one is ever queued.
    std::deque<Job> keep;
    for(auto& j:jobs_)if(j.finalize)keep.push_back(std::move(j));
    jobs_.swap(keep);bytes_=0;
    if(jobs_.empty()){Job closeOut;closeOut.epoch=epoch_;closeOut.finalize=true;jobs_.push_back(std::move(closeOut));}
    ++epoch_;latest_={};
    cv_.notify_one();
}
void HritImages::drain(){std::unique_lock lock(mutex_);cv_.wait(lock,[&]{return jobs_.empty()&&!busy_;});}
HritImages::Update HritImages::takeUpdate(){std::lock_guard lock(mutex_);auto value=std::move(latest_);latest_={};return value;}
std::vector<HritImages::Update> HritImages::takeCompleted(){
    std::lock_guard lock(mutex_);std::vector<Update> out;out.swap(completed_);return out;
}
void HritImages::pushCompleted(Update&& update){
    std::lock_guard lock(mutex_);
    if(completed_.size()>=8)completed_.erase(completed_.begin());
    completed_.push_back(std::move(update));
}
void HritImages::requestFinalize(){
    std::lock_guard lock(mutex_);
    Job job;job.epoch=epoch_;job.finalize=true;
    jobs_.push_back(std::move(job));cv_.notify_one();
}
void HritImages::run(){
#ifdef _WIN32
    // JPEG decoding is bursty and heavy; it must not take the CPU from the IQ capture and DSP threads.
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
#endif
    for(;;){
        Job job;
        {std::unique_lock lock(mutex_);cv_.wait(lock,[&]{return stopping_||!jobs_.empty();});
            if(stopping_)return;
            job=std::move(jobs_.front());jobs_.pop_front();bytes_-=job.file.bytes.size();busy_=true;
        }
        Update update;
        try{update=process(job);}catch(const std::exception& e){update.message=QStringLiteral("HRIT image rejected: %1").arg(QString::fromUtf8(e.what()));}
        {std::lock_guard lock(mutex_);if(job.finalize&&update.complete){if(completed_.size()>=8)completed_.erase(completed_.begin());completed_.push_back(update);}
        else if(job.epoch==epoch_) {
            if(update.complete){
                if(completed_.size()>=8)completed_.erase(completed_.begin());
                completed_.push_back(update);
            }
            // Latest-only: a newer VIS quicklook replaces the older one that was not yet taken.
            if(!update.live.isNull()){latest_.live=std::move(update.live);latest_.stamp=update.stamp;}
            if(!update.path.isEmpty()){auto live=std::move(latest_.live);latest_=std::move(update);if(latest_.live.isNull())latest_.live=std::move(live);}
            else if(!update.message.isEmpty())latest_.message=std::move(update.message);
        }busy_=false;}
        cv_.notify_all();
    }
}
HritImages::Update HritImages::process(Job& job){
    Update update;auto& f=job.file;
    if(imageEpoch_!=job.epoch){channels_.clear();stamp_.clear();observationDir_.clear();finalized_=false;productsMade_=false;productPaths_.clear();imageEpoch_=job.epoch;}
    // An observation already closed (full disk) is not written a second time when decoding stops.
    if(job.finalize)return finalized_?Update{}:finalizeObservation();
    if(!f.complete||!f.primary.valid||f.dataOffset>f.bytes.size())return update;
    QString name=QFileInfo(QString::fromStdString(f.fileName)).fileName();
    const QRegularExpression re(QStringLiteral("^IMG_FD_[0-9]+_(VI006|SW038|WV069|IR105|IR123)_([0-9]{8}_[0-9]{6})_([0-9]{2})\\.hrit$"),QRegularExpression::CaseInsensitiveOption);
    auto match=re.match(name);
    if(!match.hasMatch())return update;
    const bool vis=match.captured(1).compare(QStringLiteral("VI006"),Qt::CaseInsensitive)==0;
    const QString stamp=match.captured(2);const int segment=match.captured(3).toInt();
    int w=0,h=0,bits=0,count=0,seq=0,compression=-1;
    QByteArray dataFunction;
    int32_t nav2[4]={0,0,0,0};bool haveNav2=false;
    double nav130[4]={0,0,0,0};bool haveNav130=false;
    double mjd=0;bool haveMjd=false;
    for(size_t pos=16;pos+3<=f.dataOffset;){
        const auto len=be16(f.bytes.data()+pos+1);if(len<3||len>f.dataOffset-pos)return update;
        if(f.bytes[pos]==1 && len==9){bits=f.bytes[pos+3];w=int(be16(f.bytes.data()+pos+4));h=int(be16(f.bytes.data()+pos+6));compression=f.bytes[pos+8];}
        if(f.bytes[pos]==128 && len==7){seq=f.bytes[pos+3];count=f.bytes[pos+4];}
        // Calibration table (type 3, plain text), navigation (type 2 binary, type 130 text), time (type 131, MJD).
        if(f.bytes[pos]==3 && len>3)dataFunction=QByteArray(reinterpret_cast<const char*>(f.bytes.data()+pos+3),int(len-3));
        if(f.bytes[pos]==2 && len>=3+32+16){
            const uint8_t* b=f.bytes.data()+pos+3+32;
            for(int k=0;k<4;++k)nav2[k]=int32_t(be32(b+4*k));
            haveNav2=true;
        }
        if(f.bytes[pos]==130 && len>3){
            const QString text=QString::fromLatin1(reinterpret_cast<const char*>(f.bytes.data()+pos+3),int(len-3));
            double c=0,l=0,co=0,lo=0;
            if(textNumber(text,"CFAC",c)&&textNumber(text,"LFAC",l)&&textNumber(text,"COFF",co)&&textNumber(text,"LOFF",lo)){
                nav130[0]=c;nav130[1]=l;nav130[2]=co;nav130[3]=lo;haveNav130=true;
            }
        }
        if(f.bytes[pos]==131 && len>3){
            bool ok=false;
            const double v=QString::fromLatin1(reinterpret_cast<const char*>(f.bytes.data()+pos+3),int(len-3)).trimmed().toDouble(&ok);
            if(ok&&v>40000.0&&v<80000.0){mjd=v;haveMjd=true;}
        }
        pos+=len;
    }
    if(w!=(vis?11000:2750)||h!=(vis?1100:275)||bits!=10||count!=10||seq!=segment||segment<1||segment>10||compression<0||compression>2){
        update.message=QStringLiteral("HRIT segment rejected: inconsistent geometry/sequence");return update;
    }
    const QString dir=QDir(job.root).filePath(QStringLiteral("GK-2A HRIT/")+stamp);
    if(!QDir().mkpath(dir)){update.message=QStringLiteral("HRIT output directory unavailable");return update;}
    std::size_t firstHole=~std::size_t(0);
    for(const auto& hole:f.holes){
        const std::size_t end=std::size_t(hole.first)+hole.second;
        if(end<=f.dataOffset)continue;
        firstHole=std::min(firstHole,hole.first>f.dataOffset?std::size_t(hole.first)-f.dataOffset:std::size_t(0));
    }
    std::vector<uint8_t> payload(f.bytes.begin()+std::ptrdiff_t(f.dataOffset),f.bytes.end());
    std::vector<uint8_t>().swap(f.bytes);
    if(f.encrypted){
        QFile keyFile(job.keyPath);bool found=false;std::array<uint8_t,8> key{};
        if(keyFile.open(QIODevice::ReadOnly)&&keyFile.size()<=2+65535*10){
            auto keys=keyFile.readAll();
            if(keys.size()>=2){const auto* p=reinterpret_cast<const uint8_t*>(keys.constData());
                const unsigned entries=be16(p);
                if(size_t(keys.size())==2+entries*10)for(unsigned i=0;i<entries;++i){
                    if(be16(p+2+10*i)==f.encryptionKey){std::memcpy(key.data(),p+4+10*i,8);found=true;break;}
                }
            }
        }
        if(!found){update.message=QStringLiteral("HRIT encrypted segment saved; automatic key lookup failed for index %1").arg(f.encryptionKey);return update;}
        if(payload.size()%8){update.message=QStringLiteral("HRIT encrypted payload is not DES block aligned");return update;}
        const auto schedule=crypto::desKeySchedule(key.data());
        crypto::desEcbDecrypt(payload.data(),payload.data(),payload.size(),schedule);
    }
    QString error;const QImage strip=decodePreview(payload,w,h,bits,error,compression,true,firstHole);
    const QString channelName=match.captured(1).toUpper();
    if(strip.isNull()){update.message=QStringLiteral("HRIT %1: %2").arg(channelName,error.isEmpty()?QStringLiteral("image decode failed"):error);return update;}
    if(stamp_!=stamp){
        // A new observation has started, so the previous one is as complete as it will get.
        if(!stamp_.isEmpty()&&!finalized_&&!channels_.empty())pushCompleted(finalizeObservation());
        stamp_=stamp;channels_.clear();observationDir_.clear();finalized_=false;productsMade_=false;productPaths_.clear();
    }
    auto& channel=channels_[channelName];
    if(!dataFunction.isEmpty())channel.table=dataFunction;
    if(haveNav130){for(int k=0;k<4;++k)channel.nav[k]=nav130[k];channel.navSource=2;}
    else if(haveNav2&&channel.navSource<1){for(int k=0;k<4;++k)channel.nav[k]=double(nav2[k]);channel.navSource=1;}
    if(haveMjd){channel.mjd=mjd;}
    if(strip.format()!=QImage::Format_Grayscale16||strip.width()!=w||strip.height()!=h)return update;
    // Original-resolution channel image, 16-bit (10-bit counts scaled to the full range). Only the
    // segments that arrive are filled in; it is written once, when the observation is closed.
    if(channel.full.isNull()){
        channel.full=QImage(w,h*10,QImage::Format_Grayscale16);
        if(!channel.full.isNull())channel.full.fill(0);
    }
    if(channel.full.isNull()){update.message=QStringLiteral("HRIT %1: not enough memory for the full-resolution image").arg(channelName);return update;}
    for(int row=0;row<h;++row)
        std::memcpy(channel.full.scanLine((segment-1)*h+row),strip.constScanLine(row),size_t(w)*2);
    // A small 8-bit canvas, kept in memory only: the L2 products are made from it. It is never written out.
    if(channel.preview.isNull()){
        channel.preview=QImage(1375,1375,QImage::Format_Grayscale8);
        if(!channel.preview.isNull())channel.preview.fill(0);
    }
    if(channel.preview.isNull()){update.message=QStringLiteral("HRIT quicklook allocation failed");return update;}
    {
        const int y0=(segment-1)*1375/10,y1=segment*1375/10,dstHeight=y1-y0;
        for(int row=0;row<dstHeight;++row){
            const int sy0=int(int64_t(row)*h/dstHeight),sy1=std::max(sy0+1,int(int64_t(row+1)*h/dstHeight));
            auto* dst=channel.preview.scanLine(y0+row);
            for(int x=0;x<1375;++x){
                const int sx0=int(int64_t(x)*w/1375),sx1=std::max(sx0+1,int(int64_t(x+1)*w/1375));
                uint64_t sum=0;
                for(int sy=sy0;sy<sy1;++sy){
                    const auto* p=reinterpret_cast<const uint16_t*>(strip.constScanLine(sy));
                    for(int sx=sx0;sx<sx1;++sx)sum+=p[sx];
                }
                dst[x]=uint8_t(std::min<uint64_t>(255,(sum/uint64_t((sy1-sy0)*(sx1-sx0)))>>8));
            }
        }
    }
    channel.segments.insert(segment);
    update.stamp=stamp;
    if(vis){
        // Live view of the visible channel: the rows received so far (up to the last segment seen),
        // stretched so a dark disk is still readable. A deep copy, so the canvas can keep changing.
        const int last=*channel.segments.rbegin();
        const int rows=std::min(1375,std::max(1,last*1375/10));
        QImage live=channel.preview.copy(0,0,1375,rows);
        if(!live.isNull()){
            uint32_t hist[256]={};uint64_t seen=0;
            for(int y=0;y<rows;++y){const auto* p=live.constScanLine(y);for(int x=0;x<1375;++x){++hist[p[x]];}}
            for(int v=1;v<256;++v)seen+=hist[v];   // value 0 is the unreceived/space background
            int lo=0,hi=255;
            if(seen>1000){
                uint64_t acc=0;const uint64_t a=seen/200,b=seen-seen/200;
                lo=255;hi=1;bool gotLo=false;
                for(int v=1;v<256;++v){acc+=hist[v];if(!gotLo&&acc>=a){lo=v;gotLo=true;}if(acc>=b){hi=v;break;}}
                if(hi<=lo+8){lo=std::max(0,hi-64);hi=std::min(255,lo+64);}
                uint8_t lut[256];
                for(int v=0;v<256;++v)lut[v]=v==0?0:uint8_t(std::clamp((v-lo)*255/std::max(1,hi-lo),0,255));
                for(int y=0;y<rows;++y){auto* p=live.scanLine(y);for(int x=0;x<1375;++x)p[x]=lut[p[x]];}
            }
            update.live=std::move(live);
        }
    }
    observationDir_=QFileInfo(dir).absoluteFilePath();
    // A completed channel is still only one fifth of a full AMI observation.
    // Keep it in the live view; publish a finished session after all five channels exist.
    update.complete=false;
    const auto get=[this](const char* name)->Channel*{
        auto it=channels_.find(QString::fromLatin1(name));return it==channels_.end()?nullptr:&it->second;
    };
    if(!finalized_){
        const auto* vi=get("VI006");const auto* sw=get("SW038");
        const auto* wv=get("WV069");const auto* ir=get("IR105");const auto* split=get("IR123");
        if(vi&&sw&&wv&&ir&&split&&vi->segments.size()==10&&sw->segments.size()==10&&
           wv->segments.size()==10&&ir->segments.size()==10&&split->segments.size()==10){
            // Full disk: close it now rather than waiting for the next observation.
            Update done=finalizeObservation();
            done.message=QStringLiteral("GK-2A HRIT · %1 · 五通道完整，已生成通道图、L2/RGB 产品和 JSON").arg(stamp);
            return done;
        }
    }
    update.message=QStringLiteral("GK-2A HRIT · %1 · 已接收 %2/10 段").arg(channelName).arg(channel.segments.size());
    return update;
}

// Builds the calibrated L2 and RGB products at the original IR grid (2750 x 2750; VIS 11000 x 11000 is
// averaged 4 x 4 onto it), writes the PNGs and GK2A_Products_Metadata.json, returns the PNG paths.
// Missing channels or segments do not stop it: what cannot be made is listed under "skipped".
QStringList HritImages::makeL2Products(const Channel* vi,const Channel* sw,const Channel* wv,
                                       const Channel* ir,const Channel* sp,QJsonObject& meta){
    QStringList paths;
    l2::Inputs in;
    const auto fill=[](l2::Channel& dst,const char* name,const Channel* src){
        dst.name=name;
        if(!src||src->full.isNull())return;
        dst.width=src->full.width();dst.height=src->full.height();
        dst.counts=reinterpret_cast<const uint16_t*>(src->full.constScanLine(0));
        dst.stride=size_t(src->full.bytesPerLine())/2;
        for(int s:src->segments)if(s>=1&&s<=10)dst.segment[size_t(s-1)]=true;
        dst.table=l2::parseTable(std::string(src->table.constData(),size_t(src->table.size())));
    };
    fill(in.vi,"VI006",vi);fill(in.sw,"SW038",sw);fill(in.wv,"WV069",wv);fill(in.ir,"IR105",ir);fill(in.sp,"IR123",sp);
    // Navigation: the text record first, then the binary one, then the broadcast constants.
    if(ir->navSource>0){
        in.nav.cfac=ir->nav[0];in.nav.lfac=ir->nav[1];in.nav.coff=ir->nav[2];in.nav.loff=ir->nav[3];in.nav.fromFile=true;
    }
    // Observation time: the MJD of the image header, else the time stamp of the file names.
    QDateTime when;
    for(const Channel* c:{ir,sp,wv,sw,vi})
        if(c&&c->mjd>0){when=QDateTime::fromSecsSinceEpoch(qint64(std::llround((c->mjd-40587.0)*86400.0))).toUTC();break;}
    const bool timeFromHeader=when.isValid();
    if(!when.isValid()){
        when=QDateTime::fromString(stamp_,QStringLiteral("yyyyMMdd_HHmmss"));
        when.setTimeSpec(Qt::UTC);
    }
    in.utc=when.isValid()?std::time_t(when.toSecsSinceEpoch()):std::time_t(0);
    in.timeText=when.isValid()?when.toString(QStringLiteral("yyyy-MM-dd HH:mm")).toStdString()+" UTC":stamp_.toStdString();
    // Land/sea and borders.
    const QImage landMask=loadLandMask();
    if(!landMask.isNull()){
        in.land=[landMask](double lat,double lon)->int{
            const int mh=landMask.height(),mw=landMask.width();
            const int y=std::clamp(int((90.0-lat)/180.0*mh),0,mh-1);
            double wrapped=std::fmod(lon+180.0,360.0);if(wrapped<0)wrapped+=360.0;
            const int x=std::clamp(int(wrapped/360.0*mw),0,mw-1);
            return landMask.constScanLine(y)[x]>127?1:0;
        };
    }
    in.lines=loadBorderLines();
    in.overlay=!in.lines.empty();
    l2::Result result=l2::makeProducts(in);
    for(auto& product:result.products){
        const l2::Rgb& rgb=product.image;
        if(rgb.w<=0||rgb.h<=0)continue;
        const QImage image(rgb.px.data(),rgb.w,rgb.h,rgb.w*3,QImage::Format_RGB888);
        const QString path=QDir(observationDir_).filePath(QString::fromStdString(product.file));
        if(savePng(path,image))paths.append(path);
        std::vector<uint8_t>().swap(product.image.px);   // free the picture as soon as it is on disk
    }
    QJsonArray productArray,skipped,notes,calibration;
    for(const auto& product:result.products)
        productArray.append(QJsonObject{{QStringLiteral("file"),QString::fromStdString(product.file)},
                                        {QStringLiteral("title"),QString::fromStdString(product.title)},
                                        {QStringLiteral("note"),QString::fromStdString(product.note)}});
    for(const auto& s:result.skipped)skipped.append(QString::fromStdString(s));
    for(const auto& s:result.notes)notes.append(QString::fromStdString(s));
    for(const l2::Channel* c:{&in.vi,&in.sw,&in.wv,&in.ir,&in.sp})
        calibration.append(QJsonObject{{QStringLiteral("channel"),QString::fromLatin1(c->name)},
            {QStringLiteral("received"),c->present()},
            {QStringLiteral("table_points"),int(c->table.points.size())},
            {QStringLiteral("table_name"),QString::fromStdString(c->table.name)},
            {QStringLiteral("table_unit"),QString::fromStdString(c->table.unit)},
            {QStringLiteral("usable"),c->table.usable()}});
    meta=QJsonObject{
        {QStringLiteral("source"),QStringLiteral("GK-2A HRIT AMI, five channels: VI006, SW038, WV069, IR105, IR123")},
        {QStringLiteral("status"),QStringLiteral("calibrated threshold and band-difference products; NOT the operational KMA L2 (that needs 16 channels and numerical weather prediction fields)")},
        {QStringLiteral("calibration"),QStringLiteral("count-to-value tables of the HRIT image data function record (brightness temperature in K, albedo in %); a channel without a usable table is not drawn from a guess")},
        {QStringLiteral("resolution"),QStringLiteral("IR grid 2750 x 2750; VI006 averaged 4 x 4 from 11000 x 11000")},
        {QStringLiteral("navigation"),QJsonObject{{QStringLiteral("from_file"),in.nav.fromFile},{QStringLiteral("cfac"),in.nav.cfac},
            {QStringLiteral("lfac"),in.nav.lfac},{QStringLiteral("coff"),in.nav.coff},{QStringLiteral("loff"),in.nav.loff},
            {QStringLiteral("sub_longitude_deg"),in.nav.subLonDeg}}},
        {QStringLiteral("observation_time_utc"),in.timeText.empty()?QString():QString::fromStdString(in.timeText)},
        {QStringLiteral("observation_time_from_header"),timeFromHeader},
        {QStringLiteral("land_mask"),QStringLiteral("bundled Natural Earth land mask (equirectangular), through the geostationary navigation")},
        {QStringLiteral("ir105_bt_k"),QJsonObject{{QStringLiteral("median"),result.irMedianK},{QStringLiteral("min"),result.irMinK},{QStringLiteral("max"),result.irMaxK}}},
        {QStringLiteral("disk_pixels"),double(result.diskPixels)},
        {QStringLiteral("calibration_tables"),calibration},
        {QStringLiteral("products"),productArray},
        {QStringLiteral("skipped"),skipped},
        {QStringLiteral("notes"),notes}};
    const auto json=QJsonDocument(meta).toJson();
    saveBytes(QDir(observationDir_).filePath(QStringLiteral("GK2A_Products_Metadata.json")),
              reinterpret_cast<const uint8_t*>(json.constData()),size_t(json.size()));
    return paths;
}

// Writes the observation as it stands: one image per channel received, the L2 products when all
// five channels are present, and GK2A_Observation.json listing both with the segment counts, so a
// partial reception is imported honestly instead of not at all.
HritImages::Update HritImages::finalizeObservation(){
    Update update;
    if(stamp_.isEmpty()||observationDir_.isEmpty()||channels_.empty())return update;
    finalized_=true;
    static const std::array<std::pair<const char*,double>,5> kChannels{{
        {"VI006",0.64},{"SW038",3.83},{"WV069",6.95},{"IR105",10.35},{"IR123",12.36}}};
    QStringList channelPaths;
    QJsonArray channelArray;
    int segmentsTotal=0;
    // A segment that never arrived (or could not be decoded) is filled by interpolating between the
    // nearest received segments above and below, so one lost segment is a soft band, not a black one.
    // Only gaps with data on both sides are filled; the segments_received count stays honest.
    const auto conceal=[](Channel& c)->QJsonArray{
        QJsonArray filled;
        if(c.segments.size()<2||c.segments.size()>=10)return filled;
        for(int s=1;s<=10;++s){
            if(c.segments.count(s))continue;
            int a=s-1,b=s+1;
            while(a>=1&&!c.segments.count(a))--a;
            while(b<=10&&!c.segments.count(b))++b;
            if(a<1||b>10)continue;
            const auto blend=[&](QImage& img,bool wide){
                if(img.isNull())return;
                const int total=img.height();
                const int y0=int(int64_t(s-1)*total/10),y1=int(int64_t(s)*total/10);
                const int ya=int(int64_t(a)*total/10)-1;          // last row of the segment above
                const int yb=int(int64_t(b-1)*total/10);          // first row of the segment below
                if(ya<0||yb>=total||yb<=ya)return;
                const int w=img.width();
                for(int y=y0;y<y1;++y){
                    const double t=double(y-ya)/double(yb-ya);
                    if(wide){
                        const auto* pa=reinterpret_cast<const uint16_t*>(img.constScanLine(ya));
                        const auto* pb=reinterpret_cast<const uint16_t*>(img.constScanLine(yb));
                        auto* d=reinterpret_cast<uint16_t*>(img.scanLine(y));
                        for(int x=0;x<w;++x)d[x]=uint16_t(pa[x]+(double(pb[x])-pa[x])*t+0.5);
                    }else{
                        const auto* pa=img.constScanLine(ya);const auto* pb=img.constScanLine(yb);
                        auto* d=img.scanLine(y);
                        for(int x=0;x<w;++x)d[x]=uint8_t(pa[x]+(double(pb[x])-pa[x])*t+0.5);
                    }
                }
            };
            blend(c.full,true);
            blend(c.preview,false);
            filled.append(s);
        }
        return filled;
    };
    for(const auto& [name,wavelength]:kChannels){
        const auto it=channels_.find(QString::fromLatin1(name));
        QJsonObject entry{{QStringLiteral("name"),QString::fromLatin1(name)},
                          {QStringLiteral("wavelength_um"),wavelength},
                          {QStringLiteral("segments_expected"),10}};
        if(it==channels_.end()||it->second.preview.isNull()){
            entry.insert(QStringLiteral("segments_received"),0);
            channelArray.append(entry);continue;
        }
        const QJsonArray concealed=conceal(it->second);
        const QString file=QStringLiteral("GK2A_HRIT_%1.png").arg(QString::fromLatin1(name));
        const QString path=QDir(observationDir_).filePath(file);
        {
            const QImage& image=it->second.full.isNull()?it->second.preview:it->second.full;
            if(savePng(path,image))channelPaths.append(path);
            entry.insert(QStringLiteral("width"),image.width());
            entry.insert(QStringLiteral("height"),image.height());
        }
        QJsonArray received;for(int s:it->second.segments)received.append(s);
        entry.insert(QStringLiteral("file"),file);
        entry.insert(QStringLiteral("segments_received"),int(it->second.segments.size()));
        entry.insert(QStringLiteral("segments"),received);
        if(!concealed.isEmpty())entry.insert(QStringLiteral("segments_concealed"),concealed);
        segmentsTotal+=int(it->second.segments.size());
        channelArray.append(entry);
    }
    const auto get=[this](const char* name)->Channel*{
        auto it=channels_.find(QString::fromLatin1(name));return it==channels_.end()?nullptr:&it->second;
    };
    const auto* vi=get("VI006");const auto* sw=get("SW038");
    const auto* wv=get("WV069");const auto* ir=get("IR105");const auto* split=get("IR123");
    QJsonObject l2Meta;
    if(ir&&!ir->full.isNull()){
        productPaths_=makeL2Products(vi,sw,wv,ir,split,l2Meta);
        productsMade_=true;
    }
    // The calibration text of each channel as received, for checking the tables by eye.
    for(const auto& [name,wavelength]:kChannels){
        const auto* c=get(name);(void)wavelength;
        if(!c||c->table.isEmpty())continue;
        saveBytes(QDir(observationDir_).filePath(QStringLiteral("GK2A_Calibration_%1.txt").arg(QString::fromLatin1(name))),
                  reinterpret_cast<const uint8_t*>(c->table.constData()),size_t(c->table.size()));
    }
    QJsonArray products,l2;
    for(const auto& path:channelPaths)products.append(QJsonObject{
        {QStringLiteral("path"),QFileInfo(path).fileName()},{QStringLiteral("type"),QStringLiteral("png")},
        {QStringLiteral("kind"),QStringLiteral("channel")}});
    for(const auto& path:productPaths_){
        products.append(QJsonObject{
            {QStringLiteral("path"),QFileInfo(path).fileName()},{QStringLiteral("type"),QStringLiteral("png")},
            {QStringLiteral("kind"),QStringLiteral("l2")}});
        l2.append(QFileInfo(path).fileName());
    }
    // The full-resolution images are on disk now; release them (VI006 alone is 11000 x 11000 x 2 bytes).
    for(auto& entry:channels_)entry.second.full=QImage();
    const bool complete=segmentsTotal==50;
    (void)l2Meta;
    const QJsonObject observation{
        {QStringLiteral("schema"),QStringLiteral("NadirStation.GK2A.HRIT.Observation.v2")},
        {QStringLiteral("observation_timestamp"),stamp_},
        {QStringLiteral("satellite"),QStringLiteral("GK-2A")},
        {QStringLiteral("service"),QStringLiteral("HRIT 1695.4 MHz")},
        {QStringLiteral("complete"),complete},
        {QStringLiteral("segments_received"),segmentsTotal},
        {QStringLiteral("segments_expected"),50},
        {QStringLiteral("channels"),channelArray},
        {QStringLiteral("products"),products},
        {QStringLiteral("l2_products"),l2},
        {QStringLiteral("l2_note"),productPaths_.isEmpty()
            ?QStringLiteral("not generated: needs the IR 10.5 channel with its calibration table")
            :QStringLiteral("calibrated threshold and band-difference products from the five HRIT channels, not the operational KMA L2; see GK2A_Products_Metadata.json")}};
    const auto manifest=QJsonDocument(observation).toJson(QJsonDocument::Indented);
    saveBytes(QDir(observationDir_).filePath(QStringLiteral("GK2A_Observation.json")),
              reinterpret_cast<const uint8_t*>(manifest.constData()),size_t(manifest.size()));
    update.stamp=stamp_;
    update.paths=channelPaths;update.paths.append(productPaths_);
    update.path=update.paths.isEmpty()?QString():update.paths.first();
    update.complete=!update.paths.isEmpty();
    update.message=QStringLiteral("GK-2A HRIT · %1 · 已收尾：%2 个通道（%3/50 段）%4，JSON 已写入")
        .arg(stamp_).arg(channelPaths.size()).arg(segmentsTotal)
        .arg(productPaths_.isEmpty()?QStringLiteral("，L2 未生成（需要带标定表的 IR 10.5）"):QStringLiteral("，L2/RGB %1 幅").arg(productPaths_.size()));
    return update;
}
}
