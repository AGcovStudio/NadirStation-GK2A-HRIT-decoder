# NadirStation GK-2A HRIT Decoder 1.1.0

Offline decoder for the GK-2A (GEO-KOMPSAT-2A) HRIT downlink. It reads a baseband IQ recording and
demodulates the 3 Msym/s QPSK signal, corrects errors (Viterbi + Reed-Solomon), reassembles the
LRIT/HRIT files and builds images and a few L2-style products from them.

The baseband reader supports a wider set of recording formats:

| Format | Layout | Sample rate |
| --- | --- | --- |
| `cs8` / `s8` | interleaved I,Q, signed 8-bit | sidecar `.txt`, file name, or manual |
| `cs16` / `s16` | interleaved I,Q, signed 16-bit LE | sidecar `.txt`, file name, or manual |
| `cs32` / `s32` | interleaved I,Q, signed 32-bit LE | sidecar `.txt`, file name, or manual |
| `cf32` / `f32` | interleaved I,Q, 32-bit float LE | sidecar `.txt`, file name, or manual |
| `cu8` / `u8` | interleaved I,Q, unsigned 8-bit (127 = zero) | sidecar `.txt`, file name, or manual |
| `wav` | 2 channels, PCM 8/16/24/32-bit or 32-bit float, RIFF or RF64 | WAV header |
| `ziq` | ZIQ, uncompressed or zstd, 8/16-bit integer or 32-bit float | ZIQ header |
| `ziq2` | ZIQ2 packet stream, 8/16-bit integer | ZIQ2 header |

The format is taken from the file header first (`ZIQ_`, `ZIQ2`, `RIFF`/`RF64`) and from the extension
otherwise, so a renamed recording still opens. `.ziq`/`.ziq2` are the compressed and packetised
baseband formats; a zstd-compressed ZIQ needs zstd, which the Windows build includes.

It only reads files. It does not control any radio hardware, has no transmit function and makes no network connections.

Developer: AGcov (BI2QYN)

## What it does

- QPSK demodulator for 3.15 MS/s and up (tested from 3.2 MS/s to 6 MS/s), with a band-limited receive filter for low sample rates.
- DC blocker (500 Hz high-pass) ahead of the AGC and the carrier loops, on by default: an SDR's centre
  spike, LO leakage or ADC offset otherwise biases the AGC and can fool the fourth-power carrier search.
  Switch it off in the window if a recording genuinely has no DC offset.
- Soft-decision Viterbi (CCSDS K=7, r=1/2) and RS(255,223) with iterative feedback of corrected bytes.
- CADU framing, M_PDU/CCSDS packets, LRIT file assembly that tolerates lost packets: a lost packet zero-fills that spot instead of discarding the whole segment.
- JPEG 2000 / JPEG segment decoding (OpenJPEG, libjpeg-turbo), DES decryption of encrypted segments.
- Repair pass: after the first decode, frames that failed Reed-Solomon are decoded again from the recording around the gap with other loop bandwidths, filter shapes and soft-decision scalings, and the damaged segments are rebuilt. Only frames that pass Reed-Solomon are accepted, and only for counters that are really missing.
  (The repair pass needs an exact sample count; compressed ZIQ and ZIQ2 report their position as the file is read, so it is skipped for those.)
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

You need Qt 6 (MinGW 64-bit), CMake and the MinGW toolchain from the Qt installer. The CI build uses
Qt 6.8.3 LTS with MinGW 13.1; the release script below uses the Qt installation on your machine.

1. Fetch the third-party sources. They are not committed; the script downloads OpenJPEG 2.5.3,
   libjpeg-turbo 3.1.2 and zstd 1.5.7 into `third_party/`, checking pinned SHA-256 hashes:
   ```
   powershell -ExecutionPolicy Bypass -File scripts\fetch_third_party.ps1
   ```
   (Then `third_party/openjpeg-2.5.3/`, `third_party/libjpeg-turbo-3.1.2/` and
   `third_party/zstd-1.5.7/` sit next to `CMakeLists.txt`. You can also pass
   `-DHRIT_THIRD_PARTY_DIR=<folder>` or point `-DHRIT_ZSTD_SOURCE_DIR` at another zstd copy.)
2. Edit the three paths at the top of `scripts/build_release.bat` if Qt lives somewhere else.
3. Run `scripts\build_release.bat`. The program ends up in `dist\`; if Inno Setup 6 is installed, the installer is made in `installer\output\`.

Or by hand:

```
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.11.2/mingw_64
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

A command-line check of the receive chain without the window is built as well:

```
hrit_cli <recording> [sample rate in Hz] [--no-repair]
hrit_cli --formats
```

The GUI can be skipped for a quick command-line-only build (no Qt, no OpenJPEG/libjpeg-turbo):

```
cmake -S . -B build-cli -G Ninja -DCMAKE_BUILD_TYPE=Release -DHRIT_BUILD_GUI=OFF
cmake --build build-cli --parallel
ctest --test-dir build-cli --output-on-failure
```

CMake options: `HRIT_BUILD_GUI` (ON), `HRIT_BUILD_CLI` (ON), `HRIT_BUILD_TESTS` (ON),
`HRIT_WITH_ZSTD` (ON; only compressed ZIQ needs it), `HRIT_THIRD_PARTY_DIR`.

## Continuous integration

`.github/workflows/windows-build.yml` builds the Windows version on GitHub Actions for every push
to `main`/`master`, every pull request and on manual dispatch:

- installs Qt 6.8.3 (win64_mingw) and the matching MinGW 13.1 toolchain;
- fetches the pinned third-party sources (cached between runs);
- configures, builds, runs CTest and checks that `hrit_cli --formats` reports ziq/ziq2 and zstd support;
- stages `dist\` with `windeployqt`, uploads a portable ZIP, and builds the Inno Setup installer when
  Inno Setup is available.

The artifacts are `NadirStation-GK2A-HRIT-decoder-win64` (portable ZIP) and
`NadirStation-GK2A-HRIT-decoder-win64-installer` (setup executable).

## Using it

`.cs8` / `.cs16` / `.cs32` / `.cf32` / `.cu8` files have no sample-rate field. The program looks for `rate <Hz>`
in a `.txt` next to the file, then for `3200000SPS` / `3.2Msps` in the file name (for example
`..._3200000SPS_1695400000Hz.cs16`), then uses the value you type. WAV, ZIQ and ZIQ2
files carry it in the header. Choose your key file in the window, or put it next to the program or next to the recording.

For compressed ZIQ and ZIQ2 the total sample count is not known until the end of the file: the window shows
the progress by file position, and the repair pass (which seeks to exact sample positions) is skipped.

## Licence

The source code is released under the **GNU General Public License v3.0 or later**, see `LICENSE`.
Copyright (c) 2026 AGcov (BI2QYN).

Third-party components keep their own licences: Qt (LGPL v3, dynamically linked), OpenJPEG (BSD-2-Clause), libjpeg-turbo
(BSD-3-Clause / IJG / zlib), zstd (BSD-3-Clause, statically linked, used only to read compressed ZIQ), Natural Earth data
(public domain). See `installer/THIRD_PARTY_NOTICES.txt` and `installer/LICENSES/`.

`installer/EULA.txt` and `resources/eula.html` are the terms of use and disclaimer shown in the
installer and in the program. The disclaimer parts (no warranty, no affiliation with KMA/NMSC, you are responsible for
complying with the law and the data policy) apply to everything you build from this source too. The licence parts of those texts
describe the official binary installer; for the source code, `LICENSE` governs.

This is a hobby project. It is not affiliated with or endorsed by KMA, NMSC or any satellite operator, and it is not a meteorological product.
