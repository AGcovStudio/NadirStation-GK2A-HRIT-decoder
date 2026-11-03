// DES, as FIPS PUB 46-2 defines it, in the one mode this service uses: ECB decryption of
// independent eight-byte blocks.
//
// The GK-2A LRIT payload is DES-ECB encrypted with a key selected by the file's type 7 key header,
// and the operator publishes the keys. A general-purpose DES belongs in a library, and this is not
// one: it offers no other mode, no encryption direction beyond what the key schedule needs, and no
// constant-time guarantees. It exists because the payload cannot be read without it and the
// project has no crypto dependency.
//
// The tables are the standard's. They are written with one-based bit numbers exactly as the
// standard prints them, and the permutation helper subtracts one; writing them zero-based by hand
// is how an off-by-one gets in, and a DES with one wrong entry in one S-box still produces output
// that looks like noise, so the fault would present as a wrong key rather than a wrong table.

#pragma once

#include <array>
#include <cstdint>

namespace nadirstation::crypto {

// One eight-byte block, decrypted in place into `out`. `key` is the eight key bytes as they appear
// in the key file; the caller passes them in the file's order and no byte swapping is applied,
// because the reference implementation takes the key with memcpy and the cipher sees the bytes.
void desEcbDecryptBlock(const std::uint8_t in[8], std::uint8_t out[8],
                        const std::array<std::uint64_t, 16>& subkeys);

// Expand a key into the sixteen round subkeys, in the order decryption consumes them.
std::array<std::uint64_t, 16> desKeySchedule(const std::uint8_t key[8]);

// A whole buffer, eight bytes at a time. `size` must be a multiple of eight; the caller pads, as
// the reference does with zero bytes, because the payload length is not a multiple of the block.
void desEcbDecrypt(const std::uint8_t* in, std::uint8_t* out, std::size_t size,
                   const std::array<std::uint64_t, 16>& subkeys);

}  // namespace nadirstation::crypto
