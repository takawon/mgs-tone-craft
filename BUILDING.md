# MGS Tone Craft ビルド手順

## 必要環境

- Windows 10または11
- Visual Studio Community 2026
  - Desktop development with C++
  - Windows 10/11 SDK
- CMake 3.25以上
- Ninja（Visual StudioのDeveloper Command Promptに同梱）
- Git

依存関係のJUCEとrpclibはリポジトリへvendorせず、CMakeの構成時に`FetchContent`が
公式リポジトリから取得する。このためGitとネットワーク接続が必要である。
`git`がPATHに無い場合、`tools\build.cmd`はVisual Studio同梱のGitを探して
プロセス内のPATHへ追加する。見つからない場合は導入を促して停止する。

アプリの表示バージョンは、作業ツリーに`SPECIFICATION.md`がある場合はそこから、
無い場合（公開ソースのみのクローンなど）はルートの`VERSION`から読み取る。

現在の開発環境では、Visual Studio 2026、MSVC 19.51、Windows SDK
10.0.26100.0、CMake 4.2で動作確認している。

## 推奨: 通常のPowerShellから一括実行

プロジェクトのルートで次を実行する。

```powershell
.\tools\build.cmd
```

このスクリプトはVisual Studio C++環境を自動検出し、通常のPowerShellへ
MSVC／Windows SDK／Ninjaの環境を読み込んでから、CMake構成、ビルド、テストを
順に実行する。実行環境に`Path`と`PATH`が重複している場合も、MSBuildが失敗
しないようプロセス内で正規化する。`.cmd`ラッパーから実行するため、ユーザーの
PowerShell実行ポリシーを変更する必要はない。初回ビルドの過負荷とローカライズ
された依存関係ログの大量出力を避けるため、既定は4並列かつ英語ツール診断で
実行する。

JUCEのWindowsリソース生成ツールは日本語を含む出力パスを処理できないため、
プロジェクトパスに非ASCII文字がある場合、スクリプトはCMakeの中間Build treeを
`%LOCALAPPDATA%\MgsToneCraft\cmake-build-<configuration>`へ自動配置する。
完成した実行ファイルは従来どおりプロジェクト直下の`build`へ出力する。

通常のBuildでは現行UIを正式な`build\mgstc.exe`として生成する。従来の
旧Win32 GUI版はソースを含めて削除済みであり、JUCE版だけをビルドする。JUCEは8.0.15の公式コミット
`91ad83ae34a81e0833b1a2b0866f54846370ae53`へ固定し、初回構成時に
公式GitHubリポジトリから取得する。

### 画面のPNG目視検査

Windowsの画面キャプチャAPIに依存せず、JUCE自身にクライアント領域を描画させて
PNGを生成できる。通常のBuild後に次を実行する。

```powershell
.\tools\capture-ui.cmd
.\tools\capture-ui.cmd -Editor opll
```

既定の出力先はSCCが`build\ui-captures\scc-editor.png`、OPLLが
`build\ui-captures\opll-editor.png`。別の出力先を指定する場合:

```powershell
.\tools\capture-ui.cmd -OutputPath build\ui-captures\custom.png
```

この検査モードは`mgstc.exe --editor=<scc|opll> --capture-ui="<path>"`
を内部で使用する。SCCでは
確定波形を緑実線、未確定のプリセット候補を橙破線で重ねた検査状態を描画して、
PNG生成後に自動終了する。Windows 10で利用できない
`GraphicsCaptureSession.IsBorderRequired`は呼び出さない。

同一プロセス内の画面切替と補助ウィンドウ再利用は、次のセルフテストで
確認できる。終了コード`0`が成功、`16`が画面切替検証失敗を表す。

```powershell
$process = Start-Process -FilePath .\build\mgstc.exe `
    -ArgumentList '--verify-window-routing' -WindowStyle Hidden `
    -Wait -PassThru
$process.ExitCode
```

Releaseビルド:

```powershell
.\tools\build.cmd -Configuration Release
```

テストを省略する場合:

```powershell
.\tools\build.cmd -SkipTests
```

並列数を変更する場合:

```powershell
.\tools\build.cmd -Jobs 8
```

## Developer PowerShellでの手動実行

Visual StudioのDeveloper PowerShellを既に使用している場合は、従来どおり
個別に実行できる。

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
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
  - 1組の共有エンジン・WASAPI出力・MIDI入力を使用するアプリ本体

ネイティブアプリを起動する場合:

```powershell
.\build\mgstc.exe
```

音源エミュレータの固定コミットとライセンスは
[THIRD_PARTY.md](THIRD_PARTY.md)を参照する。

## バージョン表記

`About`タブに出るバージョンは、CMakeのconfigure時に
[SPECIFICATION.md](SPECIFICATION.md)の`文書版`行を読み取り、
`MGSTC_DOC_VERSION`として`mgstc`へ渡している。
仕様書側の文書版を上げれば次のビルドで自動的に再configureされるため、
ソース側の版数を書き換える必要はない。

## 日本語パスについて

Visual StudioとCMakeは通常、日本語を含むプロジェクトパスを扱える。
一部の自動実行環境で8.3短縮パスに関するMSBuild警告が出る場合があるが、
コンパイル結果には影響しない。
