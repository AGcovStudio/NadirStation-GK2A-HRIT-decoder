// GK-2A LRIT data link layer. See gk2adatalink.h for the field layouts and their sources.

#include "gk2adatalink.h"
#include "gk2adecoder.h"   // the randomiser sequence, shared with the frame layer

#include <algorithm>
#include <array>
#include <cstring>

namespace nadirstation::gk2a {
namespace {

// CRC-16/CCITT as the LRIT mission specification defines it: polynomial 0x1021, initial value
// 0xFFFF, no reflection, no final XOR.
std::uint16_t crcTableEntry(int i) {
    std::uint16_t crc = static_cast<std::uint16_t>(i << 8);
    for (int b = 0; b < 8; ++b)
        crc = static_cast<std::uint16_t>((crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1));
    return crc;
}

const std::array<std::uint16_t, 256>& crcTable() {
    static const std::array<std::uint16_t, 256> table = [] {
        std::array<std::uint16_t, 256> t{};
        for (int i = 0; i < 256; ++i) t[static_cast<std::size_t>(i)] = crcTableEntry(i);
        return t;
    }();
    return table;
}

}  // namespace

std::uint16_t lritCrc(const std::uint8_t* data, std::size_t size) {
    std::uint16_t crc = 0xFFFF;
    const std::array<std::uint16_t, 256>& t = crcTable();
    for (std::size_t i = 0; i < size; ++i)
        crc = static_cast<std::uint16_t>((crc << 8)
                                         ^ t[((crc >> 8) ^ data[i]) & 0xFF]);
    return crc;
}

Vcdu parseVcdu(const std::vector<std::uint8_t>& cadu) {
    Vcdu v;
    if (cadu.size() < 10) return v;
    v.version = static_cast<std::uint8_t>(cadu[4] >> 6);
    v.spacecraftId = static_cast<std::uint16_t>(((cadu[4] & 0x3Fu) << 2) | (cadu[5] >> 6));
    v.vcid = static_cast<std::uint8_t>(cadu[5] & 0x3Fu);
    v.counter = (static_cast<std::uint32_t>(cadu[6]) << 16)
                | (static_cast<std::uint32_t>(cadu[7]) << 8)
                | static_cast<std::uint32_t>(cadu[8]);
    v.replay = (cadu[9] >> 7) != 0;
    return v;
}

PacketHeader parsePacketHeader(const std::uint8_t* raw) {
    PacketHeader h;
    h.version = static_cast<std::uint8_t>((raw[0] >> 5) & 0x07);
    h.type = ((raw[0] >> 4) & 1) != 0;
    h.secondaryHeaderFlag = ((raw[0] >> 3) & 1) != 0;
    h.apid = static_cast<std::uint16_t>(((raw[0] & 0x07) << 8) | raw[1]);
    h.sequenceFlag = static_cast<std::uint8_t>((raw[2] >> 6) & 0x03);
    h.sequenceCount = static_cast<std::uint16_t>(((raw[2] & 0x3F) << 8) | raw[3]);
    h.packetLength = static_cast<std::uint16_t>((raw[4] << 8) | raw[5]);
    return h;
}

LritPrimaryHeader parseLritPrimaryHeader(const std::vector<uint8_t>& bytes, std::size_t* dataOffset,
                                         std::uint32_t* encryptionKey, bool* encrypted) {
    LritPrimaryHeader p;
    if (dataOffset != nullptr) *dataOffset = 0;
    if (encryptionKey != nullptr) *encryptionKey = 0;
    if (encrypted != nullptr) *encrypted = false;
    if (bytes.size() < 16) return p;

    // Primary header, 16 bytes, in the order they are transmitted:
    //   byte 0       type, always 0 for a primary header
    //   bytes 1-2    record length of this header record
    //   byte 3       file type code -- what the file IS, which is how text and additional data
    //                are told apart from imagery
    //   bytes 4-7    total header length, 32-bit: the offset where the data field begins
    //   bytes 8-15   data field length, 64-bit
    //
    // An earlier version read the type from byte 0, the header length from bytes 1-2 and the data
    // length from bytes 5-12. All three were wrong, and the files recovered from the recording
    // came out with types 229 and 40 -- values that are not defined for LRIT at all.
    // A primary header is exactly this. The specification's Table 4.3 and the header layout: byte 0
    // is the record type and is zero for a primary header, bytes 1-2 are this record's length and are
    // sixteen, byte 3 is the file type, bytes 4-7 the total header length, bytes 8-15 the data field
    // length. Anything else is not a file, whatever it is.
    if (bytes.size() < 16 || bytes[0] != 0
        || ((static_cast<std::uint16_t>(bytes[1]) << 8) | static_cast<std::uint16_t>(bytes[2])) != 16) {
        p.valid = false;
        return p;
    }

    p.fileTypeCode = bytes[3];
    p.totalHeaderLength = (static_cast<std::uint32_t>(bytes[4]) << 24)
                          | (static_cast<std::uint32_t>(bytes[5]) << 16)
                          | (static_cast<std::uint32_t>(bytes[6]) << 8)
                          | static_cast<std::uint32_t>(bytes[7]);
    p.dataLength = 0;
    for (int i = 8; i <= 15; ++i)
        p.dataLength = (p.dataLength << 8)
                       | static_cast<std::uint64_t>(bytes[static_cast<std::size_t>(i)]);

    // Walk the header records. Each begins with a type byte and a two-byte record length that
    // includes those three bytes. The walk starts at byte zero, because the primary header is
    // itself the first record -- type 0, length 16 -- and the secondary records follow it.
    //
    // A real file reads, from zero:
    //     0  type 0   primary            len   16
    //    16  type 1   image structure    len    9   count  2200
    //    25  type 2   image navigation   len   51   count 17743
    //    76  type 3   annotation         len 3757   count 16716
    //
    // The count field is not a multiplier: it reports how many records of that type the service
    // carries, not how many are packed here, so the next record is `len` bytes on. An earlier
    // version started at byte 3 and stepped by len * count, which walked into the middle of the
    // timestamp record and read every field after it from the wrong offset -- the key header among
    // them.
    const std::size_t end = std::min(bytes.size(), static_cast<std::size_t>(p.totalHeaderLength));
    std::size_t pos = 0;
    while (pos + 3 <= end) {
        const std::uint8_t type = bytes[pos];
        const std::uint16_t len = static_cast<std::uint16_t>((bytes[pos + 1] << 8) | bytes[pos + 2]);
        if (len < 3 || pos + len > end) break;
        if (type == 7 && pos + 7 <= end && encryptionKey != nullptr) {
            // GK-2A key header: type 7, record length, then a four-byte key index. A zero index
            // means the file is not encrypted; any other value selects an entry in the key file.
            *encryptionKey = (static_cast<std::uint32_t>(bytes[pos + 3]) << 24)
                             | (static_cast<std::uint32_t>(bytes[pos + 4]) << 16)
                             | (static_cast<std::uint32_t>(bytes[pos + 5]) << 8)
                             | static_cast<std::uint32_t>(bytes[pos + 6]);
            if (encrypted != nullptr) *encrypted = (*encryptionKey != 0);
        }
        pos += len;
    }
    // The announced header length has to be possible. Requiring the record walk to consume the
    // header exactly was tried and is too strict: the secondary records of a real file do not follow
    // the type-and-length layout the walk assumes -- measured on the data-link test's own file, the
    // walk stops after the primary header and reaches 16 of the 58 bytes announced. The walk has
    // always been that way and the offset was set from the announced length regardless, which is why
    // decoding worked; a check that depends on the walk would reject files that are fine.
    //
    // What is required is what the specification requires and what a frame that is not a file
    // cannot satisfy: the two tests above, and a header length that fits inside the bytes received.
    if (p.totalHeaderLength < 16 || p.totalHeaderLength > bytes.size()) {
        p.valid = false;
        return p;
    }
    p.valid = true;

    if (dataOffset != nullptr)
        *dataOffset = std::min(bytes.size(), static_cast<std::size_t>(p.totalHeaderLength));
    return p;
}

void LritFileAssembler::reset() {
    lastFrame_.clear(); limitDrops=0;
    wip_.clear();
    vcState_.clear();
    cadusFed = fillerCadus = packetsSeen = crcFailures = frameDiscontinuities = 0;
}

std::vector<LritFile> LritFileAssembler::feed(const std::vector<std::uint8_t>& cadu) {
    std::vector<LritFile> out;
    if (cadu.size() < 1024) return out;
    ++cadusFed;

    const Vcdu vcdu = parseVcdu(cadu);
    if (vcdu.vcid == 63) {      // filler, per the specification
        ++fillerCadus;
        return out;
    }

    // The M_PDU header is the two bytes after the six-byte VCDU header, and the packet data
    // follows it. Reading data from byte 10 instead, as an earlier version did, means the two
    // pointer bytes are consumed as packet content and every packet is misframed from the start.
    constexpr int kMpduHeaderOffset = 10;
    constexpr int kMpduDataOffset = 12;
    constexpr int kMpduDataSize = 884;
    if (cadu.size() < static_cast<std::size_t>(kMpduDataOffset + kMpduDataSize)) return out;

    const std::uint8_t* data = cadu.data() + kMpduDataOffset;
    const int firstHeaderPointer =
        ((cadu[static_cast<std::size_t>(kMpduHeaderOffset)] & 0x07) << 8)
        | cadu[static_cast<std::size_t>(kMpduHeaderOffset + 1)];

    // A pointer that claims to be inside the data but beyond its end cannot be trusted, and the
    // reference discards such a frame rather than guessing.
    if (firstHeaderPointer < 2047 && firstHeaderPointer >= kMpduDataSize) return out;

    if(strict_) {
        auto last=lastFrame_.find(vcdu.vcid);
        if(last!=lastFrame_.end() && vcdu.counter!=((last->second+1)&0xffffff)) {
            ++frameDiscontinuities;
            vcState_.erase(vcdu.vcid);
            // Loss-tolerant mode keeps the files in progress: the packet sequence numbers reveal
            // what is missing and it is zero-filled, instead of discarding a 25 MB segment.
            if(!lossTolerant_) {
                for(auto it=wip_.begin();it!=wip_.end();) {
                    if(it->first.first==vcdu.vcid) it=wip_.erase(it); else ++it;
                }
            }
        }
        lastFrame_[vcdu.vcid]=vcdu.counter;
    }
    VcState& st = vcState_[vcdu.vcid];

    // 1. A header split across the last boundary is completed from the front of this frame.
    int offset = 0;
    if (st.inHeader) {
        st.inHeader = false;
        const int need = 6 - st.inHeaderBuffer;
        for (int i = 0; i < need; ++i) st.headerBuffer[st.inHeaderBuffer + i] = data[i];
        offset = need;
        st.inHeaderBuffer += need;
        beginPacket(st, st.headerBuffer);
        ++packetsSeen;
    }

    // 2. An open packet takes bytes up to the new header, or the whole frame when there is none.
    if (st.remainingPayload > 0 && st.working) {
        int toWrite;
        if (firstHeaderPointer < 2047)
            toWrite = std::min(firstHeaderPointer - offset, st.remainingPayload);
        else
            toWrite = std::min(kMpduDataSize - offset, st.remainingPayload);
        if (toWrite > 0) appendPayload(st, data + offset, toWrite);
        if (firstHeaderPointer < 2047) st.remainingPayload = 0;
    }

    // 3. A packet whose payload is complete is finished here.
    if (st.remainingPayload == 0 && st.working) finishPacket(st, vcdu, out);

    // 4. Walk this frame's own headers.
    if (firstHeaderPointer < 2047) {
        if (firstHeaderPointer + 6 < kMpduDataSize) {
            beginPacket(st, data + firstHeaderPointer);
            ++packetsSeen;
            const bool hasSecondHeader =
                kMpduDataSize > firstHeaderPointer + static_cast<int>(st.totalLength);
            if (hasSecondHeader) {
                const int firstEnd = firstHeaderPointer + static_cast<int>(st.totalLength);
                if (firstEnd <= kMpduDataSize) {
                    const int take = std::min(kMpduDataSize - (firstHeaderPointer + 6),
                                              static_cast<int>(st.payloadLength));
                    if (take > 0) appendPayload(st, data + firstHeaderPointer + 6, take);
                    if (st.remainingPayload == 0) finishPacket(st, vcdu, out);
                } else {
                    abortPacket(st);
                }
                // Then every further packet that starts inside this frame.
                int next = firstHeaderPointer + static_cast<int>(st.totalLength);
                while (next < kMpduDataSize) {
                    if (next + 6 < kMpduDataSize) {
                        beginPacket(st, data + next);
                        ++packetsSeen;
                        const int take = std::min(kMpduDataSize - (next + 6),
                                                  st.remainingPayload);
                        if (take > 0) appendPayload(st, data + next + 6, take);
                    } else if (next < kMpduDataSize) {
                        // Only part of the header is here; the rest arrives next frame.
                        st.inHeader = true;
                        st.inHeaderBuffer = 0;
                        const int have = kMpduDataSize - next;
                        for (int i = 0; i < have; ++i) st.headerBuffer[i] = data[next + i];
                        st.inHeaderBuffer = have;
                        break;
                    }
                    if (st.remainingPayload == 0 && st.working) finishPacket(st, vcdu, out);
                    next += static_cast<int>(st.totalLength);
                    if (st.totalLength == 0) break;     // guard against a zero-length header
                }
            } else if (st.working) {
                // No room for another header, so this packet continues into the next frame.
                const int take = std::min(kMpduDataSize - (firstHeaderPointer + 6),
                                          st.remainingPayload);
                if (take > 0) appendPayload(st, data + firstHeaderPointer + 6, take);
            }
        } else if (firstHeaderPointer < kMpduDataSize) {
            st.inHeader = true;
            st.inHeaderBuffer = 0;
            const int have = kMpduDataSize - firstHeaderPointer;
            for (int i = 0; i < have; ++i) st.headerBuffer[i] = data[firstHeaderPointer + i];
            st.inHeaderBuffer = have;
        }
    }

    return out;
}

void LritFileAssembler::beginPacket(VcState& st, const std::uint8_t* header) {
    for (int i = 0; i < 6; ++i) st.rawHeader[i] = header[i];
    st.header = parsePacketHeader(header);
    // packet_length counts everything after the length field, so the payload is one more than it.
    st.payloadLength = static_cast<int>(st.header.packetLength) + 1;
    st.remainingPayload = st.payloadLength;
    st.totalLength = 6 + st.payloadLength;
    st.payload.clear();
    if (st.payloadLength > 0)
        st.payload.reserve(static_cast<std::size_t>(st.payloadLength));
    st.working = true;
}

void LritFileAssembler::appendPayload(VcState& st, const std::uint8_t* bytes, int count) {
    if (count <= 0 || st.remainingPayload <= 0) return;
    if (count > st.remainingPayload) count = st.remainingPayload;
    st.payload.insert(st.payload.end(), bytes, bytes + count);
    st.remainingPayload -= count;
}

void LritFileAssembler::finishPacket(VcState& st, const Vcdu& vcdu, std::vector<LritFile>& out) {
    // The file layer parses a packet as a six-byte header followed by its payload, so the two are
    // put back together here. A packet whose payload is shorter than announced is still handed on:
    // the layer above checks its CRC and will reject it, which is more informative than dropping it
    // silently.
    std::vector<std::uint8_t> packet;
    packet.reserve(6 + st.payload.size());
    packet.insert(packet.end(), st.rawHeader, st.rawHeader + 6);
    packet.insert(packet.end(), st.payload.begin(), st.payload.end());
    handlePacket(vcdu, packet, out);

    st.payload.clear();
    st.working = false;
    st.remainingPayload = 0;
}

void LritFileAssembler::abortPacket(VcState& st) {
    // A frame announced a packet longer than the space before the next header. The reference drops
    // it; keeping the partial payload would splice two unrelated packets together.
    st.payload.clear();
    st.working = false;
    st.remainingPayload = 0;
    st.payloadLength = 0;
    st.totalLength = 0;
}

std::size_t LritFileAssembler::bufferedBytes() const {
    std::size_t n=0;
    for(const auto& item:wip_) n+=item.second.lritData.capacity()+item.second.file.bytes.capacity();
    for(const auto& item:vcState_) n+=item.second.payload.capacity();
    return n;
}

void LritFileAssembler::handlePacket(const Vcdu& vcdu, const std::vector<std::uint8_t>& packet,
                                     std::vector<LritFile>& out) {
    if (packet.size() < 8) return;
    const PacketHeader h = parsePacketHeader(packet.data());
    if (h.apid == 2047) return;             // filler

    const std::vector<uint8_t> payload(packet.begin() + 6, packet.end());
    if (payload.size() < 2) return;

    // Every packet ends with a two-byte CRC over the rest of its payload.
    const std::uint16_t claimed = static_cast<std::uint16_t>(
        (payload[payload.size() - 2] << 8) | payload[payload.size() - 1]);
    const std::uint16_t computed = lritCrc(payload.data(), payload.size() - 2);
    const bool crcOk = (claimed == computed);
    if (!crcOk) {
        ++crcFailures;
        auto found=wip_.find({vcdu.vcid,h.apid});
        if(found!=wip_.end()) ++found->second.file.crcFailures;
    }

    const auto key=std::make_pair(int(vcdu.vcid),int(h.apid));
    const bool starts=(h.sequenceFlag==1 || h.sequenceFlag==3);
    if(strict_ && !crcOk) {
        // A damaged continuation packet: keep the file and put zeros where it belonged. A damaged
        // closing packet is still used to close the file (its tail may be wrong, the length is
        // forced to the announced one later).
        auto it=lossTolerant_ && !starts ? wip_.find(key) : wip_.end();
        if(it!=wip_.end() && it->second.inProgress && it->second.bodySize>0) {
            Wip& lw=it->second;
            ++lw.file.lostPackets; ++packetsZeroFilled;
            if(h.sequenceFlag==0) {
                lw.file.holes.emplace_back(std::uint32_t(lw.lritData.size()),std::uint32_t(lw.bodySize));
                lw.lritData.resize(lw.lritData.size()+lw.bodySize,0);
                lw.sequence=static_cast<uint16_t>((lw.sequence+1)&0x3fff);
                return;
            }
            // flag 2: fall through and close the file with what this packet carries
        } else { wip_.erase(key); return; }
    }
    if(!wip_.contains(key) && !starts) return;
    if(!wip_.contains(key) && wip_.size()>=32) { ++limitDrops; return; }
    Wip& w = wip_[key];
    if(strict_ && !starts && w.inProgress && h.sequenceCount!=((w.sequence+1)&0x3fff)) {
        const unsigned gap=(h.sequenceCount-w.sequence-1u)&0x3fffu;
        // A gap can be as long as a whole segment: the limit is the largest file, not a few hundred packets.
        if(lossTolerant_ && w.bodySize>0 && gap>0 && w.lritData.size()+std::size_t(gap)*w.bodySize<=kMaxFileBytes) {
            w.file.holes.emplace_back(std::uint32_t(w.lritData.size()),std::uint32_t(std::size_t(gap)*w.bodySize));
            w.lritData.resize(w.lritData.size()+std::size_t(gap)*w.bodySize,0);
            w.file.lostPackets+=gap; packetsZeroFilled+=gap;
        } else { wip_.erase(key); return; }
    }
    w.sequence=h.sequenceCount;
    const auto required=(starts?0:w.lritData.size())+payload.size();
    if(required>kMaxFileBytes) {wip_.erase(key);++limitDrops;return;}
    if(required>w.lritData.capacity()) {
        const auto capacity=std::min(kMaxFileBytes,std::max(required,w.lritData.capacity()*2));
        if(bufferedBytes()-w.lritData.capacity()+capacity>kMaxBufferedBytes) {
            wip_.erase(key);++limitDrops;return;
        }
        w.lritData.reserve(capacity);
    }
    const std::size_t bodyEnd = payload.size() - 2;
    if(crcOk) w.bodySize=bodyEnd;

    // Sequence flags: 0 continues a group, 1 starts one, 2 ends one, 3 is a whole group in a
    // single packet. Flag 3 therefore both starts and ends, and it is handled as the two steps it
    // is rather than left to a later condition. An earlier version started the file on flag 3 but
    // only emitted it from inside the early-header-parse branch, so a standalone packet -- which
    // is what a small file arrives as -- was started and never finished.
    const bool startsFile = (h.sequenceFlag == 1 || h.sequenceFlag == 3);
    const bool endsFile = (h.sequenceFlag == 2 || h.sequenceFlag == 3);

    if (startsFile && strict_ && lossTolerant_) {
        // A virtual channel carries one file at a time, so a new file starting means every other file still
        // open on it has lost its closing packet(s). Close them from their announced length now.
        for(auto& item:wip_) {
            if(item.first==key || item.first.first!=vcdu.vcid || !item.second.inProgress) continue;
            item.second.file.closedByLength = true;
            closeFile(item.first, item.second, out);
        }
    }
    if (startsFile) {
        // Start of a file. If one was already open on this APID, close it out first: that happens
        // when the receiver missed the packet that ended it.
        if (w.inProgress && !strict_) {
            w.file.complete = false;
            w.file.bytes = w.lritData;
            out.push_back(w.file);
        } else if (w.inProgress && lossTolerant_) {
            // The packet that closed the previous file never arrived. What it carried is the tail of the
            // image, so the file is closed from its announced length (zero-filled where data is missing)
            // rather than thrown away with everything that did arrive.
            w.file.closedByLength = true;
            closeFile(key, w, out);
        }
        w.lritData.clear();
        w.file = LritFile{};
        w.file.apid = h.apid;
        w.file.vcid = vcdu.vcid;
        w.inProgress = true;
        w.headerParsed = false;
        // The first packet of a file carries a ten-byte prefix ahead of the LRIT data.
        const std::size_t from = std::min<std::size_t>(10, bodyEnd);
        w.lritData.insert(w.lritData.end(),
                          payload.begin() + static_cast<std::ptrdiff_t>(from),
                          payload.begin() + static_cast<std::ptrdiff_t>(bodyEnd));
    } else if (h.sequenceFlag == 0) {
        if (w.inProgress)
            w.lritData.insert(w.lritData.end(), payload.begin(),
                              payload.begin() + static_cast<std::ptrdiff_t>(bodyEnd));
    } else if (w.inProgress) {
        // Anything that is not a start contributes its payload from the first byte: a continuation
        // packet and the packet that ends the file alike. Restricting this to flag 0, as an
        // earlier version did, meant the last packet of every file matched no branch at all and
        // its payload was dropped -- the file was emitted one packet short, the JPEG truncated,
        // and a bright line drawn where the missing rows should have been.
        w.lritData.insert(w.lritData.end(), payload.begin(),
                          payload.begin() + static_cast<std::ptrdiff_t>(bodyEnd));
    }

    if (!w.inProgress) return;

    // A continuation packet that takes the file to or past its announced length means the closing packet
    // was lost (or never flagged): close the file here instead of letting the next file run into it.
    if (strict_ && lossTolerant_ && !endsFile && !startsFile && w.headerParsed && w.file.primary.valid) {
        const auto declaredNow=(w.file.primary.dataLength/8)+(w.file.primary.dataLength%8!=0);
        if(declaredNow<=kMaxFileBytes && w.lritData.size()>=w.file.dataOffset+declaredNow) {
            w.file.closedByLength = true;
            closeFile(key, w, out);
            return;
        }
    }

    // Parse the headers as soon as enough bytes have arrived, so a caller can see the file name and
    // the image geometry before the whole file lands. The header length is a 32-bit field at bytes
    // 4 to 7; an earlier version read two bytes at 1 and 2, which is the length of the primary
    // record rather than of the whole header.
    if (!w.headerParsed && w.lritData.size() >= 8) {
        const std::uint32_t totalHdr = (static_cast<std::uint32_t>(w.lritData[4]) << 24)
                                       | (static_cast<std::uint32_t>(w.lritData[5]) << 16)
                                       | (static_cast<std::uint32_t>(w.lritData[6]) << 8)
                                       | static_cast<std::uint32_t>(w.lritData[7]);
        if (totalHdr >= 16 && w.lritData.size() >= totalHdr) {
            parseHeaders(w);
            w.headerParsed = true;
        }
    }

    // The file is finished. Its bytes are taken now rather than when the header was first read:
    // parseHeaders copies the accumulated data into the file, and it is called early so that a
    // caller can see the name and geometry before the whole file lands. That copy is a snapshot,
    // and on a file spanning many packets it holds only what had arrived at the time -- which is
    // how a 47556-byte segment came out as 8180, one packet's worth plus the header.
    //
    // Re-parsing here costs a pass over the header records and removes the question of which
    // fields are current and which are stale.
    if (endsFile) closeFile(key, w, out);
}

void LritFileAssembler::closeFile(const std::pair<int, int>& key, Wip& w, std::vector<LritFile>& out) {
    if (!w.inProgress) return;
    w.headerParsed = false;
    parseHeaders(w);
    // An assembled frame whose sixteen bytes are not a primary header is not a file. It passed
    // Reed-Solomon, so it is real data from the carrier, but it is not LRIT -- and a file with a
    // type read from a byte that is not a type code was being routed to the additional-data
    // folder, where a live reception filled it with unnamed 867-byte files that were not from
    // that service at all.
    if (!w.file.primary.valid) {
        // Counted rather than logged: this layer has no Qt and no logger by design -- it is the
        // protocol, and a frame that is not a file is a protocol fact, not an application one.
        // The caller reports the count.
        ++invalidFrames;
        w.inProgress = false;
        std::vector<uint8_t>().swap(w.lritData);
        return;
    }
    const auto declared=(w.file.primary.dataLength/8)+(w.file.primary.dataLength%8!=0);
    const std::size_t need=w.file.dataOffset+declared;
    if(strict_ && lossTolerant_ && declared<=kMaxFileBytes && need<=kMaxFileBytes) {
        // The announced length is the truth about the file; what arrived is made to match it, and the
        // places where it does not are recorded so the image layer knows which data to distrust.
        const std::size_t have=w.lritData.size();
        if(have>need) {
            // Bytes after the announced end are padding (the last packet filled to its full size) or the
            // start of something that ran into this file; neither belongs to it.
            if(have-need>7) ++filesRescued;
            w.lritData.resize(need);
        } else if(have<need) {
            // The tail never arrived (the closing packets were lost). Keep what there is.
            const std::size_t miss=need-have;
            const std::size_t per=w.bodySize>0?w.bodySize:miss;
            const std::size_t lost=(miss+per-1)/per;
            w.file.holes.emplace_back(std::uint32_t(have),std::uint32_t(miss));
            w.file.lostPackets+=lost; packetsZeroFilled+=lost;
            w.lritData.resize(need,0);
            ++filesRescued;
        }
    }
    if(strict_ && (declared>kMaxFileBytes || need>w.lritData.size() || w.lritData.size()>need+7)) {
        wip_.erase(key);++invalidFrames;return;
    }
    w.file.complete = true;
    w.file.bytes=std::move(w.lritData);
    out.push_back(std::move(w.file));
    w.inProgress = false;
    std::vector<uint8_t>().swap(w.lritData);
}

std::vector<LritFile> LritFileAssembler::flush() {
    std::vector<LritFile> out;
    if(!(strict_ && lossTolerant_)) return out;
    for(auto& item:wip_) {
        if(!item.second.inProgress) continue;
        item.second.file.closedByLength = true;
        closeFile(item.first, item.second, out);
    }
    return out;
}

void LritFileAssembler::parseHeaders(Wip& w) {
    // Metadata parsing never duplicates the segment payload.
    w.file.primary = parseLritPrimaryHeader(w.lritData, &w.file.dataOffset,
                                            &w.file.encryptionKey, &w.file.encrypted);

    // The file name, when the downlink carries one, is an ASCII annotation inside the header
    // records. Its offset is not fixed across services, so the header area is scanned for a
    // printable run that looks like a file name rather than read from a guessed position.
    const std::size_t scanEnd = std::min(w.lritData.size(), w.file.dataOffset);
    std::string best;
    std::string run;
    const auto looksLikeName = [](const std::string& s) {
        return s.size() > 4 && (s.find(".jpg") != std::string::npos
                                || s.find(".JPG") != std::string::npos
                                || s.find(".j2k") != std::string::npos
                                || s.find(".J2K") != std::string::npos
                                || s.find(".lrit") != std::string::npos
                                || s.find(".hrit") != std::string::npos);
    };
    // A name never begins with punctuation, and the byte before one is printable often enough that
    // the run kept it: a real reception gave "+ADD_RWW3M_032_20261006_173730_00.lrit", and a name
    // that does not begin with ADD_ has no category, so a sea-information product was filed under
    // ADD/TEXT/ instead of ADD/RWW3M/. Cutting back to the first letter or digit costs nothing and
    // fixes the category, the folder and what is displayed together.
    const auto fileNameFromRun = [](std::string s) {
        std::size_t start = 0;
        while (start < s.size()
               && !((s[start] >= 'A' && s[start] <= 'Z') || (s[start] >= 'a' && s[start] <= 'z')
                    || (s[start] >= '0' && s[start] <= '9'))) {
            ++start;
        }
        return s.substr(start);
    };

    for (std::size_t i = 0; i < scanEnd; ++i) {
        const char c = static_cast<char>(w.lritData[i]);
        if (c >= 0x20 && c < 0x7F) {
            run.push_back(c);
        } else {
            if (looksLikeName(run) && run.size() > best.size()) best = fileNameFromRun(run);
            run.clear();
        }
    }
    if (looksLikeName(run) && run.size() > best.size()) best = fileNameFromRun(run);
    w.file.fileName = best;
}

}  // namespace nadirstation::gk2a
