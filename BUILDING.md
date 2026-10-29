# MGS Tone Craft ビルド手順

## まずビルドして起動する

1. Windows 10／11にVisual StudioのC++開発環境、Windows SDK、CMake 3.25以上、Ninja、Gitを用意します。[必要環境](#必要環境)に詳細があります。初回は依存ソースの取得にネットワーク接続も必要です。
2. プロジェクトルートの通常のPowerShellで、次を実行します。Visual Studioの環境設定からビルド・テストまで、ラッパーが順に実行します。

   ```powershell
   .\tools\build.cmd -Configuration Release
   ```

3. 成功したら、ビルドログに表示されたBuild treeの`mgstc.exe`を起動します。ASCII文字だけの通常のチェックアウトでは、既定の場所は`build\mgstc.exe`です。日本語を含むプロジェクトパスや`-BuildDirectory`指定時は出力先が変わるため、ログの場所を確認してください。製品VST3の出力先は[現在のターゲット](#現在のターゲット)に記載しています。

OPLL PatchSlideはDBを別途配置しなくてもビルドできます。探索機能を使う場合は、下記の手動配置を行ってください。配布物を作る場合は[共通配布物とリリース準備](#共通配布物とリリース準備)、起動に問題がある場合は[起動診断と画面キャプチャ](#起動診断と画面キャプチャ)、日本語パスや依存記録の問題は[日本語パスについて](#日本語パスについて)を参照してください。

## OPLL PatchSlide DBの手動配置

開発用のアプリでOPLL PatchSlideを使う場合は、共通の`%ProgramData%\OPLL_PatchSlide_DB\`フォルダー（通常は`C:\ProgramData\OPLL_PatchSlide_DB\`）へ、次の3ファイルを配置してください。StandaloneとVST3は同じファイルを読み取り専用で使います。ビルド時や起動時には自動コピーしません。

| ファイル | 初期版（13,950候補）のSHA-256 | 重複パッチ除外更新版（12,180候補）のSHA-256 |
|---|---|---|
| `patchslide.sqlite` | `27f6d7821d0ddd16caafd8397a603670977821823a21fee7146df410665d2793` | `5122f57eedd251f7dac2e1184ded353b1c86090343567020421bf27d7ea641b6` |
| `patchslide_search.sqlite` | `0f086f089c4373490988302031a6dd4545f441cd50ec3e3a24a6e43d41b8b3e1` | `14b17f3b5458d3887a1aa73f53eeb71b6a9cff1f01280eb7c965dc1d10f0075f` |
| `manifest.json` | `4b2c918887dec0c257811c34b8e886c323fc1697bdbf205af1e4e5b021ec2880` | `839ecd1659616dc7e8731ee9e046db10da0cdbedf8f03ef401286f0c70330a10` |

同じ12,180候補を収録したcompact版も使用できます。SHA-256は本体DB `67e3de24979e2c3fe0817653fa8fce6386ee91099b9a615d4231a7a32512d0a0`、検索DB `a7b43f1121e2355f2023488f078cdbe7c8f952d028b6602291cc4a6616e47fa1`、manifest `d912cf65a8f61fe4a1a3d5c07746525401038521cf48b3bf3cce51b7d6c7eb93` です。

3ファイルは同じセットで揃え、版を混在させないでください。

配置後はアプリを再起動するか、ライブラリ管理の「パッチスライド」タブで再読込し、候補が表示されることを確認してください。ファイルの不足・破損・組み合わせの不一致は同じタブに表示されます。その場合も、既存のライブラリ、音色編集、演奏は利用できます。

SQLiteの更新中ファイルやjournal／WALは配置せず、完成した3ファイルを使ってください。アプリの実行にPythonやDB生成ツールは不要です。

登録・保存済みの総合音色はDBなしでも利用できます。開発時は上記の手動配置を使い、インストーラー版ではセットアップが正式DBを配置します。アプリによる自動展開は行いません。

## Windowsインストーラーのローカル生成（Phase 1）

[Inno Setup 6.7.3](https://jrsoftware.org/isdl.php)を導入し、両製品をReleaseでビルドします。配布定義で固定した版とコンパイラーのSHAを照合するため、別の版では生成できません。コンパイラをPATHへ追加するか、生成時に`-IsccPath`で`ISCC.exe`を指定します。

日英対応はInno Setup 6.7.3で検証します。標準メッセージは、公式6.7.3配布物の`Default.isl`と`Languages/Japanese.isl`を変更せず`installer/languages/`へ固定しています。ビルドPCの翻訳ファイルへ依存せず、生成入口で両ファイルのSHA-256を検査します。追加ダウンロードや手動配置は不要です。出典と利用条件は`THIRD_PARTY.md`、同フォルダの`Inno-Setup-LICENSE.txt`に記録しています。

起動時に言語選択を表示し、WindowsのUI言語が日本語なら日本語、英語なら英語、それ以外なら英語を初期選択します。更新では前回の言語を可能な限り引き継ぎ、手動変更もできます。アンインストーラーは導入時の選択言語を使用します。独自画面・警告・補助スクリプトの固定メッセージは`[CustomMessages]`から取得します。OS/.NET由来の詳細診断、製品名、ファイル名、保存先は翻訳しません。更新時に別名のショートカットを増やさないため、既存ショートカットのファイル名は維持し、アンインストールの説明tooltipを翻訳します。今後のUI追加時も日英のメッセージと同じ置換引数を必ず用意します。

```powershell
.\tools\build.cmd -Configuration Release
.\tools\build-installer.ps1 -DbSourceDirectory '<正式compact版の引渡しフォルダ>'
```

`-BuildDirectory`（既定`build`）、`-OutputDirectory`（既定`dist`）を指定できます。`-ValidateOnly`は生成せず前提素材を検査します。ソース定義は`installer/distribution.json`へ集約し、セットアップと手動ZIPが同じ正式素材を使います。コンパイラーはInno Setup 6.7.3本体と必要な補助ファイルのSHAを固定しています。

## 共通配布物とリリース準備

通常のリリース準備には`tools/prepare-release.ps1`を使います。StandaloneとVST3をRelease構成でビルドして全回帰テストを実行し、共通の配布素材から日英セットアップ、製品VST3・正式DB同梱ZIP、チェックサム、Notesを生成します。この処理ではタグ作成、アップロード、公開は行いません。DBの正式引渡し元は引数で指定し、ProgramDataから配布用ファイルを収集しないでください。

```powershell
.\tools\prepare-release.ps1 -PreviousTag '<前回公開タグ>' -PreviousCommit '<実SHA40桁>' `
  -DbSourceDirectory '<正式DB引渡しフォルダー>' -IsccPath '<6.7.3のISCC.exe>' -Prototype
```

初回は通常ビルドで依存ソースを取得しておきます。Prepareは既存の`build/_deps`を指定して通信なしにビルドし、未取得なら停止します。未コミット作業での試作には`-Prototype`を使い、公開へ進めません。公開候補には、確定コミットと一致するソース・文書・画像と、そのdigestに対する人手レビューが必要です。

個別のZIP生成入口は次のとおりです。既存の`Configuration`／`Version`指定は維持しつつ、Release以外や実製品と違う版は拒否します。出力が既存なら新しいフォルダーを指定します。

```powershell
.\tools\package-release.ps1 -DbSourceDirectory '<正式DB引渡しフォルダー>' `
  -IsccPath '<6.7.3のISCC.exe>' -OutputDirectory '<新規出力フォルダー>'
```

生成物には、日本語の`INSTALL.txt`、英語の`INSTALL.en.txt`、READMEからたどる公開ガイドと画像、必要なライセンスを含めます。導入ガイドは両方ともUTF-8 BOM付き・CRLFで保存してください。導入先、前提環境、更新・削除の注意を揃え、手順を変更するときは日英両方を同時に更新します。

生成前、実ZIPの再展開後、公開ゲートで、両ファイルの存在と共通記載事項を検証します。文章の意味が一致するかは、別途文書レビューで確認します。EXEとZIPは同じ`payload/`から生成でき、ZIPは再展開して全ファイルのSHAを照合します。正式ビルド証跡ともEXE・製品VST3フォルダー内の全ファイルのSHAを照合し、診断版、余分なファイル、入力の変更を拒否します。

運用コマンド、レビュー記録、Stage→draft確認→別承認→Publish、GitHub Environmentの手動設定、失敗時の手順は[リリース運用ガイド](docs/RELEASING.md)を参照してください。Actionsは手動起動専用で既存draftを扱い、クラウド上でDB取得や別のバイナリ生成を行いません。workflowの運用開始には別途承認されたcommit／pushとEnvironment設定が必要です。

前提素材は現行`VERSION`と一致するReleaseの`mgstc.exe`、製品`MGS Tone Craft.vst3`バンドル全体、12,180候補の正式compact DB3ファイル、製品文書および依存ライセンスです。DBは実行時のProgramDataから収集せず、正式引渡しフォルダを明示します。定義内の3SHA、manifestの件数・版・DBへの参照、x64形式、製品VST3 ID、バージョン、Release構成を検査し、不足・不一致なら失敗します。製品版`0.xxx`はWindows/VST3メタデータに`0.xxx.0`として記録します。

出力は`dist/MGSToneCraft-<VERSION>-win64-setup.exe`と、同梱ファイルのSHA-256・件数・セットアップSHAを記録した`.exe.payload.json`です。生成は既存Release成果物を再利用し、自動ビルド・署名・公開はしません。

製品の発行元・VST3メーカー名は`Takawo.`です。EXE／VST3の会社情報、VST3のFactory／ClassのVendor、セットアップの会社情報を生成入口で照合します。更新したインストーラーを適用すると「プログラムと機能」の発行元へ反映されます。DAWに古いメーカー名が残る場合はプラグインを再スキャンしてください。

セットアップは管理者権限でStandaloneをProgram Filesの`MgsToneCraft`、任意VST3を64-bit Common Filesの`VST3/MGS Tone Craft.vst3`、必須DBをProgramDataの`OPLL_PatchSlide_DB`へ配置します。VST3は新規時OFF、更新時は前回選択を引き継ぎます。選択を外すと既知の製品VST3ファイルを除去し、未知ファイルは保持します。

DBの更新前にはMGSTC・DAW・他のDB利用アプリを終了してください。3ファイルの排他取得・退避・セット置換を行い、失敗は復元または明確なエラーにします。SQLite sidecarや中断トランザクションのmarkerがあれば更新を拒否します。中断時はmarkerに記録された退避先を保全して復旧を確認してください。再起動待ちの強制上書きはしません。

アンインストールでは「ユーザーデータ削除」OFF、「共有DB削除」ONの独立チェックを表示します。削除時には対象と警告を再確認します。ユーザーデータの現行保存先は`%LOCALAPPDATA%/MgsToneCraft`です。現セッションのWindowsユーザーを特定できなければユーザー削除を無効にします。削除対象は同フォルダの`tone_library.sqlite`、そのSQLite sidecar、`settings-v1.ini`のみです。未知ファイル、バックアップ、他ユーザーは保持します。通常の導入・更新は既存のユーザーデータへ触れません。独立選択を確実に確認するため、無人アンインストールは受け付けません。

配布前は隔離Windows環境で新規・上書き・再導入、VST3選択変更、使用中DBの更新拒否、アンインストール4通り、標準ユーザー／別管理者のUACを試験します。導入・更新前後のユーザーDB／設定SHA、導入後DB3SHA、未知ファイル保持、Start Menu／登録の重複、実DAWでの認識・開閉も確認してください。生成成功や自動回帰はこれらの実機試験の代わりにはなりません。

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

公開ソースのアプリ表示バージョンは、ルートの`VERSION`から読み取る。開発用の版指定を併用する場合も、配布時は同じ版に揃える。

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

通常のビルドでは、現行UIのアプリを`build\mgstc.exe`に生成する。従来の
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

READMEと[操作ガイド](docs/USER_GUIDE.md)、[画面一覧](docs/screenshots/README.md)の公開画像は、撮影後に目視で現行UI・文字・個人情報を確認します。`library-patchslide`と`library-export`も撮影でき、撮影時だけ自作のサンプル音色／セットを表示します。設定のMIDI／ASIO欄は固定の例示表示へ置き換え、実機材名を画像へ出しません。通常起動や保存データは変更しません。PatchSlide撮影は読み取り専用DBの検索結果を最大15秒待つため、結果が出たか画像で確認してください。全撮影対象と再撮影コマンドは画面一覧にまとめています。

共有配置を変更せず撮影する場合は`-PatchSlideDirectory '<DB3ファイルのフォルダー>'`を指定できます。この指定は撮影プロセスだけに適用され、通常起動の参照先・DB自動配置・DB内容は変更しません。PatchSlide browserの回帰テストにも`MGSTC_PATCHSLIDE_TEST_DB_DIR`で既存DBフォルダーを指定でき、未指定なら従来どおり共有配置を検証します。

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
  - 48 kHz内部エンジンのVST3 Instrument（Layer Start Delayを含む）。ホストの44.1／48／88.2／96 kHzに対応する。`mgstc_engine` と `juce_audio_processors` のみ。WASAPI／ASIO／Windows MIDI は開かない。ビルド後にシステムへ自動コピーしない（`COPY_PLUGIN_AFTER_BUILD` は FALSE）
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

DAWのプラグインパスへこのVST3フォルダーを追加するか、ホストのスキャン対象に含める。製品VST3はホストの44.1／48／88.2／96 kHzに対応する。48 kHz以外では、内部の48 kHzエンジンからホストのレートへ変換する。

Cubase 非48 kHz フリーズ隔離（Stage D.2）の診断 VST3 は通常の `mgstc_vst3` とは別ターゲット `mgstc_vst3_diag` である。既定は `OFF`（診断ターゲットを作らない）。CMake キャッシュ `MGSTC_VST3_DIAG_MODE` で `OFF` / `NON48_NULL` / `NON48_NULL_LAT0` / `NON48_ENGINE_ONLY` / `NON48_SRC_ZERO` / `NORMAL_NO_LATENCY_NOTIFY` を選ぶ。通常 Release の `mgstc_vst3` は常に診断 OFF のまま。診断 bundle の典型パス:

```text
<build-tree>\mgstc_vst3_diag_artefacts\<Config>\VST3\MGS Tone Craft.vst3
```

Plugin Code は製品と同じ `Mgti` なので、Cubase には診断 bundle だけを追加する。プラグイン破棄時に `%LOCALAPPDATA%\MgsToneCraft\vst3-d2-diag.txt` へカウンタを書く。Audio Thread ではログしない。

音源エミュレータの固定コミットとライセンスは
[THIRD_PARTY.md](THIRD_PARTY.md)を参照する。

## バージョン表記

公開ソースでは、CMakeのconfigure時に`VERSION`を読み取り、`MGSTC_DOC_VERSION`として製品へ渡す。表示版、ファイル情報、配布物の版を一致させ、ソース内の表示文字列を個別に書き換えない。

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

### 起動診断と画面キャプチャ

`mgstc --verify-startup` は、私有の一時設定・ライブラリで通常の初期化とnativeウィンドウ生成を行い、初回描画とデバイス隔離を確認して終了する。CTestの `mgstc_startup_smoke_tests` からも実行する。作業ディレクトリの `startup-verification.txt` に結果を残す。PC Audio、実MIDI、MAmidiMEmo接続は開始しない。

`--capture-ui`も設定とライブラリを専用の一時フォルダーに隔離し、外部デバイスへ接続しない。利用者の音色や設定を撮影するのではなく、既定状態の診断用画像を生成する。自動回帰テスト、ネイティブウィンドウの起動診断、実デバイス・DAW・聴感の確認は、それぞれ別の検証として扱う。
