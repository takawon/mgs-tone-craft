# MGS Tone Craft 音源エンジン・60Hzイベント実行器 設計書

- 設計版: 0.1
- 基準日: 2026-07-25
- 対象: MGS Tone Craft（MGSDRV対応 複合音色エディタ）
- 状態: 実装開始用の初期設計

## 1. 目的

本設計は、PSG（YM2149）、SCC、OPLL（YM2413）を同時に試聴する音源エンジンと、
MGSDRV 3.20互換の約60Hzイベント実行器の責務・データ境界・実行順を定義する。

最重要の原則は、次の3経路で同じ意味実行器を使用することである。

1. GUI上のリアルタイム試聴
2. MGSDRV互換レジスタ列の回帰テスト
3. MGSC出力内容とのイベント一致検証

UIフレームワークとWindows音声出力APIは本設計から分離する。

## 2. 確定する基本方針

- コアはC++20のUI非依存ライブラリとして実装する。
- ビルドシステムはCMakeを使用する。
- 音源はチャンネルごとではなく、PSG・SCC・OPLLを各1チップずつ生成する。
- `@e`、`@r`、キーオン／オフ、音量合成、共有資源競合はMGSDRV意味実行層で処理する。
- 音源アダプタは、時刻順の物理レジスタ書き込みを受けて音声サンプルを生成する。
- エンジン内部サンプルレートは48,000Hz固定とする。
- 1論理カウントは800サンプルとする（48,000 / 60）。
- 実機Z80の命令サイクルと、処理過多によるフレームまたぎは再現しない。
- 同じ論理カウント内のレジスタ書き込み順と途中書き込みは削除しない。
- オーディオコールバック内では、ヒープ確保、ロック、ファイルI/O、ログ出力を行わない。

## 3. 全体構成

```text
Editor Model / Library
        |
        v
Program Compiler + Validator
  - @e/@r bytecode
  - layer/track allocation
  - zero-time loop validation
  - required tick-buffer capacity
        |
        v immutable snapshot
MGSDRV Runtime Core (60Hz)
  - 18 track states
  - key/gate state
  - envelope state
  - driver-compatible shadows
  - shared-resource conflict state
        |
        v ordered RegisterWrite[]
Chip Rack
  - Ym2149Adapter -> emu2149
  - SccAdapter    -> emu2212
  - Ym2413Adapter -> emu2413
        |
        v mono samples per chip
Float Mixer -> Master Headroom -> Audio Sink
```

`MGSDRV Runtime Core`と`Chip Rack`を合わせたものを`EngineCore`と呼ぶ。
Windowsの音声デバイス、GUI、保存ファイルは`EngineCore`から参照しない。

## 4. 時間モデル

### 4.1 論理時刻

```cpp
using Tick = std::uint64_t;
constexpr std::uint32_t kEngineSampleRate = 48'000;
constexpr std::uint32_t kTicksPerSecond = 60;
constexpr std::uint32_t kFramesPerTick = 800;
```

セッション開始直後をTick 0とする。Tick 0の全イベントを適用してから、
そのカウントに属する800サンプルを生成する。

キーオン時に`@e`先頭命令が同じ割り込み内で実行されるため、Tick 0では
キーオン処理とエンベロープ先頭処理の両方が完了した状態から音声生成を始める。

### 4.2 可変長オーディオコールバック

Windows側から要求されるフレーム数は800の倍数とは限らない。
`EngineCore::render()`は、次のTick境界までの残りフレーム数を保持し、
要求バッファを境界ごとの複数区間へ分割する。

```text
apply Tick N
render until next boundary
apply Tick N+1
render until next boundary
...
```

Tick境界をまたぐコールバックでも、イベントの適用サンプル位置は変化しない。

### 4.3 デバイスサンプルレート

決定論的テストと60Hz境界を優先し、コアは常に48kHzで動作する。
Windows出力デバイスが48kHz以外の場合の変換は`AudioSink`側の責務とする。
将来コアを可変レート化する場合は、浮動小数点時間を累積せず、
`floor(tick * sampleRate / 60)`による整数有理時計を使用する。

## 5. イベントモデル

### 5.1 意味イベントと物理イベント

イベントを二層に分ける。

```text
MeaningEvent
  Volume / Patch / YWrite / FrequencyDelta / Noise / Mode / KeyOn / KeyOff
        |
        v MGSDRV-compatible mapping
RegisterWrite
  PSG:  register 0..15
  SCC:  port + register + value
  OPLL: register 0..56
```

`MeaningEvent`は編集内容とMGSDRV命令の意味を表す。
`RegisterWrite`は音源へ実際に渡す最終結果であり、VGMとの比較単位でもある。

### 5.2 物理レジスタイベント

```cpp
enum class ChipId : std::uint8_t {
    Psg,
    Scc,
    Opll,
};

enum class WriteReason : std::uint8_t {
    KeyOn,
    KeyOff,
    Frequency,
    Volume,
    Patch,
    EnvelopeY,
    HardwareEnvelope,
    RhythmControl,
};

struct RegisterWrite {
    Tick tick;
    std::uint32_t sequence;
    ChipId chip;
    std::uint8_t port;       // SCC以外は0
    std::uint8_t address;
    std::uint8_t value;
    std::uint8_t track;
    WriteReason reason;
};
```

`sequence`は同じTick内の全音源共通の単調増加番号とする。
同じアドレスへ同じ値が連続しても削除しない。

`track`と`reason`はデバッグ、競合表示、MGSC出力照合に使用する。
音源アダプタは`chip/port/address/value`だけを使用する。

### 5.3 Tick書き込みバッファ

Tickごとの書き込みバッファはセッション開始前に必要容量を確保し、
実行中は`clear()`して再利用する。

- コンパイラは全トラックのゼロ時間命令、SCC 32バイト転送、OPLL音色ロードを解析する。
- 算出容量に安全余裕を加えて事前確保する。
- ハード上限を超える定義は再生開始前にエラーにする。
- 無待機ループ対策として、1トラック・1Tickあたりの命令実行予算を設ける。
- 実行予算超過時は対象トラックを停止し、リアルタイムスレッド外へ障害通知する。

初期ハード上限は、1Tickあたり8,192命令および8,192レジスタ書き込みとする。
この値は通常のMGSC定義より十分大きく、実装時に境界テストを追加する。

## 6. MGSDRV Runtime Core

### 6.1 セッション状態

```cpp
struct RuntimeSession {
    Tick tick;
    std::array<TrackRuntime, 17> tracks;
    PsgSharedState psgShared;
    SccSharedState sccShared;
    OpllSharedState opllShared;
    NineVoiceRhythmState rhythm9;
    TransportState transport;
};
```

演奏トラック数と処理順はMGSDRVに合わせる。MGSファイル上のトラック0は
音色・エンベロープ定義領域であり、`RuntimeSession`の演奏トラックには含めない。
MGSトラック1～17を、内部演奏トラック0～16へ対応させる。

1. PSGトラック 0～2
2. SCCトラック 3～7
3. OPLLトラック 8～16

同一Tickの共有レジスタ競合はこの処理順による後勝ちとする。

### 6.2 トラック状態

各`TrackRuntime`は最低限、次を保持する。

- 有効、ミュート、発音待ち
- 現在ノート、オクターブ、相対音程、デチューン
- 現在の音源内部周波数値
- キー状態とゲート残量
- トラック音量、マスター減衰、追加減衰
- 現在音色
- `@e`実行位置、待ち、ループ位置、ランプ剰余、現在音量
- `@r`の8bitレベル、フェーズ、Release予約
- PSGハードウェアEG選択状態
- キーオン時に復元する定義参照

### 6.3 1Tickの処理

```cpp
TickResult RuntimeSession::processTick();
```

処理手順:

1. Tick境界に予約されたNoteOn、NoteOff、Stopを反映する。
2. PSG、SCC、OPLLのトラック順に`TrackRuntime::processTick()`を呼ぶ。
3. 各トラックはMGSDRV 3.20と同じ順番で物理レジスタ書き込みを即時発行する。
4. `@e`のゼロ時間命令は、待ちを発生させる命令へ到達するまで同じTick内で継続する。
5. 発行された`RegisterWrite`を順番どおりChip Rackへ適用する。
6. Tick番号を進める。

Tick内の途中書き込みはイベントログへ全件残す。ただし実機Z80のサブフレーム時間は
再現しないため、書き込み間には音声サンプルを生成しない。

### 6.4 `@e`

既存の`tools/mgs_envelope_reference.py::EnvelopeState`をC++へ移植する。

- キーオン時の待ち1と、同Tick内の先頭命令実行
- 単独音量の1カウント待ち
- `:<count>`保持
- `=<count>`の整数剰余分配
- 待ち終了Tick内の後続命令継続
- `@`、`y`、`\`、`n`、`/`、`*`、ループのゼロ時間実行
- 終端後の最終音量再適用

### 6.5 `@r`

既存の`RateEnvelopeState`をC++へ移植する。

- ALから開始し、AR、DR、SL、SR、RRを8bit整数で処理する。
- PSG/SCCはキーオフ要求を保持し、Sustain到達後にRRへ移る。
- OPLLはハードウェアキーオフを出力し、同じキーオフではRRへ移らない。
- `floor(level * (trackVolume + 1) / 256)`で0～15へ量子化する。

### 6.6 状態シャドーの分離

次の状態を混同しない。

1. `DriverShadow`
   - MGSDRV自身が保持・更新する値
   - OPLLの`y`ではオリジナル音色レジスタ0～7を更新しない
2. `AuthoringShadow`
   - UIで複数ビットフィールドを安全に合成するための値
   - 9音リズムの共有ニブルを壊さない`y`生成にも使用する
3. `ChipRegisterMirror`
   - 実際にChip Rackへ最後に書いた物理値
   - デバッグ表示、テスト、スナップショットに使用する

## 7. Chip Rack

### 7.1 共通インターフェース

各アダプタはCライブラリの所有権をRAIIで包む。

```cpp
class Ym2149Adapter {
public:
    void reset();
    void write(std::uint8_t reg, std::uint8_t value);
    float renderSample();
};
```

SCCは`port/address/value`、OPLLは`register/value`を受ける。
エミュレータの構造体や関数をRuntime Coreへ公開しない。

### 7.2 YM2149 / emu2149

基準初期化:

```cpp
PSG* psg = PSG_new(3'579'545, 48'000);
PSG_setClockDivider(psg, 1);
PSG_setVolumeMode(psg, 1);
PSG_setQuality(psg, 1);
PSG_reset(psg);
```

書き込みは`PSG_writeReg()`、生成は`PSG_calc()`を使用する。

emu2149の内蔵レート変換は軽量で、公式READMEも最高精度には外部変換を推奨している。
初期実装は48kHz直接生成とし、VGM音声比較時にエイリアスが問題になる場合だけ、
チップ側ネイティブレート生成＋外部リサンプラーを第2段階として追加する。

### 7.3 SCC / emu2212

- 標準SCCモードを使用する。
- 1個のSCCインスタンスで5チャンネルを処理する。
- Runtime CoreのSCC `port/address`をemu2212のアドレス空間へ変換する。
- 波形転送は32書き込みを順序どおり渡す。
- 標準SCCのCh.4/5波形共有をアダプタ内で別管理せず、チップ実装へ反映させる。
- SCC+は初期実装に含めず、アドレス変換層を差し替え可能にしておく。

emu2212はリリースタグがないため、導入時にGitコミットを固定する。

### 7.4 YM2413 / emu2413

基準初期化:

```cpp
OPLL* opll = OPLL_new(3'579'545, 48'000);
OPLL_reset(opll);
OPLL_setChipType(opll, 0);
OPLL_resetPatch(opll, OPLL_2413_TONE);
```

書き込みは`OPLL_writeReg()`、生成は`OPLL_calc()`を使用する。

- レジスタ0～7は1チップで共有する。
- 9音メロディーモードと、レジスタ`0EH`による音源側リズムモードを同じインスタンスで扱う。
- 9音リズム中もMGSDRVが出力するレジスタ`26H`～`28H`書き込みを削除しない。
- `OPLL_setPan()`等のエミュレータ独自機能は、MGSDRV互換プレビューでは使用しない。

## 8. ミキサー

3チップの出力は32bit floatへ正規化し、次式で加算する。

```text
mix = masterGain * (
    psgSample  * psgGain +
    sccSample  * sccGain +
    opllSample * opllGain
)
```

- 初期値は各チップ中央、モノラルを左右へ同値出力する。
- チップ別ゲインは設定値として保持するが、MGSCデータへは出力しない。
- リミッターや自動正規化は音色を変えるため初期実装では使用しない。
- 最終出力範囲を超えた場合だけ安全クランプし、クリップ状態をUIへ通知する。

既定バランスは`msxplay-js` 1.9.1（commit
`6e163ef2a5bc167ede9b5e32aa021588e9415abf`）が使用する`libkss-js` 2.2.1に
準拠する。KSSX側に音源別音量がない通常状態では、libkssのPSG・SCC・OPLLの
デバイス音量はすべて0であり、同じ倍率で加算される。したがって既定チップ
ゲインはPSG:SCC:OPLL = 1.0:1.0:1.0とする。msxplay-jsのWebAudio出力に
合わせ、既定マスターゲインは3.0とする。

エミュレータの素の出力差を診断できるよう、48kHz、C4、最大論理音量で
4,800サンプルのウォームアップ後に48,000サンプルを測定するプローブを保持する。
PSGとSCCは矩形波、OPLLは持続型の最大音量オリジナル音色を使用した。

| 音源 | 素のPeak | 素のRMS | 既定チップゲイン |
|---|---:|---:|---:|
| PSG | 0.12448120 | 0.08789044 | 1.0 |
| SCC | 0.05859375 | 0.05831132 | 1.0 |
| OPLL | 0.06344604 | 0.04583222 | 1.0 |

この測定値によるRMS均一化は行わない。音色や多チャンネル加算によって
出力範囲を超過した場合だけ安全クランプし、クリップ状態を通知する。
測定は`mgstc_chip_level_probe`で再実行できる。上流の将来版で既定値が
変わっても自動追従させず、上記バージョンを本プロジェクトの固定基準とする。

## 9. リアルタイム制御

### 9.1 スレッド所有権

- 音声スレッドだけが`EngineCore`と3つのエミュレータを変更する。
- UIスレッドは音源APIを直接呼ばない。
- UIから音声へは単一生産者・単一消費者の固定長コマンドキューを使用する。
- 音声からUIへはメーター、クリップ、障害通知用の別キューを使用する。

### 9.2 コマンド

```cpp
enum class EngineCommandType {
    LoadProgram,
    NoteOn,
    NoteOff,
    Stop,
    SetMasterGain,
    SetChipGain,
};
```

`LoadProgram`は、不変かつ事前検証済みのプログラムを、3スロットの事前確保
プール内のスロット番号と世代番号で渡す。大きな波形・音色データをキューへ
コピーしない。

- 初期状態では1スロットを音声側が使用し、残り2スロットをUI側で取得できる。
- UIは`Free`スロットを`Editing`として取得し、その中で音源、ランタイムおよび
  エンベロープを構築する。この段階の確保・解放はUIスレッドで行う。
- 送信時にスロットを`Pending`へ変更し、以後UIは内容へ触れない。
- 音声コールバック先頭で`Pending`を`Active`へ切り替え、旧`Active`を`Free`へ
  返す。切替はポインターや`shared_ptr`ではなく、プール内インデックスで行う。
- 世代番号が一致しない古いコマンドは拒否する。
- プログラム切替中に音声スレッドで音源インスタンスの生成・破棄、ヒープ確保、
  参照カウント解放を行わない。

### 9.3 即時発声

パラメーター変更時の即時発声は、UI側で次を行う。

1. 新しい不変スナップショットをコンパイルする。
2. `LoadProgram`を送る。
3. `LoadProgram`に最後に手動発声した音階と再トリガー指定を含める。

音声スレッドは次のコールバック先頭で旧セッションを停止し、新スナップショットの
Tick 0から同じコールバック内で再発音する。切替完了は世代番号付き
`ProgramActivated`通知としてUIへ返す。自動発声は記憶音階を更新しない。

## 10. リセット・停止・シーク

- `HardReset`: 3チップをリセットし、全Runtime状態とレジスタミラーを初期化する。
- `Stop`: キーを解除して短い無音化処理を行い、その後必要ならHardResetする。
- `Retrigger`: チップ全体は再生成せず、対象プレビューセッションをTick 0へ戻す。
- 9音リズムモード切替を含むプレビューでは、F先頭の無音ガードをセッション開始時に1回実行する。
- 任意Tickへのシークは初期版では、音声を出さずTick 0から高速実行して状態を再構築する。
- 長いデータのシーク高速化が必要になった場合だけ、一定Tick間隔の状態チェックポイントを追加する。

## 11. コンパイル時検証

再生開始前に最低限、次を検証する。

- `@e`の不正オペコード、切れた引数、無待機無限ループ
- `@r`各値の範囲
- トラック数と物理チャンネル割り当て
- PSGハードウェアEG共有競合
- SCC Ch.4/5波形共有競合
- OPLL Original Tone Bus競合
- 9音リズム中のCh.7～9割り当て
- 打楽器音量用途の`@15`
- 同一Tickの書き込みバッファ上限
- SCC波形の32サンプル長

警告とエラーを区別する。共有資源の後勝ちを意図的に利用できる場合は警告として
再生可能にし、バッファ破壊や無限処理につながるものだけをエラーにする。

## 12. テスト戦略

### 12.1 決定論的コアテスト

現在のPython参照テスト29件をC++へ段階的に移植し、同じ期待値を使用する。

- `@e`保持、ランプ、ループ、周波数変更
- `@r`フェーズと音源別キーオフ
- PSGハードウェアEG
- OPLLパッチ＋`y`
- SCC波形転送
- 9音リズム共有ニブル

### 12.2 MGS/VGMゴールデンテスト

既存の次の実測ファイルをゴールデンデータとして使用する。

- `TEST.MGS/VGM`
- `TEST_BOUNDARY.MGS/VGM`
- `TEST_RATE.MGS/VGM`
- `TEST_AUTOMATION.MGS/VGM`
- `TEST_RHYTHM9.MGS/VGM`

テストでは音声波形より先に、Tick、書き込み順、レジスタ、値の一致を検証する。

### 12.3 音声テスト

- 固定プログラムから一定長の48kHz PCMを生成する。
- チップ別RMS、ピーク、無音区間、周波数を検証する。
- ライブラリ更新時はPCMハッシュだけで判定せず、差分波形と許容誤差も確認する。
- 実VGM由来音声との比較は、同じレジスタ列・同じクロック・同じ音量条件を揃えて行う。

## 13. 推奨ディレクトリ構成

```text
src/
  engine/
    include/
      engine/
        engine_core.hpp
        engine_command.hpp
        compiled_program.hpp
        register_write.hpp
    runtime/
      envelope_sequence.cpp
      envelope_rate.cpp
      track_runtime.cpp
      runtime_session.cpp
      register_mapper.cpp
    chips/
      ym2149_adapter.cpp
      scc_adapter.cpp
      ym2413_adapter.cpp
      chip_rack.cpp
    audio/
      float_mixer.cpp
      audio_sink.hpp
  app/
    ... UIとWindowsホスト ...
third_party/
  emu2149/
  emu2212/
  emu2413/
tests/
  engine/
  fixtures/
```

## 14. 実装順

### Phase 1: 決定論的コア

1. CMake/C++20の静的ライブラリとテスト実行環境
2. `RegisterWrite`、Tick時計、事前確保バッファ
3. `@e`と`@r`のC++移植
4. トラック順と共有状態
5. Python参照テストとの一致

### Phase 2: チップ音声

1. 3ライブラリをコミット固定して導入
2. 各チップの単音レジスタテスト
3. 48kHz PCM生成
4. チップ別ゲイン測定とミキサー
5. MGS/VGMレジスタゴールデンテスト

### Phase 3: リアルタイムホスト

1. SPSCコマンドキュー
2. 不変プログラム・スナップショット交換
3. `AudioSink`実装
4. NoteOn、NoteOff、Stop
5. アンダーラン、クリップ、障害通知

### Phase 4: UI接続

1. PCキーボードと発声ボタン
2. 単音色エディタ
3. 複合音色タイムライン
4. 保存・MGSC出力とのイベント一致

## 15. 確定した基盤選択

- Windows `AudioSink`は共有モード・イベント駆動WASAPIを直接使用する。
- 3エミュレータは`THIRD_PARTY.md`記載のコミットへ固定する。
- 既定ゲインはMSXplay準拠のPSG:SCC:OPLL = 1:1:1、master = 3.0とする。
- 3エミュレータは48kHzで直接生成する。外部高品質リサンプラーは使用せず、
  既定デバイス形式との変換が必要な場合はWindows Audio Engineへ任せる。

## 16. 参照

- [MGSDRV対応 複合音色エディタ仕様書](SPECIFICATION.md)
- [MGSDRV 3.20 ソフトウェアエンベロープ動作解析](analysis/MGSDRV_ENVELOPE_ANALYSIS.md)
- [emu2149](https://github.com/digital-sound-antiques/emu2149)
- [emu2212](https://github.com/digital-sound-antiques/emu2212)
- [emu2413](https://github.com/digital-sound-antiques/emu2413)

## 17. 実装状況

2026-07-25時点でPhase 1の決定論的コア、Phase 2の音源生成・
ミキサー基準実装、およびPhase 3のリアルタイム配送・Windows音声出力まで
完了した。

- CMake / C++20静的ライブラリ
- 48kHz・800サンプル境界の`TickClock`
- 容量固定・事前確保型`EventBuffer`
- 容量固定・事前確保型`RegisterWriteBuffer`
- `SequenceEnvelopeRuntime`
- `RateEnvelopeRuntime`
- `@e` / `@r`音量合成関数
- 無待機ループの命令予算停止
- PSG・SCC・OPLL別の`RegisterMapper`
- PSG / SCCの周期反転とOPLL F-Number加算による`\`変換
- SCC 32バイト波形転送とCh.4 / Ch.5共有波形RAM
- OPLL ROM音色選択、オリジナル音色8レジスタ転送、一時消音、`y`直列化
- MGS演奏トラック1～17を内部0～16へ対応させる`RuntimeSession`
- PSG → SCC → OPLLのトラック処理順と同一Tick通し`sequence`
- エンベロープ種別とトラック音量を保持する`TrackRuntime`
- マスター減衰・トラック追加減衰の実行時合成
- Tick境界での再キーオンと`@e` / `@r`状態初期化
- MGSDRV 3.20内蔵テーブルに基づくC1～B8のノート変換
- PSG / SCCの12bit基本周期とOPLLのF-Number / Block生成
- PSGキーオン時の初期Tone / Noise、Noise周期、基本周期の直列化
- SCC 5チャンネル共有キーマスクのキーオン／キーオフ
- OPLLハードウェアキーオン／キーオフとHigh→Low書き込み順
- emu2149 `441f3dc295a9b82db16921461f0678b483db8304`
- emu2212 `e1735c83707e6ee739a123476344659f7fa9fb18`
- emu2413 `11676f6c43af7a53a0a940f8faea57eed73a22ba`
- 3エミュレータのC APIと所有権を隠蔽するRAIIアダプター
- SCCの論理`port/address`からemu2212レジスタ空間への変換
- `RegisterWrite`列を3アダプターへ分配する`ChipRack`
- 48kHzでの3チップ個別floatサンプル生成
- 可変長コールバックを800サンプル境界で分割する`EngineCore::render()`
- Tick適用後に同Tickの800サンプルを生成する実行順
- チップ別ゲイン、マスターゲイン、ステレオ同値出力
- 最終出力の安全クランプとクリップ通知
- MSXplay準拠の既定バランス（PSG:SCC:OPLL = 1:1:1、master = 3.0）
- C4最大音量のPeak / RMS診断プローブ
- `TEST_BOUNDARY.VGM`を直接読み込むPSG Tick 0書き込みゴールデン照合
- PSGハードウェアEGの共有周期・共有形状・トラック別選択
- PSGハードウェアEG選択時のソフトウェア音量出力抑止
- 減衰合計8以上でのPSGレジスタ13再始動抑止
- 9音リズムのレジスタ`0EH`再トリガーと`36H`～`38H`共有ニブル
- 9音リズム音量用途での危険な`@15`拒否
- UI→音声および音声→UIの固定容量・ロックフリーSPSCキュー
- 音声コールバック先頭での`NoteOn`、`NoteOff`、`Stop`、`HardReset`、
  ミキサーゲイン変更コマンド適用
- 不正コマンド、レンダー失敗、クリップ開始・終了のUI通知
- キュー操作およびコマンド配送中の動的メモリ確保・ロック不使用
- 3スロット・世代番号付きの事前確保プログラムプール
- UIの`Free → Editing → Pending`と音声側の`Pending → Active → Free`による
  スナップショット所有権移譲
- プログラム切替と同一コールバック内の指定音階再トリガー
- 世代番号付き`ProgramActivated`通知
- Windows共有モード、48kHz 32bit float stereoのWASAPI `AudioSink`
- `AUDCLNT_STREAMFLAGS_EVENTCALLBACK`によるイベント駆動バッファ補充
- Windows Audio Engineによる既定デバイス形式への自動PCM変換
- `GetCurrentPadding()`に基づく書き込み可能フレーム算出
- WASAPI開始・停止、アンダーラン、デバイスエラー通知
- MSVC `/W4 /permissive-`ビルド
- CTestによるC++コア・音声生成・リアルタイム配送・WASAPIリンクテスト40件
- Python参照・VGM境界テスト29件

次の実装単位はPhase 4のWindowsアプリケーション・シェルと再生操作である。
モデルレスなメイン／OPLL／SCCウィンドウから同じ`RealtimeEngineHost`と
`WasapiAudioSink`を共有し、最初に画面鍵盤、PCキーボード、発声・停止ボタンを
実デバイスへ接続する。

Phase 4の初回実装として、追加ランタイム不要のWin32ネイティブEXE、
固定メインウィンドウ、モデルレスOPLL／SCCウィンドウ、白鍵・黒鍵のカスタム
鍵盤、マウス押下ドラッグ、`A S D F G H J K`と`W E T Y U`、停止ボタンを
同一`RealtimeEngineHost`へ接続した。次は各ウィンドウの実パラメーター
コントロールと即時発声スナップショット生成を接続する。
