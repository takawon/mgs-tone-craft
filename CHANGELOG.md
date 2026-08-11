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

ソース上の版は`0.121`付近まで進んでいますが、GitHub Releases の配布ZIPは
まだ`0.101`です。次回バイナリを公開するときに、利用者向けの変更をここに整理して
版見出し（例: `## [0.122] - 日付`）へ移し、同じ要約を Release notes へ載せます。

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
