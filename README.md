# FastForward

The Amiga personal computer is the greatest multimedia computer ever made and as such deserves 
a full-fledged pluggable multimedia framework at the operating system level — shared infrastructure  
for playback, editing, streaming, and format handling that any application can build on.

Apple had **QuickTime**, BeOS its **Media Kit**, even Windows had **DirectShow**. 

Now it's time for Amiga to catch up with *FastForward*

**FastForward** from *amigazen project* expands upon the foundations provided by `realtime.library`, 
`datatypes.library` and the rich world of open source multimedia software options found on Amiga to 
provide a native, performance optimised multimedia subsystem — a LoadSeg-pluggable pipeline from byte 
stream to pixels and samples, with QuickTime/MP4-style multimedia container parsing and a BeOS-style 
filter graph for editors, sequencers and transcoders.

**FastForward** comprises:

- **`fastforward.library`** — the shared engine: plugin registry, pipeline and filter graph, movie and graph APIs, sniffing, buffer pools, clocks, events, probe, stats, metadata and thumbnail support
- **Play** — Universal media player tool (`Source/play/`)
- **Examine** — Shell command media inspector; reports containers, streams, and which plugins would bind to the file being examined
- **player.gadget** and **playback.gadget** — BOOPSI gadget classes for embedding a media player and its playback controls in Intuition applications

**Built-in plugins** 

- `mem.source` — in-memory byte streams
- `callback.sink` — delivers frames to application callbacks

**FastForward plugins**

*Sources* — `file.source` for local files, `http-live.source` for HTTP-LS streams, `rtsp.source` for RTSP streams

*Demuxers* — `pcm.demux`, `mov.demux`, `avi.demux`, `cdxl.demux`, `au.demux`, `fpadpcm.demux`, `rm.demux`, `mpegaudio.demux`, `mpeg.demux`, `ts.demux`, `ogg.demux`, `flac.demux`, `mkv.demux`

*Codecs* — `pcm.codec`, `ima4.codec`, `fpadpcm.codec`, `msadpcm.codec`, `dviadpcm.codec`, `mace.codec`, `mpegaudio.codec`, `mpegvideo.codec`, `vorbis.codec`, `cvid.codec`, `svq1.codec`, `msvc.codec`, `rpza.codec`, `smc.codec`, `qtrle.codec`, `cdxl.codec`, `ra144.codec`, `ra288.codec`

*Filters* — `volume.filter`, `fade.filter`

*Sinks* — `audio.sink`, `ahi.sink`, `video.sink`, `ham.sink`, `null.sink`

**Additional plugins subject to GPL licensing**

- `a52.codec` — AC-3 (liba52)
- `aac.codec` — AAC-LC (FAAD2)


## [amigazen project](http://www.amigazen.com)

*A web, suddenly*

*Forty years meditation*

*Minds awaken, free*

**amigazen project** is using modern software development tools and methods to update and rerelease classic Amiga open source software. Projects include a new AWeb, a new Amiga Python 2, and the ToolKit project — a universal SDK for Amiga development. All *amigazen project* releases are guaranteed to build against the ToolKit standard so that anyone can download and begin contributing straightaway without having to tailor the toolchain for their own setup.

FastForward is an original work of the amigazen project, released under the BSD 2-Clause License (see [LICENSE](LICENSE)). Third-party codec engines wrapped by FastForward plugins retain their own licences; GPL codecs ship as separate LoadSeg binaries that are never statically linked into the core library.

The amigazen project philosophy is based on openness:

*Open* to anyone and everyone — *Open* source and free for all — *Open* your mind and create!

PRs for all projects are gratefully received at [GitHub](https://github.com/amigazen/). While the focus now is on classic 68k software, it is intended that all amigazen project releases can be ported to other Amiga-like systems including AROS and MorphOS where feasible.

## About FastForward

fastforward.library is a **standalone multimedia engine** for classic Amiga.
It performs format sniffing, container demuxing, codec decode, and device output through a linear or
arbitrary filter graph. It does **not** replace `datatypes.library` for still images — PNG, ILBM, JPEG,
and GIF stay on datatypes. FastForward owns timed media, and in fact implements the backend needed for 
the movie.datatype rootclass.

`dos.library`, `utility.library`, and `realtime.library` (for Conductor-backed clocks) open on-demand
inside the engine. Codec engines such as `mpega.library` or `liba52` are **plugin-owned** — the core
library opens none of them at init time, and FastForward plugins can be self contained, or defer to external shared libraries as appropriate.

## Inspirations

FastForward stands on decades of Amiga and cross-platform multimedia work. The design deliberately
combines ideas that already proved themselves elsewhere, expressed in Amiga-native primitives:

| Inspiration | What we took from it |
|-------------|----------------------|
| **BeOS Media Kit** (1998) | Node-per-thread topology, `BBuffer` flowing between nodes on kernel ports, a master time source anchoring the graph. FastForward maps this onto Exec Tasks, `Message`/`PutMsg`/`ReplyMsg`, and `realtime.library` Conductors — the architecture BeOS got right, on primitives Amiga has had since 1985. |
| **MacOS QuickTime** | The Movie / Track model for player apps: one high-level `FFMovie` handle, per-track mute/volume/sink control, and metadata introspection without exposing the underlying graph. |
| **AmigaOS realtime.library** | Conductor + Player as the master clock for sequencers and NLE timelines; per-element Players slaved to one graph Conductor for sample-accurate multi-track sync. |

Two metaphors read the architecture out loud. **Orchestral** language fits the timing layer
(Conductor, Player, tempo, ticks). **Tape-deck** language fits the application boundary (Play,
Pause, Stop, scrub, and *FastForward*). 

## Contact

- At GitHub https://github.com/amigazen/FastForward/
- On the web at http://www.amigazen.com/fastforward/ (Amiga browser compatible)
- Or email fastforward@amigazen.com

## Acknowledgements

*Amiga* is a trademark of **Amiga Inc**.
