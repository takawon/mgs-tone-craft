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
| **エディタ／MGSC 出力** | §6.2.3、§6.1.1、§6.5、本書 §7 | 同一カウント内個数制限、MGSC 直列化順、`]` 以降省略、プレビュー、発音前 MML、仮番号 |

試聴引擎はバイト列を**並べ替えない**。エディタが §6.2.3 に従って MGSC バイト列を生成し、それを §6.2.1 の引擎へ渡す。

## 3. `@e` 実行モデル（解析確定）

`EnvelopeState.tick()`（`tools/mgs_envelope_reference.py`）が再現する MGSDRV 3.20 互換スケジューラーの要点。

### 3.1 Tick ループ

1. **待ちカウンター `wait > 0`**: ランプ中なら音量更新（`divmod` 剰余分配）→ `wait--`。まだ `wait > 0` なら Tick 終了。
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
| `0x20`～`0x2F` + 1byte | `vol=count` | count Tick ランプ | EC-005 |
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

PSG/SCC: 符号反転して周期値へ加算。OPLL: F-Number へ加算（`track_runtime` 側）。

### 3.5 音量ランプ — EC-005

float 線形補間**禁止**。整数剰余分配:

```python
numerator = ramp_remainder + ramp_magnitude
quotient, ramp_remainder = divmod(numerator, ramp_total)
volume += ramp_direction * quotient
```

ランプ開始 Tick では現在音量を一度 emit し、**次 Tick から**更新開始。

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

## 8. 総合画面・MGSC 出力（これから実装する部分）

現状 Alpha 未保証の領域。着手時は **仕様 §6.2.4** と **ENGINE_DESIGN §11.4** を正とする。

- チャンネル 1 本 = `@e` バイト列 1 本 + 共有の終端／ループ（§6、§12.0）。
- 音量・音程・音色・`y` 等は **同一バイト列へ直列化**してから試聴。別々の「音量タイムライン」「音程タイムライン」を試聴引擎に直結しない（現行 `compileCompositeEnvelopeLane` は廃止）。
- **カウント命令スタック** — L> 列の `[` 前後ゾーンを UI で分離編集する（`EnvelopeEvent.after_loop_start`）。`[` はゼロ時間アンカーであり、`]` 復帰先は `[` 直後（§6.2.3・参照実装 `loop_position`）。MGSC 直列化は **前 → `[` → 後**（`formatMgsCompositeEnvelope`）。`fff[y1,20fff]` と `fffy1,20[fff]` を回帰（engine_tests のループ開始アンカー）。
- MGSC 文字列生成は GUI 文字列を正本にせず、カウントブロック列から `f:10` / `f=10` 等へ変換（§6.1）。
- 同一カウント内のゼロ時間命令は §6.2.3 に従って MGSC へ直列化。**試聴は生成後のバイト列を §3 の引擎へ渡す。**
- SCC／PSG／OPLL 各エンベロープ下の `@e<n> = { ... }` プレビュー（§6.1.1）。
- 発音前 MML（§6.5）、`@e` 仮番号（§6.5.2）、演奏キー度数コメント（§6.5.3）。仮番号／`k`／`p`／`so` は総合初期設定・試聴・トラックプレビューへ実装済み。MGSC 先頭 `r`／`r%` 直列化は未実装。
- パッチスライド／オリジナル`y`・TL·FB自動時の先頭 `@` 自動挿入（§6.5.1）。
- `@e` 内 `\<n>` GUI は中央 0 二極グラフ（§6.2.2）。累積表示↔`\` 差分は直列化器が担当。

## 9. 公開リポジトリ上の注意

- **MGSDRV バイナリの解析手順・逆アセンブル結果・観測ログ等は公開文書へ書かない**（リポジトリにも含めない）。
- 公開文書・コミットメッセージ・README では「互換要件」「回帰テスト」「MGSC / MGSDRV 3.20 互換」等の**製品要件**だけを書く。
- 互換の根拠は §6.2.1、本書 §3、テストフィクスチャに留める。
