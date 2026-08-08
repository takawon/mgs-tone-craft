# サードパーティーライブラリ

本体（MGS Tone Craft）のライセンスはAGPL-3.0-onlyで、全文は`LICENSE`に置く。
ここでは依存関係の取得方法とライセンスを記録する。

## UIフレームワーク（JUCE）

| 項目 | 値 |
|---|---|
| 取得方法 | CMake `FetchContent`（ソースツリーへvendorしない） |
| 固定コミット | `91ad83ae34a81e0833b1a2b0866f54846370ae53`（JUCE 8.0.15） |
| ライセンス | AGPLv3 と商用ライセンスのデュアル。本プロジェクトはAGPLv3側で使用 |
| 使用モジュール | `juce_gui_extra`、`juce_audio_utils`とその依存モジュール |

- 本体をAGPL-3.0-onlyとすることでJUCEのAGPLv3条件を満たす。商用ライセンスは購入しない。
- JUCE 8.0.15はスプラッシュ画面を使用しない版であり、`JUCE_DISPLAY_SPLASH_SCREEN`は
  指定しても無視される。表示義務のための追加実装は行わない。
- JUCEモジュールが同梱するzlib、pnglib、jpeglib、HarfBuzzなどの各依存は、
  JUCEソース内の`LICENSE.md`および各ディレクトリのライセンス表示に従う。

## 音源エミュレータ

音源プレビューでは、Digital Sound Antiquesによる次のMITライセンス実装を
`third_party`へvendored sourceとして固定して使用する。ネストした`.git`は
含めず、各ライブラリのライセンス表示を保持する。

| 音源 | ライブラリ | 固定コミット |
|---|---|---|
| YM2149 PSG | emu2149 | `441f3dc295a9b82db16921461f0678b483db8304` |
| Konami SCC | emu2212 | `e1735c83707e6ee739a123476344659f7fa9fb18` |
| YM2413 OPLL | emu2413 | `11676f6c43af7a53a0a940f8faea57eed73a22ba` |

各ライブラリの著作権表示とライセンス全文は次のファイルに保持する。

- `third_party/emu2149/LICENSE`
- `third_party/emu2212/LICENSE`
- `third_party/emu2413/LICENSE`

ソース公開物と実行形式の配布物には、これらの著作権表示およびライセンス全文を
同梱する。

emu2413には、エディタのプレビュー同期に必要なエンベロープ進行APIを
プロジェクト側で追加している。変更箇所にも元のMITライセンスが適用され、
著作権表示とライセンス全文を保持する。

## MAmidiMEmo RPCクライアント（rpclib）

MSX実機への試聴出力では、MAmidiMEmoの`-chip_server`（msgpack-RPC）へ接続する。
クライアント実装には[rpclib](https://github.com/rpclib/rpclib)を使用する。

| 項目 | 値 |
|---|---|
| 取得方法 | CMake `FetchContent`（ソースツリーへvendorしない） |
| 固定タグ | `v2.3.0` |
| ライセンス | MIT |
| 用途 | `DirectAccessToChip`の同期プローブと非同期書き込み |

現行MSVCでは同梱`format.h`の`stdext::checked_array_iterator`が使えないため、
構成時にvcpkgと同趣旨のガード（`_MSC_VER < 1951`）を適用する。

MAmidiMEmo本体は本製品に同梱・リンクしない。利用者が別途起動する外部プロセスであり、
そのライセンスと配布は公式リリースに従う。

## 参照実装

既定の音量バランスは`msxplay-js` 1.9.1と`libkss-js` 2.2.1の動作を
調査して仕様化したものであり、両リポジトリのコードを本製品へリンクまたは
コピーしていない。将来コードを取り込む場合は、その時点で依存関係と
ライセンスを再監査する。

MGSDRV/MGSC互換の挙動は、入出力および実機相当の動作観察から独立に実装した
ものである。

openMSX等の外部実装は、MAmidiMEmo RPCのワイヤ事実確認に限り参照し得るが、
コード・クラス構成・スレッド処理の移植は行わない（仕様§4.6）。
