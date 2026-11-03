// GK-2A LRIT data link layer: reassemble LRIT files from validated CADUs.
//
// This sits above gk2adecoder.{h,cpp}, which ends at a checked CADU. Everything here follows
// the operator's specification and the CCSDS packet and LRIT conventions.
//
//   VCDU, from the specification's Table 8.5
//     bytes 0-3  sync marker, already checked below
//     byte  4    version (2 bits) and the top 4 bits of the spacecraft id
//     byte  5    the low 2 bits of the spacecraft id, then the 6-bit virtual channel id
//     bytes 6-8  virtual channel frame counter
//     byte  9    replay flag in the top bit
//     bytes 10..893   884 bytes of M_PDU data
//     bytes 894..1021 128 bytes of Reed-Solomon check symbols
//
//   CCSDS space packet
//     bits 0-2 version, bit 3 type, bit 4 secondary-header flag, bits 5-15 APID
//     byte 2   sequence flag (2 bits) and the top 6 bits of the sequence count
//     byte 3   the low 8 bits of the sequence count
//     bytes 4-5 packet length minus one
//
//   Reassembly
//     virtual channel 63 is filler and is dropped
//     one file per APID
//     sequence flag 1 or 3 starts a file, 0 continues it, 2 ends it
//     the first packet of a file carries a 10-byte prefix before the LRIT data
//     each packet ends with a 2-byte CRC, which is checked
//
// What this layer produces is the LRIT file as the downlink carries it: a 16-byte primary header,
// then the secondary headers, then the compressed image. It does NOT decrypt and does NOT
// decompress; the file it emits is the payload with the check bytes already accounted for.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <utility>

namespace nadirstation::gk2a {

// A VCDU as parsed from a CADU.
struct Vcdu {
    std::uint8_t version = 0;
    std::uint16_t spacecraftId = 0;
    std::uint8_t vcid = 0;
    std::uint32_t counter = 0;
    bool replay = false;
};

// The header of a CCSDS space packet.
struct PacketHeader {
    std::uint8_t version = 0;
    bool type = false;
    bool secondaryHeaderFlag = false;
    std::uint16_t apid = 0;
    std::uint8_t sequenceFlag = 0;
    std::uint16_t sequenceCount = 0;
    std::uint16_t packetLength = 0;
};

// The primary header every LRIT file starts with.
//
// The specification's session layer defines a 16-byte primary header; the fields below are the
// ones this decoder reads.
struct LritPrimaryHeader {
    std::uint8_t fileTypeCode = 0;      // 0 = image, 128 = alpha, 129 = text, ...
    std::uint32_t totalHeaderLength = 0;
    std::uint64_t dataLength = 0;
    // False when the sixteen bytes are not a primary header. A frame that passes Reed-Solomon but
    // carries something other than an LRIT file would otherwise be read as a file whose type is
    // whatever byte three happens to be, and a type that is not imagery is routed to the
    // additional-data folder -- which is how that folder filled with files that were not from the
    // additional-data service at all.
    bool valid = false;
};

// An LRIT file as reassembled from the downlink.
struct LritFile {
    std::uint16_t apid = 0;
    std::uint8_t vcid = 0;
    std::string fileName;
    LritPrimaryHeader primary;
    // The raw file as transmitted: primary header, secondary headers, then the payload. This is
    // the byte string the layers above operate on, so it is handed over unmodified.
    std::vector<uint8_t> bytes;
    // Where the image data begins inside `bytes`, i.e. after the announced header length. The
    // payload from here on is the compressed image, still encrypted if the key header says so.
    std::size_t dataOffset = 0;
    // The key index from the GK-2A key header (type 7), or 0 when the file is not encrypted.
    std::uint32_t encryptionKey = 0;
    bool encrypted = false;
    bool complete = false;
    std::size_t crcFailures = 0;
    // Packets that were missing or failed their CRC and were replaced by zeros (loss-tolerant mode).
    std::size_t lostPackets = 0;
    // Where those zeros are: [offset, length) pairs into `bytes`, in ascending order. The image layer uses
    // them to tell damaged data from good data instead of trusting the whole segment or throwing it away.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> holes;
    // True when the closing packet never arrived and the file was closed from its announced length.
    bool closedByLength = false;
};

// Reassembles LRIT files from CADUs.
class LritFileAssembler {
public:
    // Feed one validated CADU. Returns any files completed by this call, so a caller can stream.
    std::vector<LritFile> feed(const std::vector<std::uint8_t>& cadu);

    // The reception ended: close every file still open from its announced length (loss-tolerant mode
    // only), so a segment whose closing packet was the last thing lost is not thrown away.
    std::vector<LritFile> flush();

    // Counters for reporting.
    std::size_t cadusFed = 0;
    std::size_t fillerCadus = 0;
    std::size_t packetsSeen = 0;
    std::size_t crcFailures = 0;
    std::size_t frameDiscontinuities = 0;
    // Assembled frames whose sixteen bytes are not a primary header. Reed-Solomon passed, so the
    // frame is real data from the carrier, but it is not LRIT -- and reading a file type out of a
    // byte that is not a type code is how files that were not additional data reached the
    // additional-data folder.
    std::uint64_t invalidFrames = 0;

    void reset();
    void setStrict(bool value) { strict_ = value; }
    // With strict on, a lost or corrupt packet normally discards the whole file. Loss-tolerant mode
    // keeps the file and zero-fills the hole (HRIT segments are 25+ MB, so one lost CADU in 300 would
    // otherwise mean no segment ever completes). Whole-file consumers must cope with damaged data.
    void setLossTolerant(bool value) { lossTolerant_ = value; }
    std::size_t packetsZeroFilled = 0;
    // Files that would have been discarded before and were kept: closed without their last packet,
    // padded to the announced length, or trimmed of padding.
    std::size_t filesRescued = 0;
    std::size_t bufferedBytes() const;
    std::size_t limitDrops = 0;
    static constexpr std::size_t kMaxFileBytes = 32u * 1024u * 1024u;
    static constexpr std::size_t kMaxBufferedBytes = 64u * 1024u * 1024u;

private:
    struct Wip {
        LritFile file;
        bool inProgress = false;
        bool headerParsed = false;
        uint16_t sequence = 0;
        std::size_t bodySize = 0;   // payload bytes (without CRC) of the last good packet
        std::vector<uint8_t> lritData;
    };
    // One file in progress per (vcid, apid).
    std::map<std::pair<int, int>, Wip> wip_;

    // Per-virtual-channel packet reassembly state. A packet can begin in one CADU and end in the
    // next, and a header can itself be split, so the state carries the open packet, how many
    // payload bytes it still expects, and any partial header.
    struct VcState {
        bool working = false;
        bool inHeader = false;
        std::uint8_t headerBuffer[6] = {0};
        int inHeaderBuffer = 0;
        PacketHeader header;
        // The six header bytes of the open packet, kept because the file layer wants the header
        // and the payload together. headerBuffer cannot serve: it holds a header that arrived
        // split across a frame boundary and is overwritten before the packet is finished.
        std::uint8_t rawHeader[6] = {0};
        std::vector<std::uint8_t> payload;
        int payloadLength = 0;      // total payload the header announced
        int remainingPayload = 0;   // how much of it is still outstanding
        int totalLength = 0;        // header plus payload, used to step to the next packet
    };
    std::map<int, VcState> vcState_;
    bool strict_ = false;
    bool lossTolerant_ = false;
    std::map<int, uint32_t> lastFrame_;

    // The steps of the reassembly, kept separate because each is called from more than one place.
    void beginPacket(VcState& st, const std::uint8_t* header);
    void appendPayload(VcState& st, const std::uint8_t* bytes, int count);
    void finishPacket(VcState& st, const Vcdu& vcdu, std::vector<LritFile>& out);
    void abortPacket(VcState& st);

    void handlePacket(const Vcdu& vcdu, const std::vector<std::uint8_t>& packet,
                      std::vector<LritFile>& out);
    void parseHeaders(Wip& w);
    // Close the file in `w` (it has all the packets it is going to get) and hand it on if it is sound.
    void closeFile(const std::pair<int, int>& key, Wip& w, std::vector<LritFile>& out);
};

// Parse the 16-byte primary header and locate the image data within a reassembled file.
//
// Split out so it can be tested on a byte string without building a whole CADU stream.
LritPrimaryHeader parseLritPrimaryHeader(const std::vector<uint8_t>& bytes, std::size_t* dataOffset,
                                         std::uint32_t* encryptionKey, bool* encrypted);

// Parse the VCDU header out of a CADU.
Vcdu parseVcdu(const std::vector<std::uint8_t>& cadu);

// Parse a CCSDS packet header from six bytes.
PacketHeader parsePacketHeader(const std::uint8_t* raw);

// The 16-bit CRC the LRIT packets carry, as defined by the LRIT mission specification.
std::uint16_t lritCrc(const std::uint8_t* data, std::size_t size);

}  // namespace nadirstation::gk2a
