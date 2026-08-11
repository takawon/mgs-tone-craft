# Changelog

このファイルは、利用者から見える変更の通し履歴です。
バイナリ（GitHub Releases の ZIP）を更新するときは、次を両方更新します。

1. 本ファイル（`CHANGELOG.md`）へ版ごとの項目を追記する
2. 同じ内容の要約を、当該版の [GitHub Release notes](https://github.com/takawon/mgs-tone-craft/releases) へ載せる

形式は [Keep a Changelog](https://keepachangelog.com/ja/1.1.0/) に近い書き方とします。
版番号は公開用`VERSION`（およびローカル仕様書の文書版）と揃えます。

カテゴリの目安:

- **Added**: 新しい機能
- **Changed**: 既存の振る舞いの変更
- **Fixed**: 不具合修正
- **Removed**: 削除した機能
- **Notes**: 互換性・Alpha制限など、利用者への注意

## [Unreleased]

### Changed

- 参考資料に [MAmidiMEmoNEMO readMe](https://github.com/uniskie/MSX_DOCUMENTS/blob/main/MAmidiMEmoNEMO/readMe.md)（uniskie）を追加

## [0.165] - 2026-08-11

`0.101` 以降の公開 Alpha 向けまとめ（GitHub Releases 更新用）。

### Added

- **ライブラリ管理**ウィンドウ（音色一覧／タグ管理、SCC・OPLL・複合、複製・削除・タグ付与・読込して編集、★・ソート・検索）
- 右詳細での名前・メモ編集、付与タグのチップ＋×で外す操作
- 78種の標準タグ／独自タグ、複数AND絞り込み、独自タグの一括改名・統合・削除
- お気に入り・最近使用・並べ替え、選択時試聴と A/B 比較
- 未保存変更の終了／補助画面クローズ確認
- ASIO 出力（選択・設定画面・失敗時復帰）とレート変換
- OPLL 変換の品質選択（標準／じっくり）、進捗表示、キャンセル
- MGSC プレビュー／コピー／`.txt` への音色名コメント
- Windows アプリアイコン、About の著作権／GitHub／X リンク
- ハング検知ログ（`%LOCALAPPDATA%\MgsToneCraft\hang-*.log`）

### Changed

- 鍵盤 `C1`～`B8` を MGSDRV／MGSC の `o1c`～`o8b` 対応へ統一
- UI フォント・配色・ホバー／選択ハイライトの整理（緑アクセント）
- SCC／OPLL エディタ初期サイズとライブラリパネル配置の見直し
- 埋め込みライブラリの操作配置（読込・削除・名前変更など）。複製はライブラリ管理側

### Fixed

- SCC 波形ドラッグ中の PC キー無反応
- ASIO 設定後に WASAPI へ誤って戻る問題
- タイトルバー活性化後の `Ctrl+C`／`V`／`Z`／`Y`
- A/B 表示の文字化け
- OPLL キーオフ後のハング対策（強制消音秒数）

### Notes

- 引き続き Alpha。総合画面の複合エンベロープや曲用フル `.MUS` は未保証
- ライブラリ保存形式に後方互換の保証はない（旧形式は読めない場合あり）

## [0.101] - 2026-08-09

初回の公開 Alpha。

### Added

- SCC／OPLL 単音色エディタ（波形・パラメーター編集、ライブラリ、`@s`／`@v` 入出力）
- 画面鍵盤／PCキー／MIDI／内蔵エミュレータによる試聴
- MAmi-VSIF dongle／MAmidiMEmo 経由の実機発音
- WAV／Audacity からの近似変換、SCC↔OPLL 相互近似変換
- Windows x64 配布 ZIP（`mgstc.exe` とライセンス一式）

### Notes

- Alpha（機能限定）。総合画面の複合エンベロープや曲用フル `.MUS` 出力は未保証
- 音色ライブラリの保存形式に後方互換の保証はない
