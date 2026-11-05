---
name: mkvclean
description: Extract English subtitles from MKV files and remux them down to the video plus the default English audio track, replacing the originals. Use when asked to strip subtitles or extra audio tracks from MKV files or whole folders, extract .srt subtitles from videos, or keep only the English audio. Requires MKVToolNix (mkvmerge, mkvextract).
---

# mkvclean

`mkvclean clean <file-or-folder>… [--json]` does the whole job headless, for
one file or many (folders are scanned recursively for `*.mkv`):

1. Extracts English subtitle tracks next to the original, tagged by kind:
   `<name>.en.srt` (plain) and `<name>.forced.en.srt` (forced) always;
   `<name>.sdh.en.srt` (SDH/CC, by flag or track name) only when there is no
   plain English subtitle. Extra tracks of a kind number as
   `<name>.2.en.srt`, `<name>.sdh.2.en.srt`, ….
2. Replaces each original MKV in place with a remux containing the video plus
   one audio track: the default English track, or the default foreign track
   when there is no English at all (a foreign film stays watchable; a file
   with no audio becomes video-only). No subtitles, no other audio, no
   leftover track names (they are blanked so players show the language). The
   remux is verified before the switch, and the original is moved to the
   trash when one is available (recoverable from there). If the original is
   busy, the verified remux is left as `<name>.mkv.remux` and the watch
   service completes the swap on a later pass.

A corrupt or unreadable file is reported as an error and left untouched; one
bad file never stops the batch.

## Output

Single file — one JSON object (short text lines without `--json`):

```json
{"input":"…","output":"… (same path as input)","subtitles":["…"],"kept_audio":{"id":1,"language":"eng","codec":"AAC"},"removed":{"tracks":4}}
```

`kept_audio` is `null` when the file had no audio track at all.

Multiple files — one JSON object per line (JSONL), each carrying
`"status": "ok"` or `"status": "error"` plus an `"error"` message:

```json
{"input":"…","status":"ok","output":"…","subtitles":["…"],"kept_audio":{"id":1}}
{"input":"…","status":"error","error":"No English audio track in this file."}
```

Exit 0 when everything was cleaned, 1 if any file failed. `mkvclean [open]
<file-or-folder>…` opens the GUI instead.

## Watch service

`mkvclean watch` runs a headless watcher over the folders in
`~/.config/mkvclean/watch-folders`: each new MKV is cleaned (same job as
above) once the file has finished copying. Only files that arrive while the
watcher runs are processed — existing files are never touched; failures are
logged and skipped. Manage it with:

```
mkvclean watch add <folder>        # configure a watched folder
mkvclean watch exclude <folder>    # never watch/scan inside this folder
mkvclean watch remove <folder>     # …or unexclude <folder>
mkvclean watch list
mkvclean watch scan [--json]       # clean everything missed so far
mkvclean watch enable              # systemd user service: start now + at login
mkvclean watch status
mkvclean watch disable
```

Preferences (watched folders, excluded folders, preferred audio/subtitle
languages — ISO 639-2, default `eng`) live in `~/.config/mkvclean/config.json`
and apply to the service and `watch scan`. Exclusions never affect an
explicit `clean <path>` — that always processes what it is pointed at.

Logs from the service: `journalctl --user -u mkvclean -f`.
