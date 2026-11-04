# NadirStation GK-2A HRIT Decoder 1.0.5

Offline decoder for the GK-2A (GEO-KOMPSAT-2A) HRIT downlink. It reads a baseband IQ recording
(`.cs8`, `.cs16`, `.wav`), demodulates the 3 Msym/s QPSK signal, corrects errors (Viterbi + Reed-Solomon),
reassembles the LRIT/HRIT files and builds images and a few L2-style products from them.

It only reads files. It does not control any radio hardware, has no transmit function and makes no network connections.

Developer: AGcov (BI2QYN)

## What it does

- QPSK demodulator for 3.15 MS/s and up (tested from 3.2 MS/s to 6 MS/s), with a band-limited receive filter for low sample rates.
- Soft-decision Viterbi (CCSDS K=7, r=1/2) and RS(255,223) with iterative feedback of corrected bytes.
- CADU framing, M_PDU/CCSDS packets, LRIT file assembly that tolerates lost packets: a lost packet zero-fills that spot instead of discarding the whole segment.
- JPEG 2000 / JPEG segment decoding (OpenJPEG, libjpeg-turbo), DES decryption of encrypted segments.
- Repair pass: after the first decode, frames that failed Reed-Solomon are decoded again from the recording around the gap with other loop bandwidths, filter shapes and soft-decision scalings, and the damaged segments are rebuilt. Only frames that pass Reed-Solomon are accepted, and only for counters that are really missing.
- Lost segments are filled by interpolation between the neighbouring segments, and the output says which ones.

## Weak signals

In simulation (6 MS/s, 30 random seeds per point, loss measured after carrier and timing acquisition) about 98 % of frames pass
Reed-Solomon at Es/N0 = 1.4 dB and essentially all of them from 1.6 dB; no frame ever passed the check with wrong content.
A real recording that the software estimates at about 1.4 dB decodes to a complete, lossless image.

## The decryption key is not included

Some HRIT segments are encrypted. The key file (`EncryptionKeyMessage.bin`) is issued by the satellite operator
under its own conditions. **This repository and the program do not contain it, and never will.** Get it through
the operator's official channel, check its terms, and do not publish it. Without the key, encrypted segments cannot be decoded.

## Build (Windows, MinGW)

You need Qt 6 (MinGW 64-bit), CMake and the MinGW toolchain from the Qt installer.

1. Download the sources of **OpenJPEG 2.5.3** and **libjpeg-turbo 3.1.2** and unpack them so you have
   `third_party/openjpeg-2.5.3/` and `third_party/libjpeg-turbo-3.1.2/` next to `CMakeLists.txt`
   (or pass `-DHRIT_THIRD_PARTY_DIR=<folder>`).
2. Edit the three paths at the top of `scripts/build_release.bat` if Qt lives somewhere else.
3. Run `scripts\build_release.bat`. The program ends up in `dist\`; if Inno Setup 6 is installed, the installer is made in `installer\output\`.

Or by hand:

```
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.11.2/mingw_64
cmake --build build --parallel 4
```

A command-line check of the receive chain without the window is built as well:

```
hrit_cli <recording> [sample rate in Hz]
```

## Using it

`.cs8` / `.cs16` files have no sample-rate field. The program looks for `rate <Hz>` in a `.txt` next to the file, then for
`3200000SPS` / `3.2Msps` in the file name, then uses the value you type. WAV files carry it in the header.
Choose your key file in the window, or put it next to the program or next to the recording.

## Licence

The source code is released under the **GNU General Public License v3.0 or later**, see `LICENSE`.
Copyright (c) 2026 AGcov (BI2QYN).

Third-party components keep their own licences: Qt (LGPL v3, dynamically linked), OpenJPEG (BSD-2-Clause), libjpeg-turbo
(BSD-3-Clause / IJG / zlib), Natural Earth data (public domain). See `installer/THIRD_PARTY_NOTICES.txt` and `installer/LICENSES/`.

`installer/EULA.txt` and `resources/eula.html` are the terms of use and disclaimer shown in the
installer and in the program. The disclaimer parts (no warranty, no affiliation with KMA/NMSC, you are responsible for
complying with the law and the data policy) apply to everything you build from this source too. The licence parts of those texts
describe the official binary installer; for the source code, `LICENSE` governs.

This is a hobby project. It is not affiliated with or endorsed by KMA, NMSC or any satellite operator, and it is not a meteorological product.
