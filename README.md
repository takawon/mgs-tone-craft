# MGS Tone Craft

MGS Tone Craft（MGSTC）は、MGSDRV向けの複合音色エディタです。
PSG（YM2149）、SCC、OPLL（YM2413）の複数レイヤーを組み合わせ、
音色の編集、試聴、保存、MGSC 1.11でコンパイル可能なMML出力を行います。

現在は正式リリース前の開発段階です。

## 対象音源

- PSG / SSG（YM2149）
- SCC
- OPLL（YM2413）

## ビルド

Windows、Visual Studio 2026、CMake 3.25以上を使用します。
詳しい手順は[BUILDING.md](BUILDING.md)を参照してください。

## Audacityから変換を使うときの注意事項

OPLL／SCC単音色エディタの「Audacityから変換」は、Audacityで現在選択している音声範囲を一時WAVへ書き出して音色変換します。

使用前にAudacityの「編集」→「環境設定」→「モジュール」で`mod-script-pipe`を有効にし、Audacityを再起動してください。その後、音声トラック上で変換する時間範囲を選択してから「Audacityから変換」を押します。

`mod-script-pipe`を有効にすると、同じPC上で動作する他のプログラムからもAudacityを操作できる状態になります。信頼できないプログラムを実行する環境や複数ユーザーが同時利用する環境では有効にしないでください。連携を使用しない期間は、Audacityのモジュール設定で無効に戻すことを推奨します。

## 文書

- [仕様書](SPECIFICATION.md)
- [音源エンジン設計](ENGINE_DESIGN.md)
- [サードパーティー情報](THIRD_PARTY.md)
- [公開・配布チェックリスト](RELEASE_CHECKLIST.md)
