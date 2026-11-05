# VitaFin

A small, fast Jellyfin client written only for the PS Vita (HENkaku/Enso).

Why it should feel faster than the Switchfin family: no heavy UI toolkit, just vita2d.
Every network call runs on a background thread, posters load in the background at small
sizes and are cached, and scrolling is touch-first with inertia.

## Status: untested

This was written without access to a VitaSDK toolchain or a Vita, so it has never been
compiled or run. Expect a few compile errors on the first build (SceAvPlayer struct
field names and the link library list are the likeliest) and some runtime bugs.
Playback (SceAvPlayer) is the riskiest part.

## Build

Push this folder to a GitHub repo. The included Actions workflow builds the VPK
inside the `vitasdk/vitasdk` image; download `VitaFin.vpk` from the run's artifacts.
Paste any compile errors back to Claude and they can be fixed quickly.

Local build: install VitaSDK plus the vdpm packages `curl`, `jansson`, `vita2d`,
`libpng`, `libjpeg-turbo`, `freetype`, then:

    cmake -S . -B build && cmake --build build

## Setup on the Vita

1. Install the VPK with VitaShell.
2. Create `ux0:data/VitaFin/config.json`:

       { "server": "http://192.168.1.10:8096" }

   Optional: add `"username"` and `"password"` to skip Quick Connect.
3. Launch. Without a username it shows a Quick Connect code: in Jellyfin go to
   Profile > Quick Connect and enter it. (Quick Connect must be enabled in the dashboard.)

## Controls

- Browse: touch to scroll and tap, or D-pad + X. L/R switch libraries, O goes back.
- Detail: X play/resume, Square cycles subtitles, O back.
- Player: X pause, L/R or D-pad left/right seek 10s, Square subtitles, O stop, tap the bar to seek.
- Music: Triangle pause, Select next track.

## Known limits

- Direct play only for MP4/M4V + H.264 (<=960 wide) + AAC/MP3. Anything else asks the
  server for a transcode to that, which may not seek well.
- Music: MP3/AAC/M4A direct; FLAC is not supported by the Vita player.
- Subtitles are text only (SRT fetched from the server); image subtitles won't show.
- Lists load the first 300 items per library.
- TLS certificates are not verified (LAN use).
