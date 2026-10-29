# MGS Tone Craft

**OPLL・SCC・PSGを重ねて、MSX／MGSDRV向けの複合音色を作るWindows用音色エディタです。**

MGS Tone Craft（MGSTC）は、MSXやMGSDRVで使う音色を、音を聴きながら作るためのアプリです。OPLLのパラメーターやSCCの波形を編集するほか、複数の音源を重ねて、音量や音程の変化をひとつの時間軸で調整できます。作った音色は、MGSCで使う定義ソースとして出力できます。

![開発版0.365の総合音色エディタ。複数音源のレイヤーと共通の時間軸で編集](docs/screenshots/main.png)

**このREADMEと画像はv0.365の機能・操作を説明しています。** 配布版ごとの公開状況と内容は[GitHub Releases](https://github.com/takawon/mgs-tone-craft/releases)を確認してください。Alphaのため操作や画面は今後変わります。ユーザー音色ライブラリSQLiteの後方互換方針は[保存データの互換性](#保存データの互換性)を参照してください。

<!-- mgstc:download:start -->
## ダウンロードと導入

[GitHub Releases一覧](https://github.com/takawon/mgs-tone-craft/releases)で対象版のRelease Notesと添付ファイルを確認し、使う版に合った配布物を選んでください。Alphaのpre-releaseも一覧に含まれます。ここに記載した構成のすべてが各過去版に提供されるとは限りません。

| 配布形式 | 選び方・導入 |
|---|---|
| Windowsインストーラー（EXE） | `MGSToneCraft-<VERSION>-win64-setup.exe`。通常はこちらを選び、案内に従って導入します |
| Windows手動導入ZIP | `MGSToneCraft-<VERSION>-win64.zip`。全体を展開し、同梱の`INSTALL.txt`（日本語）または`INSTALL.en.txt`（英語）に従って配置します |
| ソースZIP／tar.gz | GitHubがタグから提供するSource codeアーカイブ。実行ファイルの配布ZIPとは別で、[BUILDING.md](BUILDING.md)に従うビルドが必要です |

インストーラーでは、**MGS Tone Craft本体を必ずインストールし、VST3プラグインは必要な場合だけ選択します（新規インストール時はOFF）**。表示言語は日本語・英語から選べます。更新時は前回のコンポーネント選択を引き継ぎます。

OPLL PatchSlide用の共有データは、3ファイルを一組として`%ProgramData%\OPLL_PatchSlide_DB\`へ配置します。ユーザーが作った音色は`%LOCALAPPDATA%\MgsToneCraft\tone_library.sqlite`に保存されます。ユーザー音色・設定は配布物に含めず、通常の導入・更新で削除しません。

**旧版から更新する前に、音色と設定をバックアップしてください。** v0.285の旧独自形式ライブラリ（`timbre-library-v1.mgstc`、`composite-timbre-library-v1.mgstc`）は、v0.365で直接読み込めず、自動移行もされません。バックアップしただけで新版で使えるようになるわけではありません。元ファイルを保管し、対応する移行方法を確認してください。SQLite形式のライブラリとは互換性の扱いが異なります。[保存データの互換性](#保存データの互換性)も確認してください。

アンインストール時は、ユーザーデータと共有DBを削除するか、それぞれ選べます。初期値は**ユーザーデータ削除がOFF、共有DB削除がON**です。共有DBを他のアプリでも使っている場合は、削除をOFFにしてください。

手動ZIPにはアンインストーラーがありません。配置・更新・削除の手順は、同梱の`INSTALL.txt`（日本語）／`INSTALL.en.txt`（英語）を参照してください。外部ランタイム・ASIOドライバー・MAmidiMEmo本体は同梱しません。配布物を作る方は[BUILDING.md](BUILDING.md)を参照してください。
<!-- mgstc:download:end -->

## 主な機能

### 複合音色とWAV変換

- PSG（YM2149）／SCC／OPLL（YM2413）のレイヤーを共通時間軸で編集・試聴。
- 音量・音程・休符・ループ、ソフトウェアエンベロープ`@e`／`@r`、LFO、SCCモーフなどを組み合わせた音作り。
- WAVから複合音色や単音色への近似変換。候補を試聴・比較してから適用できます。元のWAVと同じ音の再現を保証する機能ではありません。
- MGSCの`@s`／`@v`／`@e`／`@r`と演奏設定を含むソースの確認・コピー・入出力。曲全体の完成した`.MUS`を書き出す機能とは別です。

### OPLLとSCCの単音色編集

OPLLのMOD／CARパラメーターとエンベロープを編集し、SCCでは32サンプルの波形を描画・数値編集できます。プリセット、候補のA/B比較、波形加工、単音色の近似変換にも対応します。

![OPLLのパラメーターとエンベロープを編集する画面](docs/screenshots/opll-editor.png)

![SCCの32サンプル波形と数値入力・補間を編集する画面](docs/screenshots/scc-editor.png)

SCCの数値入力は、16～256段階の分解能、5種類の補間曲線、括弧による時間配分、`SCCWAV`形式の共有に対応します。例と制限は[数値波形入力ガイド](docs/SCC_NUMERIC_WAVE.md)へ。

### 音色ライブラリ、セット、Furnace入出力

**音色一覧／パッチスライド／タグ管理／インポート／エクスポート**の5タブから音色を整理します。94種類の標準タグと独自タグ、★、検索・並べ替え、複数のセットへの所属に対応します。セットの所属解除・削除と、音色本体の削除は別の操作です。

![音色一覧とセットを管理する開発版のライブラリ画面](docs/screenshots/library-manager.png)

非圧縮MGS、MML／MUS、MuSICA VCD、SCC-Musixx SNG、VGM／VGZなどから完成音色を候補として取り込み、試聴してから登録できます。Furnaceの`.fui`／`.fuw`とMGSTCの`.mgstc`の入出力も扱います。対応する音源やマクロに制限があり、Furnaceへの複合音色出力は静的な音色設定のみの部分変換です。警告・変換結果を確認してください。

### OPLL PatchSlideを探索

OPLL PatchSlideには**12,180の候補**があり、音を聴き比べながら音色を探せます。タグのAND／OR検索や16群・64群の分類で候補を絞り、`Standard B`で似ている順に並べたり、`Variation`で変化を探したりできます。気になる候補は一時ストックへ置き、総合音色への適用やライブラリへの登録に使えます。

![パッチスライドの分類・候補一覧・一時ストックを使う探索画面](docs/screenshots/patchslide.png)

12,180はDB全体の件数であり、全候補の安全性・音質を保証する数ではありません。`Rejectも表示`は既定OFFです。一時ストックはアプリ／プラグインインスタンスの終了を越えて保存されません。DBが未配置・不整合でも、既存の音色編集は利用できます。DB3ファイルの配置と対応セットは[BUILDING.md](BUILDING.md)を参照してください。

### 試聴、解析、実機出力

画面鍵盤、PCキー、MIDIで試聴し、スペクトログラム／スペクトラムで音を確認できます。履歴、ピーク情報、比較保持、音源別の色分けを使えます。スペアナ画面の操作中も、直前に使っていたエディタへ鍵盤入力を送れます。

![試聴音の周波数成分と履歴を表示するスペクトラム](docs/screenshots/spectrum.png)

StandaloneではWindows音声出力（WASAPI／ASIO）を選択でき、対応機材・設定があればMAmi-VSIFやMAmidiMEmo経由の実機出力も使えます。外部ドライバー、MAmidiMEmo本体、機材は同梱しません。Audacity連携は任意で、通常のWAVファイル変換には不要です。

### StandaloneとVST3

VST3対応DAWでも、総合音色エディタ、レイヤーごとの音色編集、ライブラリ、PatchSlide、解析画面を使えます。総合音色とマスターボリュームは、プラグインごとにDAWのプロジェクトへ保存・復元できます（Plugin State）。プロジェクトに保存した音色は、ライブラリの登録内容とは独立して復元できます。

| 項目 | Standalone | VST3 |
|---|---|---|
| 音声・MIDIの管理 | アプリのWindowsデバイス設定 | DAW |
| SCC／OPLL編集 | 独立した単音色画面と総合のレイヤー編集 | 総合音色に属するレイヤー編集 |
| 音声出力 | PC音声／対応する実機経路 | stereo出力、音声入力・MIDI出力なし |
| 設定・状態 | アプリ設定、ライブラリ、ポータブル音色 | インスタンス単位のPlugin State。Standaloneのデバイス設定は共有しません |

VST3のホストサンプルレートは44.1／48／88.2／96 kHzに対応し、48 kHz以外は内部の48 kHzエンジンとの間で変換します。ホストパラメーター自動化とプリセットブラウザは未対応です。全DAW・実機・組合せでの実動作を保証するものではありません。

<!-- mgstc:whats-new:start -->
## What's New — v0.365

v0.365で追加・変更した機能です。比較元は[前回の有効なWindows配布版 v0.285](https://github.com/takawon/mgs-tone-craft/releases/tag/v0.285)です。対象版の配布内容と検証範囲はReleases一覧のRelease Notesを参照してください。

1. **DAWでも音色を編集** — VST3プラグインに対応しました。総合音色の編集・解析に加え、音色と音量をDAWのプロジェクトへ保存・復元できます。対応するサンプルレートでは内部エンジンとの変換も行います。
2. **WAVを音作りの出発点に** — WAVから複合音色への変換候補を試聴・比較できます。SCCモーフは区間ごとの変化を編集できます。変換は元の音を完全に再現するものではありません。
3. **音色をセットで整理** — 5タブのライブラリ画面で、セットへの複数所属、94種類の標準タグ、Furnace形式の入出力を使えます。
4. **OPLL PatchSlideで音色を探索** — 12,180候補を分類・類似検索し、試聴して一時ストックに追加できます。ストックはアプリ／プラグインの終了時に消えます。候補を総合音色へ適用することもできます。
5. **数値と式でSCC波形を作成** — SCCWAV数値入力に、分解能の選択、5種類の補間、時間配分、共有用の形式を追加しました。[入力例](docs/SCC_NUMERIC_WAVE.md)を参照してください。
6. **日英の導入案内と保存データの注意** — 日本語・英語のインストーラーと手動導入ガイドを用意しました。発行元表記は`Takawo.`です。SQLiteライブラリの互換方針と、旧独自形式ライブラリが自動移行されないことを案内しています。

詳細は[CHANGELOG](CHANGELOG.md)と[Release Notes](https://github.com/takawon/mgs-tone-craft/releases)へ。
<!-- mgstc:whats-new:end -->

## 最初の使い方

以下はv0.365の操作です。使用する版の説明・画面を確認してください。

1. 起動した総合画面で`+ チャンネル`を押し、`PSG 追加`／`SCC 追加`／`OPLL 追加`から使う音源を選びます。
2. `初期設定 / 編集`と`カウント編集`で音色・エンベロープ・音程変化を編集します。
3. 画面鍵盤、PCの`Z S X D C V …`、MIDIで試聴。`スペアナ`で音の変化を確認できます。
4. `保存 / ライブラリ`から音色を保存し、MGSCソースをコピーまたは書き出して、自分のMMLに組み込みます。

画面ごとの手順、鍵盤配列、セット、インポート・エクスポート、設定は[操作ガイド](docs/USER_GUIDE.md)へ。[画面一覧](docs/screenshots/README.md)には設定や管理タブも掲載しています。

## 動作環境と制限

- Windows 10／11、64-bit。VST3にはVST3対応DAWが必要です。
- StandaloneのASIO出力には対応ドライバーが必要です。環境によっては[Microsoft Visual C++ Redistributable](https://learn.microsoft.com/ja-jp/cpp/windows/latest-supported-vc-redist)の導入が必要です。
- MIDI機器、Audacity、実機出力機材は任意です。Audacity連携の準備と注意は[操作ガイド](docs/USER_GUIDE.md)を参照してください。
- 圧縮MGSはインポートしません。外部ファイルの任意の断片や曲全体を、そのままMGSTCの編集モデルへ復元する機能ではありません。
- Alphaであり、機器・DAWでの確認範囲とコード／自動テストで確認した範囲は区別しています。

### 保存データの互換性

**ユーザー音色ライブラリ`tone_library.sqlite`は、Alphaを含め、保存データの意味を保った読込・段階的な移行を保証する方針です。** Schema v1を起点とし、現行はSchema v2です。保存済みの音色・セット・タグ・メタデータを対象とし、旧DBにない追加値は未設定として扱い、必要な既定値や値の変換は個別仕様に従います。

この方針はSQLite形式の音色ライブラリが対象です。Schema v1からv2へは既存データを保って移行し、移行に失敗した場合も元のデータベースを保護します。

v0.285の旧独自形式ライブラリ（`timbre-library-v1.mgstc`、`composite-timbre-library-v1.mgstc`）は、v0.365では直接読み込めず、自動移行もしません。更新前に旧ファイルをバックアップし、対応する移行方法を確認してください。保管は元ファイルを失わないためのもので、新版での利用を保証するものではありません。

新しいDBを古いMGSTCで読む前方互換性、設定INI・交換／ポータブル`.mgstc`のAlpha版間互換は保証しません。また、音源エンジンや内部演算が変わった場合、出音や波形が以前と完全に一致するとは限りません。音色DBのバックアップに加え、必要に応じてMGSC定義テキストも控えておくことを推奨します。

## ドキュメント・開発・ライセンス

- [操作ガイド](docs/USER_GUIDE.md)／[SCC数値波形入力](docs/SCC_NUMERIC_WAVE.md)／[画面一覧](docs/screenshots/README.md)
- [更新履歴](CHANGELOG.md)／[GitHub Releases](https://github.com/takawon/mgs-tone-craft/releases)／[ビルド手順](BUILDING.md)／[リリース運用](docs/RELEASING.md)
- 発行元・メーカー名：**Takawo.**　製品ライセンス：**AGPL-3.0-only**（[LICENSE](LICENSE)）
- JUCE、音源エミュレータ、SQLite、rpclib、Inno Setup標準翻訳等の条件は[THIRD_PARTY.md](THIRD_PARTY.md)へ。外部ツール本体の同梱を意味しません。
- 不具合・要望は[GitHub Issues](https://github.com/takawon/mgs-tone-craft/issues)へ。使用版、Standalone／VST3、再現手順を添えてください。音色DBや個人情報を無断で公開しないようご注意ください。

本プロジェクトは[OpenAI Codex](https://openai.com/codex/)と[Cursor](https://cursor.com/)による対話的なAI支援開発を利用しています。利用ライブラリ・公開参考資料の一覧は[操作ガイドの付録](docs/USER_GUIDE.md#参考資料)に整理しています。
