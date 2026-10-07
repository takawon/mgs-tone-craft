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

ルートの `build.cmd` も同じスクリプトへ委譲する。

> **注意:** 通常の PowerShell から `cmake --build build` だけを実行しない。
> Visual Studio の Developer 環境（`INCLUDE` 等）が無いと、標準ヘッダ未検出の C1083 になる。
> CMake 構成時と、Ninja がソースをコンパイルする前に環境チェックがあり、未設定なら上記の正しい手順を案内して停止する。
> 同じ作業ツリーで `.\tools\build.cmd` を二重起動しない。作業ツリーとビルド先を排他ロックする。

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
実行ファイルもそのBuild treeへ出力する。ASCIIの通常チェックアウトでは既定の
Build tree／exe出力先は従来どおり`build`である。`-BuildDirectory`を指定した場合は
exeとCTestも指定先を使用し、別ツリーのexeが上書きされて誤検証されることを防ぐ。

> **Debug／Releaseの上書き注意:** 同じBuild treeを指定すると最後の構成がexeを
> 上書きする。構成ごとに異なる`-BuildDirectory`を使うとexeも分離される。
> 既定の`build`でDebug検証後にそのまま起動すると、
> Standard変換もDebug速度で動作する。Debugは
> 診断用であり、利用者向けの変換速度評価には使わない。性能確認の最後は必ず
> `.\tools\build.cmd -Configuration Release -SkipTests`（またはテストを含むRelease）
> を実行し、ログがReleaseのBuild treeを示すことを確認してから`build\mgstc.exe`を
> 起動する。

通常のBuildでは現行UIを正式な`build\mgstc.exe`として生成する。従来の
旧Win32 GUI版はソースを含めて削除済みであり、JUCE版だけをビルドする。JUCEは8.0.15の公式コミット
`91ad83ae34a81e0833b1a2b0866f54846370ae53`へ固定し、構成時に
公式GitHubリポジトリから取得する。
`third_party/patches/juce/direct2d.patch`（JUCE Forum / reuk、Direct2D
`Present1` の Message Thread blocking 対策）を JUCE ソースへ適用する。
JUCE 8.0.12 Windows painting overlay は使わない。すでに取得済みの
`build/_deps/juce-src` がある場合も、8.0.15 の
`Windowing` / Direct2D 3ファイルとpatchの内容をハッシュで確認する。
変更時だけstock復元とpatch適用を行い、内容が同じ場合はタイムスタンプを保持する。
Standalone の画面位置は 8.0.15 `Windowing` の `logicalBounds` /
`userBounds` に依存する（overlay 用の Displays 書き換えは持ち込まない）。
上流に同等修正が入ったらこの patch を削除する。
ASIO対応もJUCE 8.0.15同梱のASIO SDKヘッダーを`JUCE_ASIO=1`で使用するため、
別途ASIO SDKをダウンロードする必要はない。本体はAGPL-3.0-only、同梱ASIO SDKは
GPLv3側の条件で使用し、配布ZIPにはSteinbergのASIO SDKライセンス全文を含める。

### 増分ビルドと最終検証

Debugの既定は`/Z7`（OBJ内のデバッグ情報）。`/FS`付きでも発生したコンパイラPDB
競合を避ける。リンクPDBは維持する。Releaseの最適化・製品動作は変更しない。
ヘッダ更新はNinjaの依存関係で増分処理し、毎回全体cleanしない。
依存関係が疑わしい場合は、まず次で記録を確認する。

```powershell
.\tools\build.cmd -PlanOnly -DependencyReport
.\tools\build.cmd -TestRegex '"mgstc_(vst3_processor|layer_delay_scheduler)_tests"'
.\tools\build.cmd -Target mgstc_vst3 -SkipTests
```

`.cmd`から複数Targetを渡す場合はPowerShell配列構文に頼らず、単一Targetで呼び出すか
既定の全ターゲットビルドを使う。`-Target`は`-SkipTests`または`-PlanOnly`と併用する。
PowerShellから`.cmd`へ`|`などのcmdメタ文字を含む正規表現を渡す場合は、上の例のように
文字列内へ二重引用符を残す。外側のPowerShell引用符だけではcmdがパイプとして解釈する。
テスト時は全前提ターゲットをビルドし、古いテストexeによるPASSを防ぐ。
`-PlanOnly`は製品ターゲットのコンパイル／CTestを実行しないが、CMake構成に必要な
JUCEの`juceaide`をビルドする場合がある。Ninja dry-runの工程数は再構成予定を含む概算。
依存記録はBuild treeの`.mgstc-ninja-deps.txt`に保存する。

工程別の固定費を調べる場合は `-Timings` を付ける。通常の環境初期化・排他・
configure・ビルド・テスト・最終ソース照合を維持し、Build treeへ
`.mgstc-timing-<PID>.json`、`.mgstc-cmake-profile.json`を保存する。
Ninjaの依存走査統計もログへ出す。CMake profileの子工程はconfigure時間の内訳であり、
合計へ重複加算しない。compile/linkはビルドログとNinja実行記録で区別する。
テストを実行しない基準測定には `-SkipTests` を明示し、テストありの検証とは区別する。
`build.cmd`起動前後を呼出し側のStopwatchでも測ると、PowerShell起動・終了を含む全体時間になる。
同条件の比較では構成・tree・Jobs・テスト選択・計測有無を揃える。

```powershell
.\tools\build.cmd -BuildDirectory build-debug -Configuration Debug -Jobs 4 -SkipTests -Timings
.\tools\build.cmd -BuildDirectory build-debug -Configuration Debug -Jobs 4 -Timings
```

実装・テスト・レビュー修正を完了してから最終検証を実行する。
スクリプトはHEAD、未コミット／未追跡ソース、ローカルfixture、ビルド設定、前後SHA256を
`.mgstc-validation.json`へ記録し、検証中に内容または更新時刻が変われば成功扱いしない。
内容を変更後に戻す通常の編集も更新時刻で検出する。意図的な時刻復元や外部依存cacheの
一時変更までを監視する仕組みではない。
生成先・依存キャッシュ・内部の作業記録はソースハッシュ対象外。失敗時は
`FailedOrIncomplete`、`-SkipTests`は`BuildPassedTestsNotRun`として区別する。
証跡は各実行で更新するため、最終結果として残すものはローカルの監査／ログ先へコピーする。
環境・外部依存の同一性や実UI・DAW・聴感をこのハッシュだけで保証したとは扱わない。
実行中のMGSTCバイナリは強制終了せず、PIDとパスを表示して停止する。

長いビルドではログを保存し、末尾・新規エラー・終了コードを確認する。
同じ全文や同じ進捗を繰り返し取得せず、ビルドは統合者一人が実行する。
独立レビューは対象差分を固定して読み取りで行い、後続修正を影響範囲だけ再確認する。

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
.\tools\build.cmd -Configuration Release -BuildDirectory build-benchmark `
    -BuildBenchmarks -Target mgstc_wave_import_benchmark -Jobs 8 -SkipTests
.\build-benchmark\mgstc_wave_import_benchmark.exe `
    --effort standard --workers 8 --runs 3 --case all
```

日本語を含むソースパスでJUCE関連ターゲットの構成を避けたい場合は、
ASCIIだけのBuild treeを`-BuildDirectory`へ指定する。

ベンチCLIは`--effort standard|thorough`、`--workers N`、`--runs N`、
`--case scc|wav-short|wav-2s|all`を受け付ける。`thorough`の既定実行回数は1回。
個別実行例:

```powershell
.\build-benchmark\mgstc_wave_import_benchmark.exe `
    --effort standard --workers 8 --runs 3 --case wav-2s
.\build-benchmark\mgstc_wave_import_benchmark.exe `
    --effort thorough --workers 8 --runs 1 --case scc
.\build-benchmark\mgstc_wave_import_benchmark.exe `
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

変更箇所の回帰を先に確認する場合は、同じラッパーへCTest名の正規表現を渡せる。
指定を省略すると全試験を実行し、指定した試験が存在しない場合は失敗する。

```powershell
.\tools\build.cmd -Configuration Release -TestRegex "composite_wav"
```

並列数を変更する場合:

```powershell
.\tools\build.cmd -Jobs 8
```

## Developer PowerShellでの実行

Visual StudioのDeveloper PowerShellでも同じラッパーを使用する。
環境チェックだけでなく排他・増分ビルド・ソース照合の手順を維持する。

```powershell
.\tools\build.cmd -Configuration Debug
```

## 現在のターゲット

- `mgstc_engine`
  - UI非依存のC++20静的ライブラリ
- `emu2149` / `emu2212` / `emu2413`
  - `third_party`へ固定した音源エミュレータ
- `sqlite3`
  - `third_party/sqlite`へ固定した SQLite amalgamation（音色ライブラリ）
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
- `mgstc_vst3`
  - 48kHz VST3 Instrument（Layer Start Delay 含む）。`mgstc_engine` と `juce_audio_processors` のみ。WASAPI／ASIO／Windows MIDI は開かない。ビルド後にシステムへ自動コピーしない（`COPY_PLUGIN_AFTER_BUILD` は FALSE）
- `mgstc_vst3_diag`
  - Stage D.2 診断 VST3。製品 `mgstc_vst3` とは別成果物。`MGSTC_VST3_DIAG_MODE` で経路だけを切り替える（既定 `OFF`）。製品機能ではない
- `mgstc_realtime_engine_host_tests` / `mgstc_layer_delay_scheduler_tests` / `mgstc_vst3_processor_tests` / `mgstc_vst3_diag_null_tests` / `mgstc_vst3_diag_lat0_tests`
  - Audio Thread 直接 Note API、遅延 frame 変換と cancel、MIDI sample offset、block 跨ぎ、複数 instance、非48kHz 無音化
- `mgstc_stereo_sample_rate_converter_tests`
  - ASIO向け48kHz→44.1／48／96kHzステレオ変換の連続性・左右同期テスト

ネイティブアプリを起動する場合:

```powershell
.\build\mgstc.exe
```

VST3 bundle は JUCE artefacts 配下に生成される（構成ディレクトリは `tools\build.cmd` が選ぶ中間 Build tree）。典型例:

```text
<build-tree>\mgstc_vst3_artefacts\<Config>\VST3\MGS Tone Craft.vst3
```

DAW のプラグインパスへこの bundle を追加するか、ホストのスキャン対象に含める。Stage C は 48,000 Hz のみ発音する。

Cubase 非48 kHz フリーズ隔離（Stage D.2）の診断 VST3 は通常の `mgstc_vst3` とは別ターゲット `mgstc_vst3_diag` である。既定は `OFF`（診断ターゲットを作らない）。CMake キャッシュ `MGSTC_VST3_DIAG_MODE` で `OFF` / `NON48_NULL` / `NON48_NULL_LAT0` / `NON48_ENGINE_ONLY` / `NON48_SRC_ZERO` / `NORMAL_NO_LATENCY_NOTIFY` を選ぶ。通常 Release の `mgstc_vst3` は常に診断 OFF のまま。診断 bundle の典型パス:

```text
<build-tree>\mgstc_vst3_diag_artefacts\<Config>\VST3\MGS Tone Craft.vst3
```

Plugin Code は製品と同じ `Mgti` なので、Cubase には診断 bundle だけを追加する。プラグイン破棄時に `%LOCALAPPDATA%\MgsToneCraft\vst3-d2-diag.txt` へカウンタを書く。Audio Thread ではログしない。

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

### MSVC / CMake / Ninja の依存記録と復旧

Windows wrapper は Ninja の `-t wincodepage` で使用する文字コードを取得し、
CMake のコンパイラ検出から MSVC の `/showIncludes` 出力、Ninja の依存解析まで、
コンソールの入力・出力コードページと PowerShell の出力文字コードを揃える。
終了時は元へ戻す。生成済み Ninja ファイルの書換えや、日本語 prefix の固定値は使用しない。

各ビルド後は、対象に応じた代表 OBJ の依存記録を必須検証する。
OBJ の存在、有効な依存記録、依存件数、期待するプロジェクト・依存ライブラリのヘッダを確認し、
コンパイル自体が成功していても依存が欠落していれば Fail とする。
`-SkipTests` でもこの確認は省略しない。証跡はビルド先の
`.mgstc-deps-validation.json` と `.mgstc-native-encoding.json` に保存する。
`-Target` 指定時はそのビルドグラフに含まれる代表 OBJ を確認する。
代表を含まないターゲットは NotApplicable と明示する。

既存ツリーで検出済み prefix または依存記録が壊れている場合は、次を一度実行する。

```powershell
.\tools\build.cmd -Configuration Debug -BuildDirectory build-debug -RecoverDependencies
```

復旧は CMake の `--fresh` を使う（CMake 3.24 以降、プロジェクト自体は 3.25 以降が必要）。
元の CMakeCache.txt を退避し、BOOL / STRING / PATH / FILEPATH の設定を初期 cache に保存する。
UNINITIALIZED は値を保って STRING として保存し、INTERNAL / STATIC の検出結果は保存しない。
wrapper の明示引数を初期 cache より後に指定し、今回の構成指定を優先する。
コンパイラ検出と依存記録を再生成し、復旧時だけ --clean-first でサブディレクトリの旧OBJも再生成するため完全ビルドとなり、`-Target` / `-PlanOnly` は併用できない。

新規ツリーを通信なしで検証する場合は `-DependencySourceRoot build/_deps` を指定できる。
既存の juce-src / rpclib-src のソースだけを参照し、OBJ・生成物・バイナリは新しいツリーに作る。
通常ビルドでは常時 fresh / clean を実行せず、Ninja の増分コンパイルを維持する。
