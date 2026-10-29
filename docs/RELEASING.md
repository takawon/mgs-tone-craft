# Windowsリリースの準備と承認

Windows配布物はローカルで生成します。内容を確認してGitHubのdraftへ登録し、さらに別の承認を得てからAlpha pre-releaseとして公開します。通常のビルド、push、PR、開発タグをきっかけに公開するworkflowはありません。ソースZIP／tar.gzには、GitHubがタグから提供する標準アーカイブを使います。

## 必要な環境と素材

- [ビルド環境](../BUILDING.md)と、事前の通常ビルドで取得したJUCE／rpclibソース。Prepareは既存の依存ソースを指定して通信せずにビルドします。初回の依存取得はPrepareの前に済ませてください。
- `installer/distribution.json`で固定したInno Setup **6.7.3**と、その公式配布の補助ファイル。コンパイラー本体・補助ファイル・日英言語ファイルのSHAを検査します。自動ダウンロードやコード署名は行いません。
- 正式PatchSlide DBの引渡しフォルダー。`patchslide.sqlite`、`patchslide_search.sqlite`、`manifest.json`を同じ承認済みセットとして使用します。`ProgramData`は利用者の導入先であり、配布元として収集しません。
- 現在のDB取得方式は`LocalHandoff`のみです。取得層は3ファイルのローカルパスを返します。検証・共通ステージング・生成の各処理は取得方式に依存しません。将来の独立DBリポジトリの特定Release取得は、この取得層へ追加します。現段階ではGitHubからDBを取得する処理はありません。

## 1. Docs Prep：公開版との差分と文書を整備する

GitHubの公開Release一覧で、draftを除いた直近の公開版と実際のタグSHAを確認します。`releases/latest`はpre-releaseを除くため、Alphaの比較元選定には使いません。ローカルの同名タグとGitHubの参照先が一致するとは限りません。

比較元を`$previousTag`と`$previousCommit`（40桁）として記録します。Prepareはこの入力を使ってオフラインで差分一覧を生成し、Stage／PublishがGitHubの最新公開版・実タグSHAを再照合します。機能／操作／UI／入出力／互換性／配布／修正の7分類で、変更なしの項目も含めて人が確認してください。

リリース指示を受けたら、Prepareを実行する前にDocs Prepで公開文書を整えます。前回公開版からの機能・操作・UI・導入・保存互換性の変更を調べ、READMEの機能説明・What's New、公開操作ガイド、CHANGELOG、Release Notes案を必要に応じて実際に編集します。直近の開発コミットだけでWhat's Newを決めません。画像は現行UIと比較し、画面変更があるものだけ再撮影します。更新不要の場合も、その根拠と確認範囲を短く記録します。

READMEの主導線は版に依存しないReleases一覧とし、「最新公開版はv…」「対象版は未公開」といった公開前後で真偽が変わる断定を使いません。過去版は履歴・比較元として明確に区別します。配布形式はEXE／手動ZIP／GitHub標準ソースZIP・tar.gzを区別し、導入ガイドは日英両方を案内します。具体版の未作成アセットURLだけを主導線にしません。

READMEでは `mgstc:download:start/end` と `mgstc:whats-new:start/end` のHTMLコメントを各1組維持します。ゲートは対応するH2見出し、対象版、配布説明を検査し、マーカー外の冒頭や現行版断定も検査します。履歴中の旧版番号だけでは拒否しません。形式が欠落・重複・不正、明確な旧版案内や未公開表示があれば、正式候補はファイル・行・見出し・問題文言を示して停止します。Prototypeは警告に留めてもStage／Publishには使用できません。自然言語すべての意味や画像内容の正しさは機械検査だけでは保証できません。

外部リンクの取得不能と404は別に扱い、未確認の重要事項を黙って合格扱いにしません。外部リンクと画像の人手レビューは、下記の受入記録へ残します。相対リンク・画像・見出し・大文字小文字・Git追跡はツールでも検査します。

Docs Prepでは、事実の更新に加えて、次の編集レビューを毎回行います。これはリリース時だけでなく、通常の機能追加・UI変更・不具合修正・導入条件の変更、新しいガイドの初稿にも適用します。

1. **文章品質**：何ができるか、どこを操作するかを先に示します。音源・MML・UIの正確な用語は残し、内部実装の説明は必要な読者向けの節へ分けます。一文に条件や操作を詰め込まず、手順は実行順に並べます。
2. **技術的正確さ**：文章品質とは独立して、実装・仕様・実画面との一致を確認します。保存互換性、バックアップ、上書き・削除、外部環境、変換の制限、未検証範囲を落としていないか照合します。読みやすさのために仕様を変えたり、保証を広げたりしません。
3. **日本語・英語の一致**：対応する文書は同時に見直します。手順、条件、警告、パス、ファイル名を突き合わせ、英語は逐語訳ではなく自然な指示文にします。製品名・UIラベル・音源用語の表記も揃えます。ZIPの構造・導入・更新・削除手順を変更したら、日英INSTALLを一組で必ず点検します。専用展開先と削除対象を一致させ、共用ディレクトリ全体の削除を案内しません。バックアップによる保管と新版への移行は区別し、実証していない再利用手順は推奨しません。
4. **確認記録**：対象ファイルごとに確認範囲を残します。変更が不要な文書も理由を記録し、変更前後の例と保留事項を最終レビューへ提示します。文章品質と技術的正確さは別々に判定し、必要に応じて別の担当者がレビューします。

一般向けの手順、上級者向けの技術仕様、開発者向けのビルド・公開手順、ライセンスは読者別に保ちます。過去のUIモックを現行画面や確定した予定と混同しません。ユーザーに見えるコード変更では、関連文書の正確さと読みやすさも同時に点検し、適用不要の場合は理由を記録します。最終校正では誤操作・矛盾・事実誤認と操作上の迷いをまとめて解消し、意味に影響しない表現の好みだけで修正を繰り返しません。

Docs Prep後に文書・掲載画像・公開禁止情報を監査し、利用者が差分と画像を確認できる状態で、人による最終確認を依頼します。対象はREADME、公開ガイド、全参照画像、CHANGELOG、Release Notes案です。自動検査はリンク・版・ファイル名・マーカー・digestなどの整合性を確認するもので、自然な文章や意味の正しさの確認を代替しません。文書・画像の確認や重要な注意事項のレビューが未完了なら、公開を保留します。

利用者の別途承認後に、公開許可されたファイルだけをcommit／pushして候補ソースの完全SHAを確定します。画像のGit追跡も確認します。正式Prepareはこのcleanなコミットと作業ツリーの内容一致を要求し、Stage／PublishもGitツリー・文書画像digest・Notesと配布SHAを再検証します。文書・画像・Notes・候補コミット・配布物のどれかが変われば再検証と該当承認を更新し、古い候補・承認・SHAは流用しません。

GitHubトップのREADMEは既定ブランチの内容です。候補のREADMEが既定ブランチにも反映される手順を確認し、Releaseの作成だけでトップが更新されると扱いません。

Prepare／Stage／PublishはREADME・ガイド・画像を自動変更せず、commit／pushも行いません。不整合ならDocs Prepへ戻します。Stageのタグはdraftと同時でも公開参照可能です。READMEのcommit／push段階とタグ作成段階は別で、StageとPublishにはそれぞれ別の指示・承認が必要です。

## CHANGELOGとRelease Notesを同じ公開版差分から作る

CHANGELOGの今回候補節が変更内容の正本です。比較元は直近に実際に公開したタグの製品状態で、途中の開発版やコミット一覧ではありません。初公開機能の開発中修正はAddedへ統合し、Fixedは前回公開版にも存在した不具合に限ります。既存利用者の不具合修正・データ保護・互換性の注意を落とさず、解消済みの一時制限はNotesに残しません。公開済みの歴史は当時の内容を保持します。

Docs Prepで、項目ID、前公開版の存在と状態、候補の最終状態、分類、コード・仕様等の根拠、両文書の掲載箇所を持つ非公開台帳を作り、利用者へCHANGELOG差分とNotes案を提示します。意味の正しさはCodexの根拠確認と人のレビューで扱います。自動検査は台帳の整合・全項目の対応を確認しますが、根拠文の事実を自動証明するものではありません。

準備中は`## [0.365] - Unreleased`のような見出しを使い、承認された正式候補では`## [0.365]`のように版を確定します。公開日が未確定なら日付を省略し、実績日を先に捏造しません。正式PrepareはUnreleasedを拒否します。

```powershell
.\tools\write-release-notes.ps1 -OutputFile 'dist/notes-review/RELEASE_NOTES.md'
```

原稿は新規ファイルへBOMなしUTF-8／LFで生成し、既存原稿やソースを上書きしません。現在の方式は、CHANGELOG候補節の本文を分類・注意事項ごとコピーする方式です。自由な要約や片方だけの分類変更は許可しません。内容を変える場合はCHANGELOGを編集し、新しい原稿を生成して再レビューします。`-ReleaseNotesFile`は候補本文との一致検証用であり、改行・先頭BOM以外の実質差分を許す抜け道ではありません。原稿自体が未追跡でも、正式Prepareは確定GitツリーのCHANGELOGからの一致を検証するため、未検証の別原稿をPublishへ渡せません。

正式Prepareには`-ChangeAuditFile '<非公開の分類台帳.json>'`を渡します。台帳は`version`、`previousTag`、`previousCommit`、`baselineVerified`と`items`を持ちます。各項目は`id`、`category`、本文のUTF-8 SHA-256である`sha256`、`baselineState`（absent/present/not-applicable）、非空の`evidence`を持ちます。Fixedは`baselineBugConfirmed=true`、Notesは`currentRelevant=true`も必要です。項目数・分類・本文SHA・比較元が一致しなければ停止します。新規機能をFixedにする、公開版の修正や注意を落とす、一時制限をNotesとして承認する台帳は不整合として拒否します。

本文のSHAは、CHANGELOGの正本から抽出して台帳へ記録します。文字列を手入力し直すと、表記や空白の違いで一致しなくなるためです。

```powershell
. .\tools\release-content.ps1
$version = (Get-Content .\VERSION -Raw).Trim()
$section = Get-MgstcChangelogCandidate (Get-Content .\CHANGELOG.md -Raw -Encoding UTF8) $version
$section.items | Select-Object category, sha256, text
```

各項目について、比較元の公開版での状態と根拠を確認して台帳を埋めます。初公開機能をFixedへ分類した台帳や、不明な比較元を確認済みとして扱わないでください。検査導入前の候補には新しい分類bindingがないため、Stage／Publishへ流用せず再Prepareします。
`-ValidateOnly`が表示する`releaseContentDigest`を、既存のレビューJSONへ追加します。これに候補commit、比較元、CHANGELOG候補節SHA、Notes SHA、分類digestを結び付けます。台帳のID・根拠本文は公開せず、公開する候補情報には版とSHAだけを含めます。Stageで登録した本文とPublish直前の本文も取得して照合します。古い承認・候補を流用せず、正本・原稿・分類・commit・配布物が変わったら再検証と該当承認更新が必要です。Prepare／Stage／PublishはCHANGELOGや入力原稿を黙って書き換えません。

## 2. ローカルで調査・試作する

以下の変数は自分の環境で設定します。実際の絶対パスや秘密はリポジトリへ保存しません。

```powershell
$dbSource = '<正式DB引渡しフォルダー>'
$iscc = '<Inno Setup 6.7.3のISCC.exe>'
$previousTag = '<前回公開タグ>'
$previousCommit = '<そのタグの実コミットSHA・40桁>'

.\tools\prepare-release.ps1 -PreviousTag $previousTag -PreviousCommit $previousCommit `
  -DbSourceDirectory $dbSource -IsccPath $iscc -Prototype -ValidateOnly
```

`-ValidateOnly`（`-DryRun`）はビルド・配布物生成・外部公開をせず、既存のビルド証跡と入力を検査します。新しい配布ツールを導入した直後など、証跡が古いと拒否するため、その場合は先に正式ビルドを行います。

```powershell
.\tools\build.cmd -Configuration Release
.\tools\prepare-release.ps1 -PreviousTag $previousTag -PreviousCommit $previousCommit `
  -DbSourceDirectory $dbSource -IsccPath $iscc -Prototype
```

未コミット差分で試作する`-Prototype`の生成物は、Stage／Publishで必ず拒否します。これは公開候補の代用にはなりません。各実行は新しい候補フォルダーを作り、古いEXE／ZIPを上書き・流用しません。`-OutputDirectory`で指定する場合も新規フォルダーにします。

## 3. レビューを記録し、確定候補を生成する

公開するソースが承認されたコミットとなったら、`-ValidateOnly`の表示する`sourceDigest`、`documentsDigest`、比較元SHAに対して人がレビューします。次のJSONは記入例であり、自動承認用テンプレートではありません。受入が完了してから値を記入し、ローカルの記録として保存します。

```json
{
  "accepted": true,
  "reviewer": "<確認者>",
  "reviewedAt": "<確認時刻・ISO 8601>",
  "sourceDigest": "<表示されたSHA256>",
  "documentsDigest": "<表示されたSHA256>",
  "releaseContentDigest": "<分類台帳・候補節・Notes・commitのSHA256>",
  "previousCommit": "<前回公開の実SHA>",
  "externalLinksAccepted": true,
  "screenshotReview": "accepted",
  "changeCategories": ["feature", "operation", "ui", "io", "compatibility", "distribution", "fix"]
}
```

確定候補のレビューでは、画像のハッシュだけでなく、実際の画像と説明文を確認してください。ソースや文書を変更したら、レビューも新しいdigestへ更新します。ソース・文書のdigestはGitとWindowsの改行差を吸収するためテキストのCRLFだけをLFに正規化します。画像・製品・DB・配布アセットのSHAは実バイト列の値です。

```powershell
.\tools\prepare-release.ps1 -PreviousTag $previousTag -PreviousCommit $previousCommit `
  -DbSourceDirectory $dbSource -IsccPath $iscc -ChangeAuditFile '<非公開の分類台帳.json>' `
  -ReviewFile '<レビューJSON>' -ReleaseNotesFile '<検証済みNotes原稿>'
```

通常はRelease両製品のビルドと全回帰を実行します。`-UseValidatedBuild`は、すでに同じ全ソースを正式ビルド・全回帰した証跡がある場合だけ再利用でき、変更があると拒否します。`-ReleaseNotesFile`で指定する原稿は、先頭を`# MGS Tone Craft <VERSION> Alpha`とし、BOMなしUTF-8／LFへ確定してハッシュを記録します。

候補フォルダーには次を生成します。

| ファイル | 内容 |
|---|---|
| `MGSToneCraft-<VERSION>-win64-setup.exe` | 日英インストーラー |
| `MGSToneCraft-<VERSION>-win64.zip` | Standalone、製品VST3 bundle、正式DB3ファイル、INSTALL.txt（日本語）・INSTALL.en.txt（英語）、文書・画像・ライセンス |
| `SHA256SUMS.txt` | 実際に配布するEXEとZIPの2件のSHA256 |
| `candidate.json`／`RELEASE_NOTES.md` | 対象SHA・素材・レビュー・アセットの固定情報と公開原稿 |
| `payload/`／`changes.json` | 同一素材の検証用ステージングと比較元からの変更一覧 |

GitHubへ添付するのは最初の**3アセットだけ**です。ローカル証跡、ステージング、独自ソースZIPを追加添付しません。候補情報のうち公開可能な版・SHA・レビュー情報は、Release本文の機械読取欄へ記録します。

## 4. 別途指示してdraftへ登録する

GitHubに対象コミットが存在すること、候補の画像・説明・同梱物を確認したうえで、draft登録を明示的に指示します。ローカルでは`GH_TOKEN`（または`GITHUB_TOKEN`）に必要な権限のトークンを安全に設定してください。値をファイル・ログ・チャットへ貼り付けません。

```powershell
$candidate = '<確定候補フォルダー>'
$repository = '<owner/repository>'
.\tools\stage-release.ps1 -CandidateDirectory $candidate -Repository $repository
.\tools\stage-release.ps1 -CandidateDirectory $candidate -Repository $repository -Execute
```

無指定では検証のみです。`-Execute`では版・完全SHA・アセットdigest・Notes SHAを含む今回限りの承認文を入力します。**この段階で公開参照可能なGitタグを作成し、その後に非公開draftを作ります。draftでもタグやソースが非公開になるわけではありません。**

既存の別SHAのタグ・異なる候補のdraft・公開済みReleaseがあれば停止します。同じ候補の中断再開だけを許し、アセットを上書きしません。3アセットを再ダウンロードしてSHAを照合します。

## 5. 最終承認して公開する

draftの本文・版・対象SHA・3アセット・画像・配布方法を利用者が最終確認します。draft登録時の承認は、公開の承認にはなりません。

```powershell
.\tools\publish-release.ps1 -CandidateDirectory $candidate -Repository $repository
.\tools\publish-release.ps1 -CandidateDirectory $candidate -Repository $repository -Execute
```

再び対象ハッシュを含む承認文の入力が必要です。公開直前にソース・文書・ZIP内容・DB・VST3・Notes・タグ・アセットを再検証し、`prerelease=true`、`make_latest=false`で公開します。公開済みReleaseの自動差替え・削除・タグ付け替えは行いません。

GitHub APIへ公開要求を送る前の失敗はdraftを保持します。公開要求の送信後に通信が切れた場合は、GitHub側だけ完了している可能性があります。**読み取りで実際の公開状態を確認し、無条件に再実行しないでください。**

## 任意のGitHub Actions公開

`.github/workflows/release.yml`は`workflow_dispatch`専用で、ローカルで作成済みのdraftを検証・公開する役割です。Windows runner上でDB取得や別ビルドを行わず、ローカルのセットアップと別のZIPを混ぜません。今回の実装だけではGitHub上で有効になりません。別途承認されたcommit／pushでdefault branchへ導入する必要があります。

運用前に次を設定します。

1. Environment **`mgstc-release`**を作り、**Required reviewers**に実際の確認者を設定します。単独運用で自分が承認する場合はPrevent self-reviewsを有効にしないでください。最初はUser reviewerを推奨します。
2. Actionsを許可します。検証jobは`contents: read`／`actions: read`、公開jobだけ`contents: write`を使用します。Environmentの読取・実際のreview履歴をAPIで確認できない場合は公開しません。Team reviewerには追加の組織メンバー参照権限が必要になり得ます。
3. default branchから、`verify`または`publish`、正確なtag、40桁commit、`assetsDigest`、`notesSha256`を指定して手動起動します。`publish`はEnvironment承認後に同じ候補を再検証します。

Environment保護が未設定・利用不可の場合は安全側で停止します。利用者が別途承認したローカル公開コマンドを使い、承認なしの自動公開へ切り替えません。同一タグはActionsのconcurrencyとローカルの共通ロックで保護しますが、別PC／ローカルとActionsの同時実行を分散ロックするものではありません。**1つの候補の公開実行者・実行経路を1つに決めてください。**

ローカルトークンには対象repoのContents書込とActions読取が必要です。公開側設定・トークン権限・Required reviewersは実リポジトリで運用開始前に確認します。模擬テストは、実際のEnvironment設定や実公開の成功を示すものではありません。

## 正式DBセットを将来更新するとき

自動で「最新」を採用しません。DB側で特定バージョンと正式3ファイルを確定し、同一セットのSHA、manifestの束縛、候補数、必要ライセンスをレビューして共通定義を更新します。取得providerを将来追加しても、検証済みの3ファイルを返す契約と、下流の検証・生成処理を維持します。

インストーラー／ZIPの新規環境での起動、DB認識、VST3認識、実音声・MIDI・DAW、既存ユーザーデータ保護の受入は別途必要です。自動生成・静的検査を実環境試験の代用にしません。

手動導入ガイドの手順変更では、`installer/INSTALL.txt`（日本語）と`installer/INSTALL.en.txt`（英語）を必ず同時更新し、配置先・外部環境・更新削除の注意の意味を照合します。両方をUTF-8 BOM付き・CRLFで保存します。配布定義の必須ファイル一覧と共通記載事項に従い、ZIP生成時の実再展開と公開ゲートで存在・文字コード・記載事項・候補ソースとの一致を検査し、両言語を文書レビューdigestへ含めます。機械検査は翻訳の意味の一致を保証しないため、日英レビューが未完了なら公開を保留します。
