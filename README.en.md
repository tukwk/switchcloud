# switchcloud

A SoundCloud-style music player for modded Nintendo Switch (Atmosphère / homebrew).
Plays local files from the SD card, searches SoundCloud, resolves playlists (including
your own private ones with an OAuth token) and caches tracks for offline listening.

**Русский:** [README.md](README.md) · **English** · v0.1

## Features

- **Local library**: scans `sdmc:/switch/scswitch/music`, `sdmc:/music`, `sdmc:/Music`
  recursively; "Scan whole SD" searches the entire card. Reads ID3v2/ID3v1 tags and
  falls back to `Artist - Title.mp3`.
- **Playback**: MP3 (dr_mp3), FLAC (dr_flac), OGG/Vorbis (stb_vorbis) and AAC/M4A via
  libavformat/libavcodec. Custom windowed-sinc resampler 44.1→48 kHz, output through
  `audout` (48 kHz, stereo, s16).
- **SoundCloud**: track search, public playlist search, open playlist/account URLs,
  private playlists via an OAuth token, download to the cache and play offline.
- **HLS/AAC**: when a track has no progressive MP3, the HLS (fMP4) stream is downloaded
  into `cache/sc_<id>.m4a` and decoded with FFmpeg — offline playback included.
- **UI**: 1280×720 dark theme, gamepad + touch controls, software keyboard, likes,
  queue, shuffle, downloads, settings.
- **Bypass**: SOCKS5 / HTTP CONNECT proxy support (including a helper that exposes the
  PC's VPN route to the Switch).

## Requirements

- Atmosphère + hbmenu (title override is required for audio).
- devkitPro with `devkitA64`, `libnx`, `switch-mbedtls` and the FFmpeg portlibs
  (`libavformat`, `libavcodec`, `libavutil`, `libswresample`).
- Python 3 + Pillow only to regenerate the font/icon (already committed).

## Build

```bash
export DEVKITPRO=/opt/devkitpro      # or your local devkitPro path
make                                  # → build/scswitch.nro
make test                             # host tests: JSON parser + resampler
make dist                             # dist/switchcloud-<date>.zip
```

No exceptions/RTTI; the NRO is self-contained. `tools/check_relocs.py` runs during the
build and refuses relocations that would land in the read-only NRO segment.

## Install

1. Copy `build/scswitch.nro` to `sd:/switch/scswitch.nro`.
2. Launch hbmenu via **title override**: hold **R** on a game and start it (audio is not
   available in Applet Mode).
3. Optional folders:

```
sd:/switch/scswitch/
├── config.cfg
├── likes.txt
├── log.txt
├── music/          # your local files
├── cache/          # downloaded SoundCloud tracks
└── cacert.pem      # optional custom CA bundle
```

## Configuration

`config.cfg`:

```ini
client_id=<SoundCloud client id>
proxy=socks5://192.168.1.10:10808
oauth_token=<optional OAuth token>
music_dir=sdmc:/switch/scswitch/music
cache_dir=sdmc:/switch/scswitch/cache
insecure_tls=0
lang=ru
```

- `client_id`: open soundcloud.com in a browser → DevTools → Network → copy `client_id`
  from an `api-v2.soundcloud.com` request. Or run `python3 tools/get_client_id.py`.
- `oauth_token`: sign in on soundcloud.com → DevTools → Application → Cookies →
  `oauth_token`. Needed for your own private playlists.
- `proxy`: SOCKS5 (`socks5://host:port`) or HTTP CONNECT (`http://host:port`), optional
  `user:pass@`. If the proxy is unreachable the request falls back to a direct one.
- `lang`: `ru` or `en` (also switchable in Settings).

### Bypass helper

```bash
tools/start_bypass.sh
```

Starts a LAN SOCKS5 proxy on the PC, detects its LAN IP, prints
`socks5://<ip>:10808` and updates `config.cfg` over MTP when the SD card is mounted.
The PC must be on the same network as the Switch and its VPN must be connected.

## Controls

| Input | Action |
|---|---|
| ↑/↓ or left stick | select a row |
| ←/→ | seek ±10 s (full-screen player) |
| A | play / pause / confirm / open keyboard |
| B | back |
| X | ♥ (delete in Downloads, open URL in Playlists) |
| Y | context: search / rescan / download playlist / shuffle |
| ZL / ZR | sections (previous/next track in the player) |
| L / R | volume |
| + | pause/resume |
| − | full-screen player |
| Touch | prev / play / next buttons and the waveform seek bar |

English is the default UI language; Russian can be selected in Settings → Language.

## Known limitations

- HLS support covers unencrypted fMP4 (`EXT-X-MAP` + `.m4s`), no AES-128.
- AAC/M4A audio is supported, MP4 video is not.
- No account feed/likes sync; playlists only.
- `client_id` rotates; refresh it with `tools/get_client_id.py`.
- Some tracks expose only a 30-second preview or no progressive/HLS stream at all.
- SoundCloud's private API use may violate its Terms of Service; use at your own risk.

## License

The project is **GPL-3.0** (see `LICENSE`) because it statically links the GPL build of
FFmpeg from devkitPro. Third-party components and their licenses are listed in
`THIRD_PARTY.md`.

## Layout

```
source/
├── main.cpp        screens, input (pad + touch), i18n-wrapped UI
├── gfx.*           software framebuffer renderer + 8×16 UTF-8 font
├── audio.*         decoders, audout output, FFmpeg AAC/HLS playback
├── resampler.h     windowed-sinc 44.1→48 kHz resampler
├── hls.*           m3u8/fMP4 downloader
├── http.*          HTTPS client (mbedTLS, proxy, redirects, timeouts)
├── json.*          minimal JSON parser
├── sc.*            api-v2: search, playlists, OAuth, transcodings
├── library.*       SD scanner, ID3, cache index
├── core.*          config, utils, likes, logging
├── i18n.*          RU/EN dictionary
└── third_party/    dr_mp3, dr_flac, stb_vorbis
```
