# Third-party components

| Component | License | Notes |
|---|---|---|
| [dr_libs](https://github.com/mackron/dr_libs) (`dr_mp3.h`, `dr_flac.h`) | MIT-0 / public domain | vendored in `source/third_party/` |
| [stb_vorbis](https://github.com/nothings/stb) | MIT | vendored in `source/third_party/` |
| [Mozilla CA bundle](https://curl.se/ca/cacert.pem) | MPL-2.0 | `data/ca_bundle.pem`, embedded via `source/ca_bundle.h` |
| [mbedTLS](https://www.trustedfirmware.org/projects/mbed-tls/) | Apache-2.0 | linked from devkitPro portlibs |
| [FFmpeg](https://ffmpeg.org/) | GPL-2.0-or-later (build uses `--enable-gpl`) | linked statically: `libavformat`, `libavcodec`, `libavutil`, `libswresample` |
| [libnx](https://github.com/switchbrew/libnx) | ISC | linked |
| [DejaVu fonts](https://dejavu-fonts.github.io/) | Bitstream Vera / public domain | used to generate `source/font8x16.h` |

The project as a whole is distributed under **GPL-3.0** because it links a GPL build of
FFmpeg. If you rebuild FFmpeg without GPL components, you may relicense the combined work
accordingly.
