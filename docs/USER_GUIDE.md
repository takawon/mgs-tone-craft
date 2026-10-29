# MGS Tone Craft 操作ガイド（v0.365）

このガイドは MGSTC v0.365 の操作を説明します。配布版ごとの内容・公開状況は[README](../README.md)からReleases一覧を確認してください。画面や操作は今後変わる場合があります。

MGS Tone Craft は、OPLL、SCC、PSG を重ねた複合音色を作る Windows アプリです。音を聴きながら編集し、MGSC 向けの音色定義や演奏設定を書き出せます。Standalone 版と VST3 版で、共通の編集画面と音色ライブラリを使えます。

VST3 版では、音声出力と演奏用 MIDI 入力を DAW 側で設定します。アプリ内の PC 音声出力・MIDI 入力設定とは別です。総合音色とマスターボリュームは、プラグインごとに DAW のプロジェクトへ保存・復元されます（Plugin State）。保存した音色は、ライブラリとは独立して復元できます。

## 画面を開く

- **Standalone 版**：起動すると、チャンネルをまだ追加していない総合音色エディタが開きます。右上の `SCC音色`／`OPLL音色` から単音色エディタも開けます。独立した単音色画面は、閉じても編集状態を保持します。
- **VST3 版**：DAW からプラグインの編集画面を開きます。初期状態の PSG レイヤーを編集するか、必要な音源を追加してください。

各エディタの `スペアナ` から、試聴音を解析する共通の画面を開けます。`ライブラリ管理` は別ウィンドウで開くため、音色の編集画面と並べて使えます。

## 総合音色を編集する

総合音色では、PSG、SCC、OPLL のレイヤーを同じ時間軸に並べます。

1. `+ チャンネル` を開き、`PSG 追加`／`SCC 追加`／`OPLL 追加` から使う音源を選びます。概要欄の `複製` や `削除` で構成を調整できます。
2. チャンネルの `初期設定 / 編集` で、発音前の音色、音程、変調などを設定します。
3. 時間軸で音量、音程、音色切替、レジスタ変更、休符、ループを編集します。

`編集に集中` を使うと、概要と MIX 行が隠れ、編集領域を広く使えます。

音色を切り替えるには、音色（`@`）のカウントを選びます。開いた音色選択画面で、ライブラリ音色や対応する ROM 音色・SCC プリセットを試聴し、読み込んでください。そのカウントへ音色が割り当てられます。SCC／OPLL レイヤーの波形やパラメーターは、`単音色編集` から編集できます。

割り当てた音色は、総合音色の中にコピーして保持します。後から単音色ライブラリを変更しても、総合音色の内容は変わりません。

画面上部には `MGSCプレビュー`、`スペアナ`、即時発音などの操作があります。独立した `SCC音色`／`OPLL音色` ボタンはStandalone用で、VST3では表示せず総合のレイヤーを編集します。総合音色の `開く`／`保存` は編集中の総合音色全体を扱います。`MGSCソースを開く`／`MGSCソースを書き出す` は MGSC 定義ソースの入出力です。コピー／貼り付けも総合音色または MGSC ソースを扱い、曲全体の MML/MUS を生成する機能ではありません。

音量は0～15を等間隔で編集し、チップ固有の論理カーブを重ねて確認できます。「自動変化」は開始から終端までの線を設定し、MGSCのランプ定義へ変換します。

初期設定の `h` からソフトウェアLFOを編集できます。波形は約60Hzの水平保持と垂直な変化で表示し、縦軸は音程オフセット±127です。PSG／SCCの `p` と `h` は同時に使えません。

## SCC 単音色を編集する

SCC エディタでは波形グラフをドラッグして32サンプルを描き、各値を10進／16進欄で編集できます。平均化、正規化、反転、位相移動、上下反転、縦倍率などの波形操作もあります。

波形プリセットを使う場合は、倍音倍率と置換／マージ条件を調整します。確定前の候補を表示・試聴し、`A/B` で確定波形と比較してください。`適用` で確定し、`取消` で候補を破棄します。

数値入力欄は分解能を選び、最大32個の値と補間演算子を使って波形を入力します。括弧で入力値の時間配分を指定できます。有効な式は入力と同時に反映され、Undo／Redo では波形、分解能、式をまとめて戻せます。詳細は[数値波形入力ガイド](SCC_NUMERIC_WAVE.md)を参照してください。

ツールバーからは、MGSC の `@s` 定義を `.txt` ファイルで読み書きできます。貼り付けは、クリップボードにある WAV 音声を優先します。WAV がなければ画像を背景画像として読み込み、それ以外は MGSC テキストとして扱います。

> 文字コードの補足：MGSCテキストをコピーすると、UnicodeとANSI（CP932）の両形式をクリップボードへ登録します。

`WAV変換` は、WAV ファイルやクリップボードの音声から利用でき、Audacity は不要です。`Audacityから変換` を使う場合だけ、次の準備をしてください。

1. Audacity の「編集」→「環境設定」→「モジュール」を開きます。
2. `mod-script-pipe` を有効にし、Audacity を再起動します。
3. Audacity で変換したい範囲を選び、MGSTC の `Audacityから変換` を実行します。

**`mod-script-pipe` が有効な間は、他のプログラムからも Audacity を操作できます。利用しないときは無効に戻してください。**

## OPLL 単音色を編集する

OPLL エディタでは MOD／CAR の AM、PM、EG、KR、波形、MULT、KSL、TL、AR、DR、SL、RR、FB などを編集できます。オリジナル音色のエンベロープ表示や `@v` プレビューは編集内容に追従します。

固定ROM音色は15種類で、MGSC番号 `@0`～`@14` から選択して試聴できます。一般的な1始まりの音色番号とは表示が異なるため、外部資料の番号ではなく画面の `@` 番号を確認してください。ROM音色を編集するには、`編集音色へ読込` でオリジナル音色へコピーしてから編集します。

ツールバーから `.txt` の MGSC `@v` 定義を読み書きし、コピー／貼り付けや Undo／Redo を利用できます。WAV変換の結果は候補として確認してから適用します。

## 試聴、PC鍵盤、MIDI

- 画面鍵盤は `C1`～`B8`。クリック／ドラッグで発音します。
- PC鍵盤の下段は `Z S X D C V G B H N J M`（Z が基準オクターブの C）、上段は `Q 2 W 3 E R …` です。
- `Page Down`／`Page Up`（画面上は `PgDn`／`PgUp`）でPC鍵盤のオクターブを移動します。`Ctrl` を押している間は PC 演奏を止めます。
- 鍵盤上部の `Poly`／`Mono` で発音モードを選びます。テキスト入力欄にフォーカスがある間は PC 演奏を抑制します。
- Standalone の MIDI 入力デバイスはアプリ設定の `MIDI` タブで選択します。入力機器の接続状態は鍵盤付近に表示されます。VST3 では DAW からの MIDI を使います。
- スピーカーの即時発音をONにすると、対応する音色編集の変更時に最後に手動演奏した音程を約1秒試聴します。OFFでも鍵盤、PC、MIDIによる手動試聴は使えます。

### 音をスペアナで確認する

スペアナは、総合／SCC／OPLL の各エディタから開く共通のウィンドウです。`スペクトログラム` と `スペクトラム` を切り替え、音源色または疑似カラー、強調率、履歴、ピーク情報などを確認できます。

スペクトラムでは最大5秒の履歴を表示し、`比較用に保持`で基準を残した比較、表示停止と履歴位置の確認ができます。スペアナ操作中も直前のエディタへ鍵盤入力を送れます。

ピン留めすると、スペアナをMGSTCのほかのウィンドウより前に保ちます。Windows全体で常に最前面になる機能ではなく、他のアプリを前面にすると覆われます。

## 音色ライブラリとセット

単音色／総合音色エディタの `ライブラリ管理` から一覧を開きます。タブは `音色一覧`、`パッチスライド`、`タグ管理`、`インポート`、`エクスポート` です。一覧ではカテゴリ、文字検索、タグ、★、並べ替えで絞り込み、選択音色の詳細確認、試聴、編集、複製、削除、タグ付与を行います。複数のタグを選んだ場合は、すべてを含む音色が表示されます。標準タグは94件で、独自タグも追加できます。

### セットで音色を整理する

`ツリー` を ON にすると、セットと未所属音色を表示します。セットの追加、複製、改名、メモ編集、削除は音色本体の削除とは別です。セット内の音色を通常ドラッグするとセット間で所属を移動し、Ctrl+ドラッグでは所属を追加します。同一セット内のドラッグは並べ替えです。セット内の音色を削除する操作は所属解除であり、音色一覧からの削除はライブラリ本体を削除します。削除前の確認を読んで対象を確かめてください。

### 編集した音色を保存する

単音色画面では、右側のライブラリから `読込`、`保存`、`別名保存`、`名前変更`、`削除` などを使います。既存音色を複製するには `読込` の後に `別名保存` します。総合画面では `保存 / ライブラリ` を使います。独自タグの名前変更・統合・削除は全音源のライブラリが対象で、標準タグは変更しません。アプリ内のユーザー音色ライブラリは `%LOCALAPPDATA%\MgsToneCraft\tone_library.sqlite` にあり、インストール／更新では削除されません。

## OPLL PatchSlide を探す

`ライブラリ管理` の `パッチスライド` タブでは、配置済みの OPLL PatchSlide データを探索できます。配布用の compact 版には12,180候補があり、初期版のデータには13,950候補があります。使用するデータの版によって件数は異なります。タグ検索は複数タグのAND／OR条件、分類検索は16群／64群、または全候補を対象にします。`Standard B` は距離順位による比較候補、`Variation` は別の候補選出です。候補を選んで試聴し、一時ストックへ追加できます。

`編集へ適用` では OPLL レイヤーへの追加または既存レイヤーの置換先を確認して適用します。適用は総合音色の編集で、共有 PatchSlide データは変更しません。候補の安全分類を確認してください。通常一覧では Reject 候補は隠され、`Rejectも表示` を明示したときに表示されます。PatchSlide タブにデータの不足や不整合が表示された場合は、PatchSlide 機能だけが利用できません。

## 外部音色のインポートとエクスポート

`インポート` タブで対応ファイルを開くと、見つかった完成音色が候補として一覧に加わります。対応形式には非圧縮MGS、MML／MUS定義、MuSICA VCD、SCC-Musixx SNG、VGM／VGZがあります。圧縮MGSや完成音色として特定できないデータは候補になりません。読み込みだけではライブラリに保存されません。候補を試聴し、名前や★を確認し、チェックした項目を `選択項目を登録` で登録します。候補の `編集` は未登録状態で対応エディタへ開きます。インポート候補はアプリ終了まで保持されます。

候補名の既定は `接頭辞 - 識別子` です。曲名がなければファイル名、音色番号・音色名がなければ `00` からの連番を使い、登録前に変更できます。MGS／MMLの総合候補は実際に使われた `@e` を基に作ります。OPLLの7～9chはリズムが有効な時点の音色候補から除外します。参照定義の不足には警告や音源初期値による補完があるため、候補と警告を確認してください。

`.mgstc` も `インポート` タブから開けます。この形式は、内容と登録の確認を終えるとライブラリへ追加されます。他の対応形式のように、`選択項目を登録` を押すまで未保存の候補として残るわけではありません。右パネルから音色を1件ずつ取り込む操作も利用できます。

### Furnace の音色を取り込む

`インポート` タブは `.fui` と `.fuw` も受け付けます。Furnaceインストゥルメントは対応する音源種別とデータだけが候補になり、非対応の音源種別やOPLLドラムは読み込みません。Furnaceの波形はSCC用32サンプルに変換され、元が32サンプル以外なら候補に `[コンバート]` が表示されます。新しいFurnace版や未対応マクロには警告が付くことがあります。

### 音色やセットを書き出す

`エクスポート` タブでは、一覧から追加した音色やセットを書き出します。形式は単音色・総合音色・セットの `.mgstc`、Furnace `.fui`、SCC波形 `.fuw` です。Furnace形式はMGSTCの全編集情報を表現するものではありません。特に総合音色からの `.fui` はレイヤーごとの静的な音色設定のみの部分変換で、`.fuw` はSCC波形のみを書き出します。結果に表示される警告・変換内容を確認してください。既存ファイルへ書き出す場合の上書き／連番／スキップ選択を確認します。

右パネルの `.mgstc` `取込`／`書出` は単一ライブラリ項目の交換です。これは `インポート` タブの外部演奏ファイル解析とは別の操作です。総合 `MGSCソースを開く`／`MGSCソースを書き出す`、SCC／OPLLの `.txt` 入出力はMGSC音色定義の操作です。

## アプリ設定と保存データ

右上のアプリ設定には `View`、`MIDI`、`Output`、`About` タブがあります。`View` は75%／100%／125%の表示倍率、`MIDI` はStandaloneの入力機器と受信条件、`Output` はPC音声出力と試聴経路の設定です。Output の選択によっては ASIO ドライバーなど環境固有の設定が必要です。マスターボリュームは0～100%で全画面が共有し、チップの音量値を変えず最終出力レベルを調整します。Standaloneでは再起動後も復元します。編集画面の `Ctrl` + `+`／`-` は倍率の一時上書き、`Ctrl+0` はViewの設定値へ復帰します。ウィンドウの位置・サイズは保存しません。

### 更新前のバックアップと互換性

**更新前に、重要な音色と保存ファイルをバックアップしてください。** v0.285の旧独自形式 `timbre-library-v1.mgstc` と `composite-timbre-library-v1.mgstc` は、v0.365へ直接読み込むことも、自動移行することもできません。元ファイルの保管だけでは、新版でそのまま使えるようにはなりません。旧ファイルを消さずに保管し、利用する音色に対応した移行方法が確認できるまで保持してください。

現在の音色ライブラリ `tone_library.sqlite` は、Alpha 版を含め、保存した音色・セット・タグなどの意味を保って読み込む互換性の対象です。保存構造は SQLite Schema v2 で、対応する旧構造から段階的に移行します。この方針は、旧独自形式のライブラリとは別です。

DAW のプロジェクトには、プラグインごとの総合音色を音色ライブラリとは独立して保存・復元します。設定 INI、単体 `.mgstc`、複合音色のポータブルファイルには、版をまたぐアップグレード互換の保証はありません。また、音源の計算方法が変わると、過去と同じ出音になるとは限りません。重要な音色は、MGSC 定義の `.txt` などにも書き出して保管してください。

## MGSCについて

SCC／OPLLのプレビューと `.txt` は各音色定義（`@s`／`@v`）です。総合音色のMGSCプレビュー／ソースには音色定義に加え、エンベロープ等の演奏設定が含まれます。どちらも曲全体の完成した `.MUS` を書き出すものではありません。単音色の一時出力番号はSCCが0～31、OPLLが15～31で、プレビューの番号に反映します。入力欄以外では `Ctrl+C`／`Ctrl+V` で定義をコピー／貼り付けできます。


## 参考資料

### 使用ライブラリ

- [JUCE](https://juce.com/)（Raw Material Software Limited）— UI／オーディオ基盤（AGPLv3 側で使用）
- [emu2149](https://github.com/digital-sound-antiques/emu2149)（Digital Sound Antiques）— YM2149（PSG）エミュレータ
- [emu2212](https://github.com/digital-sound-antiques/emu2212)（Digital Sound Antiques）— SCC エミュレータ
- [emu2413](https://github.com/digital-sound-antiques/emu2413)（Digital Sound Antiques）— YM2413（OPLL）エミュレータ
- [rpclib](https://github.com/rpclib/rpclib)（Tamás Szelei）— MAmidiMEmo 連携用 RPC クライアント

### 参考にしたサイト・資料

- [MGSDRV API仕様](https://gigamix.hatenablog.com/entry/mgsdrv/api-specifications)（GIGAMIX / Ain）
- [MGSC 1.11ユーザーマニュアル](https://www.gigamix.jp/mgsdrv/MGSC111.TXT)（GIGAMIX / Ain）
- [MGSDRVテクニック・資料集](https://gigamix.hatenablog.com/entry/mgsdrv/technique)（GIGAMIX / Ain）
- [MGSDRV v3.xxデータ形式解析](https://github.com/digital-sound-antiques/mgsc/blob/main/mgs-format.md)（Digital Sound Antiques）
- [YM2413アプリケーションマニュアル](https://d4.princess.ne.jp/msx/datas/OPLL/YM2413AP.html)（d4 / princess.ne.jp）
- [MAmidiMEmo](https://github.com/110-kenichi/MAmidiMEmo)（110-kenichi）— 実機音源RPCプロキシ（本体は同梱しません）
- [MAmidiMEmoNEMO readMe](https://github.com/uniskie/MSX_DOCUMENTS/blob/main/MAmidiMEmoNEMO/readMe.md)（uniskie）— MAmidiMEmo 利用の参考
- [msxplay-js](https://github.com/digital-sound-antiques/msxplay-js) / [libkss-js](https://github.com/digital-sound-antiques/libkss-js)（Digital Sound Antiques）— 試聴バランス等の参考
- [Wavetable Synthesizer algorithm](https://www.mathworks.com/help/audio/ref/wavetablesynthesizer-system-object.html)（MathWorks）
- [A Data-Driven Approach to Wavetable-Synthesis](https://www.creative-technologies.de/a-data-driven-approach-to-wavetable-synthesis/)（Creative Technologies）
- [JUCE `dsp::FFT` Class Reference](https://docs.juce.com/master/classjuce_1_1dsp_1_1FFT.html)（JUCE）— 実数FFT処理のAPI参照
- [JUCE `dsp::WindowingFunction` Class Reference](https://docs.juce.com/master/classjuce_1_1dsp_1_1WindowingFunction.html)（JUCE）— Hann窓処理のAPI参照
- [NumPy Discrete Fourier Transform (DFT)](https://numpy.org/doc/2.2/reference/routines.fft.html)（NumPy）— FFT／DFTの定義
- [SciPy `detrend`](https://docs.scipy.org/doc/scipy-1.14.0/reference/generated/scipy.signal.detrend.html)（SciPy）— DC／定常成分除去の参考
- [SciPy `correlation_lags`](https://docs.scipy.org/doc/scipy-1.11.1/reference/generated/scipy.signal.correlation_lags.html)（SciPy）— 時系列遅延の参考

