---
name: mkvclean
description: Extract English subtitles from MKV files and remux them down to the video plus the default English audio track, replacing the originals. Use when asked to strip subtitles or extra audio tracks from MKV files or whole folders, extract .srt subtitles from videos, or keep only the English audio. Requires MKVToolNix (mkvmerge, mkvextract).
---

# mkvclean

`mkvclean clean <file-or-folder>… [--json]` does the whole job headless, for
one file or many (folders are scanned recursively for `*.mkv`):

1. Extracts every English subtitle track next to the original, tagged by
   kind: `<name>.en.srt` (plain), `<name>.sdh.en.srt` (SDH/CC, by flag or
   track name), `<name>.forced.en.srt` (forced); extra tracks of a kind
   number as `<name>.2.en.srt`, `<name>.sdh.2.en.srt`, ….
2. Replaces each original MKV in place with a remux containing the video plus
   the default English audio track only — no subtitles, no other audio, and
   no leftover track names (they are blanked so players show the language).
   The remux is verified before the switch, and the original is moved to the
   trash when one is available (recoverable from there).

A file with no English audio track is left untouched and reported as an
error; one bad file never stops the batch.

## Output

Single file — one JSON object (short text lines without `--json`):

```json
{"input":"…","output":"… (same path as input)","subtitles":["…"],"kept_audio":{"id":1,"language":"eng","codec":"AAC"},"removed":{"tracks":4}}
```

Multiple files — one JSON object per line (JSONL), each carrying
`"status": "ok"` or `"status": "error"` plus an `"error"` message:

```json
{"input":"…","status":"ok","output":"…","subtitles":["…"],"kept_audio":{"id":1}}
{"input":"…","status":"error","error":"No English audio track in this file."}
```

Exit 0 when everything was cleaned, 1 if any file failed. `mkvclean [open]
<file-or-folder>…` opens the GUI instead.
