# MGS Tone Craft ビルド手順

## 必要環境

- Windows 10または11
- Visual Studio Community 2026
  - Desktop development with C++
  - Windows 10/11 SDK
- CMake 3.25以上
- Ninja（Visual StudioのDeveloper Command Promptに同梱）

現在の開発環境では、Visual Studio 2026、MSVC 19.51、Windows SDK
10.0.26100.0、CMake 4.2で動作確認している。

## 構成

Visual StudioのDeveloper PowerShellでプロジェクトのルートへ移動し、次を
実行する。

```powershell
cmake -S . -B build -G Ninja
```

## ビルド

```powershell
cmake --build build --parallel
```

## テスト

```powershell
ctest --test-dir build --output-on-failure
```

テスト実行ファイルを直接起動する場合:

```powershell
.\build\mgstc_engine_tests.exe
```

## 現在のターゲット

- `mgstc_engine`
  - UI非依存のC++20静的ライブラリ
- `emu2149` / `emu2212` / `emu2413`
  - `third_party`へ固定した音源エミュレータ
- `mgstc_engine_tests`
  - 外部テストフレームワークに依存しないコア・音声生成テスト
- `mgstc_chip_level_probe`
  - PSG・SCC・OPLLの単音Peak / RMSを再測定する診断ツール
- `mgstc_windows_audio`
  - Windows共有モード・イベント駆動WASAPI出力
- `mgstc`
  - Win32ネイティブアプリ本体

ネイティブアプリを起動する場合:

```powershell
.\build\mgstc.exe
```

音源エミュレータの固定コミットとライセンスは
[THIRD_PARTY.md](THIRD_PARTY.md)を参照する。

## 日本語パスについて

Visual StudioとCMakeは通常、日本語を含むプロジェクトパスを扱える。
一部の自動実行環境で8.3短縮パスに関するMSBuild警告が出る場合があるが、
コンパイル結果には影響しない。
