# MGS Tone Craft 公開・配布チェックリスト

GitHubへのソース公開とWindows実行形式の配布前に、次をすべて確認する。

## 自作コード

- [ ] プロジェクト本体のライセンスを決定し、ルートの`LICENSE`へ記載する。
- [ ] 全コミットに秘密情報、個人情報、ローカル絶対パスがない。
- [ ] Release用構成でクリーンビルドし、全テストを実行する。

## サードパーティー

- [ ] `THIRD_PARTY.md`をソースとWindows配布物の両方へ含める。
- [ ] emu2149、emu2212、emu2413の各`LICENSE`を両配布物へ含める。
- [x] `third_party`はネストした`.git`を除いたvendored sourceとして公開する。
- [x] GUI基盤はWindows SDK同梱のWin32 APIと標準コントロールへ確定した。
      Qt、WinUI 3、WebView等の追加GUIランタイムは使用しない。

## 公開禁止・要権利確認データ

- [x] `.gitignore`対象の`analysis`全体をGitHubやReleaseへ含めない。
      ここにはMGSDRV、MGSC、MGSP、MSX-DOS 2、COMMAND2、曲データ、解析記録、
      解析用ディスク内容が含まれ得る。
- [x] EAYZS004をGitHubおよび配布物へ含めない。
- [x] `tests/fixtures`は自作の最小MMLと、それから生成したMGS/VGMだけであり、
      第三者の曲データやツール本体を含まないことを確認した。
- [x] ダウンロード元ZIP、ディスクイメージ、解析用外部リポジトリを含めない。
- [x] MGSDRV/MGSCのソース、バイナリ、逆アセンブル出力、抽出コードを含めない。

## Windows配布物

- [ ] 音源ライブラリのライセンスをアプリ内または同梱テキストから確認できる。
- [ ] 必要なMicrosoft Visual C++ RuntimeはMicrosoftが認める方法で同梱または案内する。
- [ ] ユーザー設定、ライブラリ、書き出しデータの保存先がProgram Files直下ではない。
- [ ] 新規Windows環境でインストール、起動、音声出力、MGSCテキスト出力を確認する。
