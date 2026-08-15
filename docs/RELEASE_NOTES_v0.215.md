# GitHub Release notes 下書き — v0.215

公開アップロード時に Release 本文へ貼る用（バイナリ未アップロード）。

```
Alpha update (0.215).

## Added
- Major progress on composite software-envelope editing (@e preview, timeline UI)
- Composite initial settings (rests, relative pitch, OPLL ROM @0–@14, tempo, …)
- Edit assigned SCC/OPLL while hearing the composite output
- Richer hang logs (activity name, stage: detected / still-hung / recovered)

## Changed
- Composite layout (right library, 1600×1050, clamp windows to the monitor work area)
- Keyboard / WASAPI responsiveness and window switching polish
- Credits: uniskie MAmidiMEmoNEMO readMe

## Fixed
- Idle multi-second freezes during PC-audio device recovery
- Hang on quit / output switch with a stuck audio device
- Title-bar focus / switching and unsaved-dialog edge cases
- Windows overlapping the taskbar

## Notes
- Still Alpha. Full count-command serialization, `[` zone editing, and full .MUS export remain unfinished
- Composite / library save formats have no backward-compatibility promise

See CHANGELOG.md for details.
```
