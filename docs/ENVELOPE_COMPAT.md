# ソフトウェアエンベロープ互換実装ガイド

実装者・エージェント向け。**MGSDRV 3.20 / MGSC 1.11 互換**の `@e`（シーケンス型）・`@r`（レート型）試聴・出力を、一箇所に集約して誤実装を防ぐための索引である。

利用者向けの振る舞い・受け入れ条件の正本は `SPECIFICATION.md` §6。本書は**解析で確定した互換挙動と、コードを触るときの正本の鎖**を扱う。

**重要**: `@e` まわりは複雑である。会話ログや UI 都合で挙動を言い換えず、必ず参照実装・マニフェスト・VGM ゴールデンと照合する。

## 1. 正本の順序（上から下へ）

| 順 | 文書／コード | 役割 |
|---|---|---|
| 1 | `SPECIFICATION.md` §6 | 利用者向け要件（§6.2.1＝互換実行モデル、§6.2.3＝エディタ製品ルール） |
| 2 | 本書 | 実装索引・解析詳細・変更チェックリスト・ケース ID |
| 3 | `tools/mgs_envelope_reference.py` | 論理イベントまでの参照実装（`EnvelopeState.tick`） |
| 4 | `src/engine/envelope_sequence.cpp` | 本番 `@e` ランタイム（`SequenceEnvelopeRuntime`） |
| 5 | `src/engine/envelope_rate.cpp` | 本番 `@r` ランタイム |
| 6 | `src/engine/track_runtime.cpp` / `runtime_session.cpp` | トラック合成・キーオン・音量合成・レジスタ映射 |
| 7 | `tests/fixtures/envelope_compat_manifest.json` | 共有回帰ケース ID（バイト列と期待値） |
| 8 | `tests/test_envelope_compat_manifest.py` | マニフェスト ↔ 参照実装の一致 |
| 9 | `tests/test_mgs_envelope_reference.py` | 参照実装の詳細単体テスト |
| 10 | `tests/cpp/engine_tests.cpp` | C++ ランタイム（マニフェストと同じケース ID を維持） |
| 11 | `tests/fixtures/mgsdrv_selftest/` | VGM ゴールデン（境界・レート・自動化など） |

**ルール**: 互換挙動を変えるときは、上表の **3→4→5→7→8→9→10** を同一変更内で更新する。§6 だけ、または UI だけを直して終わらせない。

## 2. 互換挙動とエディタ製品ルールの区別

| 種別 | 正本 | 内容 |
|---|---|---|
| **MGSDRV 互換試聴** | §6.2.1、本書 §3、`mgs_envelope_reference.py` | 待ちカウンター、バイト列の**記述順**実行、`\` 累積、ランプ `divmod`、ゼロ時間命令 |
| **エディタ／MGSC 出力** | §6.2.3、§6.1.1、§6.5、本書 §7 | 同一カウント内個数制限、MGSC 直列化順、`]` 以降省略、プレビュー、発音前 MML、仮番号、連続音量の `:` 結合、終端ホールド短縮、1行255文字改行、コンパイル256バイト |

試聴引擎はバイト列を**並べ替えない**。エディタが §6.2.3 に従って MGSC バイト列を生成し、それを §6.2.1 の引擎へ渡す。

## 3. `@e` 実行モデル（解析確定）

`EnvelopeState.tick()`（`tools/mgs_envelope_reference.py`）が再現する MGSDRV 3.20 互換スケジューラーの要点。

### 3.1 Tick ループ

1. **待ちカウンター `wait > 0`**: ランプ中なら音量更新（8bit加算後の`divmod`剰余分配）→ `wait--`。まだ `wait > 0` なら Tick 終了。
2. **`wait == 0`**: ランプ状態をクリアし、命令解釈ループへ。
3. **命令解釈ループ**（バイト列を先頭から順に読む）:
   - 終端に達した → 終端音量を emit して終了。
   - 音量単独（オペコード `0x00`～`0x0F`）→ 音量適用、`wait=1`、**ループ break**（この Tick 終了）。
   - ゼロ時間命令（`@` / `y` / `\` / `n` / `/` / `[` / `]`）→ emit して **continue**（同 Tick 内で次命令へ）。
   - 保持（`0xE0`～）・ランプ（`0x20`～）→ 音量適用・待ち設定、**break**。

キーオン時: `position=0`, `volume=0`, **`wait=1`**。最初の Tick で `wait` を 0 にして先頭命令から解釈開始（EC-001～003）。

### 3.2 バイト列＝実行順（MML 例）

MGSC 上の並びがそのまま実行順。音量命令の**直後**のゼロ時間命令は、当該音量で待ちが発生した**後**、`wait==0` になった Tick から**記述順**で実行される。

| MML 例 | 実行順 |
|---|---|
| `f\20d\10a\5` | `f → \20 → d → \10 → a → \5` |
| `f\20f\20f\20f\20` | 各 `f` の直後の `\20` で周波数オフセット `0→+20→+40→+60→+80` |

**誤り例**: 「`f` の直前に `\20`」— バイト列上 `\20` は常に直前の音量命令の**後**に来る。

### 3.3 オペコード対応（MGS バイト列）

| オペコード | MGSC | 時間 | 参照実装 |
|---|---|---|---|
| `0x00`～`0x0F` | `0`～`f` | 1 Tick 待ち | `volume=opcode; wait=1; break` |
| `0x10` + 1byte | `@n` | 0 | `patch` |
| `0x11` + 2bytes | `yreg,data` | 0 | `register_write` |
| `0x12` + 1byte | `\n`（128以上は負数） | 0 | `frequency_delta`（累積） |
| `0x20`～`0x2F` + 1byte | `vol=count`（MGSC の `f=<n>`） | count Tick ランプ | EC-005 / EC-009～019 |
| `0x40` | `[` | 0 | ループ開始位置記録 |
| `0x60` | `]` | 0 | ループ位置へジャンプ |
| `0x80`～`0x9F` | `n` | 0 | `noise` |
| `0xA0`～`0xA3` | `/` または `*` | 0 | `tone_noise_mode` |
| `0xE0`～`0xEF` + 1byte | `vol:count` | count Tick 待ち | EC-002 |

### 3.4 `\`（周波数変更）— EC-007

- **相対加算**。絶対音程指定ではない。
- **ゼロ時間**。同一 Tick 内で複数 `\` が連続可能。
- **累積**。各 `\` は直前までの内部周波数を基準に加算。
- マニフェスト: バイト列 `12 03 12 FF 0F`（`\3\255 f`）→ Tick 0: `+3`, `-1`, 音量 `15`。

内部状態はトラックごとに **16bit**（PSG/SCC=周期、OPLL=F-num 下位8bit＋レジスタ20h相当の上位）。`ADD` は16bit折り返しで、0や `0x0FFF` へ**クランプしない**。

PSG/SCC: 符号反転してからその16bitへ加算し、下位8bitと上位8bitをチップへ書く。YM2149 / K051649 の周波数は上位4bitだけ有効。周期0付近（SCCではハードウェア周期≤8）は無音になり得る。大きな正の `\` 連打で16bitが回ると、12bitクランプ実装は0で止まるがドライバは `0xFxx` 側へ出る。

OPLL: 符号は反転しない。パック16bitへ加算したあと、**1コマンドあたり1回だけ**窓へ戻す。F-num bit8が立っていて低位≥`0x59`（F-num≥`0x159`）なら `+0x153`、bit8が落ちていて低位<`0xAC` なら `-0x153`。`0x153` は Block±1 と F-num ∓ `0xAD` に相当する。9bit F-num だけを加減して `0xAC`～`0x15F` で `0xAD` を複数回巻き戻す近似は、中程度の `\` では一致し、Block 7を超える大きな `\` ではキービットへ溢れるドライバとずれる。

### 3.5 音量ランプ — EC-005

float 線形補間**禁止**。整数剰余分配:

```python
numerator = (ramp_remainder + ramp_magnitude) & 0xFF
quotient, ramp_remainder = divmod(numerator, ramp_total)
volume += ramp_direction * quotient
```

ランプ開始 Tick では現在音量を一度 emit し、**次 Tick から**更新開始。加算は
MGSDRVのZ80 Aレジスタと同じ8bitで、桁上がりを除算へ持ち越さない。

ステップ幅は次のパターンで決まる（`S.T=C` は先頭の音量命令＋ランプ）。

1. Tick 0 で開始音量 `S` を1カウント適用し、Tick 1 でランプ開始（まだ `S`）。
2. 以降 C 回、上式で更新する。`D < C` かつ `R+D < 256` のあいだは、1段階の滞在が
   `floor(C/D)` または `ceil(C/D)` Tick になる（Bresenham 型）。先頭段階だけ
   上記の2 Tick が加わる。上昇と下降は同じ幅列の符号反転である。
3. `R+D >= 256` になると `N` が折り返し、無限精度なら発生するはずの商が
   落ちる。`D=15` では `C=241` まで目標へ到達し、`C=242` は途中で幅が倍化して
   2で停止、`C=254` は8で停止、`C=255` は17 Tick 刻みで到達する。`D=12` かつ
   `C=254`／`255` は商が常に0で開始値のままである。

MGSC 1.11の `:`／`=` カウントは文書上 2～239。コンパイラは **1** と
**ちょうど240** だけを `Invalid parameter` にする（241～255は通る）。
ドライバの `2n cc` は 0～255 の生バイトを実行する。エディタの MGSC 出力は
240 を出さない。

EC-010の`f.0=100`はキーオン相対Tick 0～7が`f`、8～14が`e`、…、
95～100が`1`、101で`0`になる。EC-011／019の`c.0=255`／`254`は差分12の
剰余が8bitで折り返すため、更新中も`c`のままである。
EC-013～016は`f.0=50`、`c.0=50`、`f.0=5`、`c.0=5`。
EC-017は上昇`0.f=50`が下降と同じ幅列。EC-018は`f.0=242`の折り返し停止。
EC-019は`c.0=254`がゲート中ずっと`c`のままであること。
トラック音量`v`は§3.6の合成後出力だけを変え、ランプの更新Tickは変えない。

生バイト列のランプcount 0はエラーではない。現在音量をそのTickに一度emitし、
次Tickで後続命令へ進む（EC-012）。エディタが生成する自動変化durationは
1～255に制限する。

MGSC の音量自動変化は `f=<n>`（例: `f.8=10`）で表し、コンパイル後は
先頭の1カウント音量＋`0x20 | 8`、`0x0A`（`2n cc`）になる。保持のあと
続ける場合は `f:2.0=4` のように `=` だけを続ける。エディタの自動ステップは
直前の音量指定（音量指定がない場合はトラックの `v` をカウント 0 の仮指定とする）
とのカウント間隔を duration に使う。duration は 1～255 のバイト値へ制限し、
ランタイムの開始 Tick と剰余分配は通常の `0x20`～`0x2F` と同じである。
直列化した `f=<n>` は直前の音量指定位置で実行し、指定ステップのカウントで
目標値へ到達する。内部モデルでは自動変化イベントを目標ステップに保持する。
連続する自動変化は `f.8=10.0=10` のように `=` を続け、間に原点音量の
1カウントを差し込まない。ゼロ時間命令が自動変化の途中カウントにあるときは
**編集モデルは1本の自動変化のまま**とし、MGSC／`@e` 出力だけ命令位置の
線形補間音量で `=` を分割する。命令の後にその音量を1カウント原点として付けない（例: `f.8=5.\20.0=5`。`.\20.8.0=5` の `8.` は余分なステップになる）。1カウント区間は `=` せず16進1文字にする。
`f.8=10` のパースでは先頭の1カウント `f` を原点として `cc` を加算する
（原点+1+cc にはしない）。`8=10` だけでも従来どおり目標ステップ 10 になる。

### 3.6 音量合成（トラック `v` との関係）

`@e` 音量 `E` とトラック音量 `V`（0～15）:

```text
premaster = max(0, E + V - 15)
final = max(0, premaster - master_attenuation - track_attenuation)
```

OPLL は下位 4bit を `XOR 15` して減衰ニブルへ。詳細は `SPECIFICATION.md` §6.3。

### 3.7 禁止事項（再発防止）

- UI／タイムラインが独自に「音量だけ進める」「ランプを float 補間する」
- 会話や仕様の言い換えだけで `\` の向き（直前／直後）を決める — **必ず参照実装と EC-007 を見る**
- ゼロ時間命令の同一 Tick 内順序を、§6.2.3 の MGSC 直列化順と混同して**試聴引擎側で並べ替える**

## 4. データの流れ（UI を含む）

```text
総合画面タイムライン / 将来のコンパイラ
        │
        ▼  §6.2.3 に従う MGSC 直列化 → MGS バイト列（@e 定義）
        │  ＋ 発音前トラック MML（§6.5）
SequenceEnvelopeRuntime  ──► MeaningEvent（Volume, Patch, YWrite, FrequencyDelta, …）
        │
        ▼  TrackRuntime::processTick
RegisterWrite 列（PSG / SCC / OPLL）
        │
        ▼  Chip Rack → 試聴 PCM / VGM 照合 / MGSC 出力検証
```

## 5. 変更チェックリスト

互換挙動を追加・修正するとき:

1. `SPECIFICATION.md` §6.2.1 に利用者向け要件があるか確認。なければ追記（文書版を上げ §18 に行追加）。
2. 本書 §3 を更新。
3. `tools/mgs_envelope_reference.py` を更新。
4. `src/engine/envelope_sequence.cpp`（および必要なら `envelope_rate.cpp` / `track_runtime.cpp`）を更新。
5. `tests/fixtures/envelope_compat_manifest.json` にケース ID を追加または期待値を更新。
6. `tests/test_envelope_compat_manifest.py` が通ることを確認。
7. `tests/cpp/engine_tests.cpp` の **同じ ID** のテストを更新。
8. 必要なら `tests/fixtures/mgsdrv_selftest/` に最小 MML/VGM を追加。
9. 本書 §6 のケース表を更新。

## 6. 共有回帰ケース ID

| ID | 内容 | Python（詳細） | C++ |
|---|---|---|---|
| EC-001 | 隣接単音量は 1 Tick ずつ | `test_adjacent_single_count_volumes_are_one_tick_apart` | `testAdjacentVolumesAreOneTickApart` |
| EC-002 | 4 カウント保持列 | `test_four_count_holds` | `testFourCountHoldsCompat` |
| EC-003 | 保持終了 Tick で次命令 | `test_hold_count_executes_next_command_when_count_reaches_zero` | （マニフェストのみ。C++ 追加推奨） |
| EC-004 | `@` / `y` ゼロ時間 | `test_patch_and_register_write_do_not_advance_time` | `testPatchAndRegisterWriteAreZeroTime` |
| EC-005 | ランプ剰余分配 | `test_ramp_uses_remainder_distribution_and_starts_next_tick` | `testRampUsesIntegerRemainderDistribution` |
| EC-006 | ループに暗黙待ちなし | `test_loop_has_no_implicit_wait` | （マニフェストのみ） |
| EC-007 | `\` 累積ゼロ時間 | `test_frequency_changes_are_zero_time_and_cumulative_in_mapping_layer` | `testFrequencyDeltasAreSignedAndCumulativeEvents` |
| EC-008 | 無待ちループは命令予算で停止 | — | `testNoWaitLoopHitsInstructionBudget` |
| EC-009 | `f=<n>` 自動音量ランプの `2n cc` | `test_automatic_volume_opcode_uses_target_and_duration` | `testAutomaticVolumeOpcode` |
| EC-010 | `f.0=100`の実測更新Tick | `test_f_to_zero_over_100_matches_observed_driver_ticks` | `testFToZeroOver100MatchesObservedDriverTicks` |
| EC-011 | count 255の8bit剰余折り返し | `test_count_255_ramp_uses_eight_bit_accumulator_wrap` | `testCount255RampUsesEightBitAccumulatorWrap` |
| EC-012 | 生count 0は実行Tickを消費 | `test_zero_count_ramp_consumes_its_execution_tick` | `testZeroCountRampConsumesItsExecutionTick` |
| EC-013～016 | duration 50／5・開始値`f`／`c`の実測更新Tick | `test_observed_duration_50_and_5_ramp_ticks` | `testObservedDuration50And5RampTicks` |
| EC-017 | 上昇`0.f=50`は下降と同じステップ幅 | `test_ramp_step_widths_follow_eight_bit_remainder_pattern` | `testRampStepWidthsFollowEightBitRemainderPattern` |
| EC-018 | `f.0=242`の8bit折り返しで目標未到達 | `test_ramp_step_widths_follow_eight_bit_remainder_pattern` | `testRampStepWidthsFollowEightBitRemainderPattern` |
| EC-019 | `c.0=254`は8bit折り返しで`c`のまま | `test_ramp_step_widths_follow_eight_bit_remainder_pattern` | `testRampStepWidthsFollowEightBitRemainderPattern` |

期待バイト列と論理イベントは `tests/fixtures/envelope_compat_manifest.json` を正とする。

## 7. テストの実行

```text
# マニフェスト ↔ 参照実装（Python）
python -m unittest tests.test_envelope_compat_manifest

# 参照実装の既存単体
python -m unittest tests.test_mgs_envelope_reference

# C++ コア（ビルド後）
ctest -R engine
```

## 8. 総合画面・MGSC 出力

現状 Alpha 未保証の領域。着手時は **仕様 §6.2.4** と **ENGINE_DESIGN §11.4** を正とする。

- チャンネル 1 本 = `@e` バイト列 1 本 + 共有の終端／ループ（§6、§12.0）。
- 音量・音色・`y` 等はレーン別バイト列が残る。`\` は音量 `@e` ストリームへ載せて試聴する（別レーン待ちだと MGSDRV より1カウント早く周波数が動く）。保持の終端カウントに並ぶ `\` は次の音量の直前（`e:7.\-127.d:4`）。音量イベントの「次カーソル」だけを見ると欠落する。
- **カウント命令スタック** — L> 列の `[` 前後ゾーンを UI で分離編集する（`EnvelopeEvent.after_loop_start`）。`[` はゼロ時間アンカーであり、`]` 復帰先は `[` 直後（§6.2.3・参照実装 `loop_position`）。MGSC 直列化は **前 → `[` → 後**（`formatMgsCompositeEnvelope`）。`fff[y1,20fff]` と `fffy1,20[fff]` を回帰（engine_tests のループ開始アンカー）。
- MGSC 文字列生成は GUI 文字列を正本にせず、カウントブロック列から `f:10`（保持）／`f=10`（自動変化）等へ変換（§6.1）。連続する同一音量は `f:2.e.d:5` へ結合する。定義終端の `f:<n>` は後続命令が無いので `f` にする（`]` 直前は残す）。連続する自動変化は `f.8=10.0=10`。途中のゼロ時間命令は、既定（`詳細`OFF）では出力側だけ `=` を分割し、命令後は次の `target=<n>` だけを続ける（`f.8=5.\20.0=5`）。`詳細`ONでは無分割 `f=<n>` の剰余ランプ Tick 列へ命令を挟み、`=` を再開しない。編集イベントは分割しない。`詳細`は総合形式17のイベントフラグであり、MGSC 本文からは復元しない。形式16以前は `詳細` を false、形式15以前は自動変化も false として読む。
- `@e` ソースは MGSC の1行255文字制限で `.` 分割改行する（継続行はタブ。コメントは1行なら `}` 後、改行時は `{ ,,` 直後）。コンパイル上限は **256バイト**（文字数ではない）。END 自動可変も同じバイト数を使う。
- 同一カウント内のゼロ時間命令は §6.2.3 に従って MGSC へ直列化。**試聴は生成後のバイト列を §3 の引擎へ渡す。**
- 音量レーンの編集座標は 0～15 を等間隔にし、チップ固有の論理音量カーブは半透明の実効値オーバーレイとして表示する。自動変化区間の実効バーは試聴と同じ `@e` バイト列を `SequenceEnvelopeRuntime` で進めた音量に同期する。PCM 波形を音量編集値へ逆算しない。
- 自動変化ONの編集棒は開始音量から終端音量を結ぶラインであり、斜め点線は使わない。
- SCC／PSG／OPLL 各エンベロープ下の `@e<n> = { ... }` プレビュー（§6.1.1）。
- 発音前 MML（§6.5）、`@e` 仮番号（§6.5.2）、演奏キー度数コメント（§6.5.3）。仮番号／`k`／`p`／`so`および§6.5.3コメントは総合初期設定・試聴・トラック／定義プレビューへ実装済み。MGSC 先頭 `r`／`r%` 直列化は未実装。
- パッチスライド／オリジナル`y`・TL·FB自動時の先頭 `@` 自動挿入（§6.5.1）。
- `@e` 内 `\<n>` GUI は中央 0 二極グラフ（§6.2.2）。累積表示↔`\` 差分は直列化器が担当。

## 9. 公開リポジトリ上の注意

- **MGSDRV バイナリの解析手順・逆アセンブル結果・観測ログ等は公開文書へ書かない**（リポジトリにも含めない）。
- 公開文書・コミットメッセージ・README では「互換要件」「回帰テスト」「MGSC / MGSDRV 3.20 互換」等の**製品要件**だけを書く。
- 互換の根拠は §6.2.1、本書 §3、テストフィクスチャに留める。
