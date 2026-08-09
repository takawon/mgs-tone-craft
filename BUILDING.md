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

> **Debug／Releaseの上書き注意:** 両構成は中間Build treeが別でも、実行可能な
> `build\mgstc.exe`を共有する。最後にビルドした構成がこのファイルを上書きするため、
> Debug検証後にそのまま起動すると、Standard変換もDebug速度で動作する。Debugは
> 診断用であり、利用者向けの変換速度評価には使わない。性能確認の最後は必ず
> `.\tools\build.cmd -Configuration Release -SkipTests`（またはテストを含むRelease）
> を実行し、ログがReleaseのBuild treeを示すことを確認してから`build\mgstc.exe`を
> 起動する。

通常のBuildでは現行UIを正式な`build\mgstc.exe`として生成する。従来の
旧Win32 GUI版はソースを含めて削除済みであり、JUCE版だけをビルドする。JUCEは8.0.15の公式コミット
`91ad83ae34a81e0833b1a2b0866f54846370ae53`へ固定し、初回構成時に
公式GitHubリポジトリから取得する。
ASIO対応もJUCE 8.0.15同梱のASIO SDKヘッダーを`JUCE_ASIO=1`で使用するため、
別途ASIO SDKをダウンロードする必要はない。本体はAGPL-3.0-only、同梱ASIO SDKは
GPLv3側の条件で使用し、配布ZIPにはSteinbergのASIO SDKライセンス全文を含める。

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

### OPLL変換探索ベンチマーク

SCC→OPLL／WAV→OPLLの探索性能はDebugビルドではなくReleaseビルドで測定する。
通常テストには壁時計による合否を含めず、明示的にベンチターゲットを有効化する。

```powershell
cmake -S . -B build-benchmark -G Ninja `
    -DCMAKE_BUILD_TYPE=Release `
    -DMGSTC_BUILD_TESTS=OFF `
    -DMGSTC_BUILD_BENCHMARKS=ON
cmake --build build-benchmark --target mgstc_wave_import_benchmark --parallel 8
.\build\mgstc_wave_import_benchmark.exe `
    --effort standard --workers 8 --runs 3 --case all
```

日本語を含むソースパスでJUCE関連ターゲットの構成を避けたい場合は、
ASCIIだけのBuild treeを`-B`へ指定する。

ベンチCLIは`--effort standard|thorough`、`--workers N`、`--runs N`、
`--case scc|wav-short|wav-2s|all`を受け付ける。`thorough`の既定実行回数は1回。
個別実行例:

```powershell
.\build\mgstc_wave_import_benchmark.exe `
    --effort standard --workers 8 --runs 3 --case wav-2s
.\build\mgstc_wave_import_benchmark.exe `
    --effort thorough --workers 8 --runs 1 --case scc
.\build\mgstc_wave_import_benchmark.exe `
    --effort thorough --workers 8 --runs 1 --case wav-2s
```

評価件数は壁時計ではなく固定予算で制限する。StandardはSCC 5,000、WAVは
代表周期5,000＋短尺2,100＋全長360。ThoroughはSCC 160,000、WAVは
代表周期48,000＋短尺48,000＋全長6,000である。

同一キー音高修正後の決定入力・8ワーカーでは、ReleaseのStandard中央値は
SCC 2,330.7ms
（hash `79b9901186d55905`）、WAV 0.25秒3,523.9ms
（hash `46ffb00daaa554f5`）、WAV 2秒5,531.1ms
（hash `2adf82269cb396a2`）。SCC→OPLLの候補評価音高が変わるため、修正前のhashとは
一致しない。Debug／Release間では同じソース・入力・ワーカー数のhashを一致させる。
構成間の速度差は候補生成、品質または決定性の変更
ではなく、コンパイラ最適化とビルド構成による。直近の低速報告では、最終Debug検証が
共有`build\mgstc.exe`を上書きしていた。
Thorough 1回ではSCC 108,040.7ms（hash `199a71aadf6e3f3a`）、
WAV 2秒122,088.9ms（hash `7ca5239c56a82f02`）を測定済み。
`最大約2分`は機械負荷に依存する目安であり、評価件数の固定予算は変更しない。
最終変更後の自動テストは83/83成功し、既存の決定性・キャンセルテストも成功した。

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
- `mgstc_wave_import_benchmark`（`MGSTC_BUILD_BENCHMARKS=ON`）
  - SCC→OPLL／WAV→OPLL探索のRelease中央値と候補数を測定する
- `mgstc_windows_audio`
  - Windows共有モード・イベント駆動WASAPI出力
- `mgstc`
  - 1組の共有エンジン・WASAPI／ASIO選択出力・MIDI入力を使用するアプリ本体
- `mgstc_stereo_sample_rate_converter_tests`
  - ASIO向け48kHz→44.1／48／96kHzステレオ変換の連続性・左右同期テスト

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
