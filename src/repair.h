// repair.h - second chance for frames the first pass of a recording could not decode.
// No Qt in here.
//
// A recording is decoded once, front to back. A frame whose four Reed-Solomon codewords could not be
// corrected is simply missing from that pass, and the file it belongs to gets a hole. The frames carry
// a counter, so the holes are known exactly: this module finds them, goes back to the recording, and
// decodes the stretch around each gap again with other loop bandwidths, filter shapes and soft-decision
// scalings. A frame that fails in one setting very often comes through in another (the noise is the
// same, but the receiver's decisions, and therefore its mistakes, are not). Only frames that pass the
// Reed-Solomon check are taken, and only for counters that are really missing.
#pragma once
#include "iqreader.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hritapp {

struct RepairReport {
    std::size_t gaps = 0;             // runs of missing frame counters (1..8 frames each) found in the first pass
    std::size_t framesMissing = 0;    // frames missing in those runs
    std::size_t framesRecovered = 0;
    std::size_t decodes = 0;          // short re-decodes performed
    bool budgetExceeded = false;
};

// `frames`: every Reed-Solomon-checked CADU of the first pass, in the order they were received.
// Recovered frames are inserted at their place. `reader` is the already opened recording.
RepairReport repairFrames(IqReader& reader, std::vector<std::vector<std::uint8_t>>& frames,
                          const std::function<bool()>& stopRequested,
                          const std::function<void(const std::string&)>& log,
                          double budgetSeconds = 240.0);

}  // namespace hritapp
