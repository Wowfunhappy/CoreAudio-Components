This repository contains audio components which allow e.g. QuickTime to play audio codecs which Mac OS X does not natively support. They have been tested on Mac OS X 10.6 (Snow Leopard) – 10.9 (Mavericks). They may or may not work on more recent versions of macOS; note that OS X 10.11+ natively supports EAC3, and macOS 11+ (I think) supports FLAC.

To install, copy built .component to Library/Audio/Plug-Ins/Components/.

These components were written almost entirely by Claude, via an enormous amount of trial and error. They work well, but code quality is assumed to be terrible.

Note that these components do NOT add support for additional containers. For example, the FLAC component will allow applications to play FLAC audio, but it will not teach them how to open .flac files. The FLAC audio stream would need to be in a container which the system already understands, such as .mov or .mp4. In the case of FLAC, this is quite unusual.



MULTI-CHANNEL AUDIO:
===================

FLAC & OPUS:
-----------
Supports multi-channel audio but without downmixing (excess channels discarded).

EAC3:
----
Supports multi-channel audio with automatic downmixing when necessary. May misreport that audio has 8 channels regardless of actual count; this should not affect playback.

If channel mappings are incorrect, please contact Wowfunhappy, who lacks the necessary audio setup to test this properly.