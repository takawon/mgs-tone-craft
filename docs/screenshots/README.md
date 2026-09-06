# 公開用スクリーンショット

README トップ説明用。バイナリ／README 更新のたびに現行 UI の最新版へ差し替える。
（この README 自体は作業メモ。利用者向けの注記は README 本体側。）

## 必須ファイル

| ファイル | 内容 |
|---|---|
| `main.png` | 総合画面 |
| `opll-editor.png` | OPLL 音色エディタ |
| `scc-editor.png` | SCC 音色エディタ |
| `settings-view.png` | アプリ設定・View タブ |
| `settings-midi.png` | アプリ設定・MIDI タブ |
| `settings-output.png` | アプリ設定・Output タブ |
| `library-manager.png` | 音色ライブラリ管理 |
| `spectrogram-playing.png` | OPLL単音試聴中のスペクトログラム |

README では各群の直下に `※画面は開発中のものです` を入れる。

## 撮影

```text
.\tools\capture-ui.cmd -Editor main -Target editor -OutputPath docs\screenshots\main.png
.\tools\capture-ui.cmd -Editor opll -Target editor -OutputPath docs\screenshots\opll-editor.png
.\tools\capture-ui.cmd -Editor scc -Target editor -OutputPath docs\screenshots\scc-editor.png
.\tools\capture-ui.cmd -Editor main -Target settings-view -OutputPath docs\screenshots\settings-view.png
.\tools\capture-ui.cmd -Editor main -Target settings-midi -OutputPath docs\screenshots\settings-midi.png
.\tools\capture-ui.cmd -Editor main -Target settings-output -OutputPath docs\screenshots\settings-output.png
.\tools\capture-ui.cmd -Editor scc -Target library -OutputPath docs\screenshots\library-manager.png
.\tools\capture-ui.cmd -Editor opll -Target spectrogram -OutputPath docs\screenshots\spectrogram-playing.png
```
