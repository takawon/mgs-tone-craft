; Build through tools/build-installer.ps1; payload is an explicit validated stage.
#ifndef SourceRoot
  #error SourceRoot must point to a validated distribution stage
#endif
#ifndef AppVersion
  #error AppVersion is required
#endif
#ifndef BinaryVersion
  #error BinaryVersion is required
#endif
[Setup]
AppId={{170DA51C-AD34-49EF-8702-A282ACDA7EC8}
AppName=MGS Tone Craft
AppPublisher=Takawo.
VersionInfoCompany=Takawo.
AppVersion={#AppVersion}
VersionInfoVersion={#BinaryVersion}
VersionInfoProductVersion={#BinaryVersion}
DefaultDirName={autopf}\MgsToneCraft
DefaultGroupName=MGS Tone Craft
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputBaseFilename=MGSToneCraft-{#AppVersion}-win64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
LicenseFile={#SourceRoot}\app\LICENSE
UsePreviousSetupType=yes
LanguageDetectionMethod=uilanguage
ShowLanguageDialog=yes
UsePreviousLanguage=yes
CloseApplications=no
RestartApplications=no
UninstallDisplayIcon={app}\mgstc.exe
DisableProgramGroupPage=yes
[Languages]
Name: "english"; MessagesFile: "languages\Default.isl"
Name: "japanese"; MessagesFile: "languages\Japanese.isl"
[CustomMessages]
english.StandardType=MGS Tone Craft and shared PatchSlide database
japanese.StandardType=MGS Tone Craftとパッチスライド共有DB
english.CustomType=Custom installation
japanese.CustomType=カスタムインストール
english.StandaloneComponent=MGS Tone Craft (required)
japanese.StandaloneComponent=MGS Tone Craft（必須）
english.DatabaseComponent=Shared OPLL PatchSlide database (required)
japanese.DatabaseComponent=OPLLパッチスライド共有DB（必須）
english.Vst3Component=VST3 Instrument for 64-bit DAWs (optional)
japanese.Vst3Component=64-bit DAW用VST3インストゥルメント（任意）
english.UninstallShortcut=Uninstall MGS Tone Craft
japanese.UninstallShortcut=MGS Tone Craftのアンインストール
english.OperationFailed=Data operation failed. Exit code: %1
japanese.OperationFailed=データ処理に失敗しました。終了コード: %1
english.CloseDawsToRemove=Close all DAWs using MGS Tone Craft and retry. Cannot remove: %1
japanese.CloseDawsToRemove=MGS Tone Craftを使用中のDAWをすべて終了して再試行してください。削除できません: %1
english.CannotRemove=Cannot remove: %1
japanese.CannotRemove=削除できません: %1
english.CloseDatabaseUsers=Close MGSTC, all DAWs and other PatchSlide database users, then retry.
japanese.CloseDatabaseUsers=MGSTC、すべてのDAW、パッチスライドDBを使用する他のアプリを終了して再試行してください。
english.MissingHelper=Uninstall helper is missing. Repair the installation before uninstalling.
japanese.MissingHelper=アンインストール用補助ファイルがありません。再インストールで修復してからアンインストールしてください。
english.DataRemovalTitle=MGS Tone Craft - data removal
japanese.DataRemovalTitle=MGS Tone Craft - データの削除
english.DataRemovalInfo=Select independently. Unknown files and other Windows users are preserved.
japanese.DataRemovalInfo=削除項目は個別に選択できます。対象外のファイルと他のWindowsユーザーのデータは保持します。
english.UserDataPath=User data: %1
japanese.UserDataPath=ユーザーデータ: %1
english.SharedDatabasePath=Shared DB: %1
japanese.SharedDatabasePath=共有DB: %1
english.UserDeletionUnavailable=User deletion unavailable: %1
japanese.UserDeletionUnavailable=ユーザーデータを削除できません: %1
english.DeleteUserData=Delete user library and settings (permanent data loss)
japanese.DeleteUserData=ユーザー音色ライブラリと設定を削除する（元に戻せません）
english.DeleteSharedDatabase=Delete shared PatchSlide DB (other applications may use it)
japanese.DeleteSharedDatabase=パッチスライド共有DBを削除する（他のアプリも使用する可能性があります）
english.Continue=Continue
japanese.Continue=続行
english.Cancel=Cancel
japanese.Cancel=キャンセル
english.ConfirmDeleteUser=Permanently delete tone_library.sqlite, its SQLite sidecars and settings-v1.ini in:
japanese.ConfirmDeleteUser=次のフォルダー内のtone_library.sqlite、SQLite補助ファイル、settings-v1.iniを完全に削除します:
english.ConfirmDeleteUserContents=This includes timbres, sets, tags and favorites. Continue?
japanese.ConfirmDeleteUserContents=音色、セット、タグ、お気に入りが削除されます。続行しますか？
english.ConfirmDeleteShared=Delete the three shared PatchSlide files? Other applications may stop finding their DB.
japanese.ConfirmDeleteShared=パッチスライド共有DBの3ファイルを削除しますか？ 他のアプリでもDBを利用できなくなる可能性があります。
english.HelperReparsePoint=Refusing a reparse point: %1
japanese.HelperReparsePoint=再解析ポイントのため処理を中止しました: %1
english.HelperDesktopUserUnknown=Cannot uniquely identify the Windows desktop user. User data will be preserved.
japanese.HelperDesktopUserUnknown=Windowsデスクトップのユーザーを一意に特定できません。ユーザーデータは保持します。
english.HelperProfilePathUnresolved=The desktop profile/AppData path is not a resolved absolute path.
japanese.HelperProfilePathUnresolved=デスクトップユーザーのプロファイルまたはAppDataのパスが、展開済みの絶対パスではありません。
english.HelperAppDataRedirected=AppData is redirected outside the identified profile. User data will be preserved.
japanese.HelperAppDataRedirected=AppDataが特定したユーザープロファイルの外へリダイレクトされています。ユーザーデータは保持します。
english.HelperFilesInUse=Close MGSTC, all DAWs and other applications using these files, then retry. %1
japanese.HelperFilesInUse=MGSTC、すべてのDAW、および対象ファイルを使用している他のアプリを終了してから再試行してください。%1
english.HelperSqliteSidecar=SQLite sidecar present: %1. Close all applications and complete/recover the database transaction before retrying.
japanese.HelperSqliteSidecar=SQLiteの付随ファイルが存在します: %1。すべてのアプリを終了し、データベースのトランザクションを完了または復旧してから再試行してください。
english.HelperSourceRequired=InstallDb requires -Source.
japanese.HelperSourceRequired=InstallDbには-Sourceの指定が必要です。
english.HelperPackagedHashMismatch=The packaged database hash is incorrect: %1
japanese.HelperPackagedHashMismatch=同梱データベースのハッシュが一致しません: %1
english.HelperDatabaseOperationPending=Another database operation or interrupted replacement exists. Inspect %1 and its backup before retrying; no database was changed.
japanese.HelperDatabaseOperationPending=別のデータベース操作が実行中、または前回の置換が中断されています。%1とそのバックアップを確認してから再試行してください。データベースは変更していません。
english.HelperStagedHashMismatch=Staged database verification failed: %1
japanese.HelperStagedHashMismatch=配置準備したデータベースの検証に失敗しました: %1
english.HelperRollbackFailed=Database replacement AND rollback failed. Close all applications. Preserve %1 and %2 for recovery. %3; rollback: %4
japanese.HelperRollbackFailed=データベースの置換と元の状態への復元の両方に失敗しました。すべてのアプリを終了してください。復旧のため%1と%2を保持してください。置換エラー: %3 / 復元エラー: %4
english.HelperReplacementRestored=Database replacement failed; the original files were restored. %1
japanese.HelperReplacementRestored=データベースの置換に失敗したため、元のファイルへ復元しました。%1
english.HelperDatabaseInstalled=The complete shared database set was installed and verified.
japanese.HelperDatabaseInstalled=共有データベース一式のインストールと検証が完了しました。
english.HelperDatabaseUninstallPending=A shared database replacement is active or interrupted. Resolve it before uninstalling database files.
japanese.HelperDatabaseUninstallPending=共有データベースの置換が実行中、または中断されています。置換の問題を解決してからデータベースファイルをアンインストールしてください。
english.HelperDatabaseRemoved=The three installer-managed shared database files were removed. Unknown files were preserved.
japanese.HelperDatabaseRemoved=インストーラーが管理する共有データベース3ファイルを削除しました。管理対象外のファイルは保持しました。
english.HelperUserPathMismatch=The confirmed user-data path does not match the current desktop user. Nothing was deleted.
japanese.HelperUserPathMismatch=確認済みのユーザーデータ保存先が、現在のデスクトップユーザーと一致しません。何も削除していません。
english.HelperUserDataRemoved=Known timbre-library and settings files for the confirmed desktop user were removed. Unknown files were preserved.
japanese.HelperUserDataRemoved=確認済みのデスクトップユーザーの音色ライブラリと設定の管理対象ファイルを削除しました。管理対象外のファイルは保持しました。
english.CannotWriteMessages=Cannot prepare the language messages file: %1
japanese.CannotWriteMessages=表示言語のメッセージファイルを準備できません: %1
english.DataOperationError=Data operation failed: %1
japanese.DataOperationError=データ処理に失敗しました: %1
[Types]
Name: "standard"; Description: "{cm:StandardType}"
Name: "custom"; Description: "{cm:CustomType}"; Flags: iscustom
[Components]
Name: "standalone"; Description: "{cm:StandaloneComponent}"; Types: standard custom; Flags: fixed
Name: "database"; Description: "{cm:DatabaseComponent}"; Types: standard custom; Flags: fixed
Name: "vst3"; Description: "{cm:Vst3Component}"; Flags: disablenouninstallwarning
[Files]
Source: "{#SourceRoot}\app\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceRoot}\vst3\*"; DestDir: "{commoncf64}\VST3\MGS Tone Craft.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
; dontcopy files never enter the uninstall log. DB ownership is explicitly conditional.
Source: "{#SourceRoot}\db\patchslide.sqlite"; Flags: dontcopy
Source: "{#SourceRoot}\db\patchslide_search.sqlite"; Flags: dontcopy
Source: "{#SourceRoot}\db\manifest.json"; Flags: dontcopy
[Icons]
Name: "{group}\MGS Tone Craft"; Filename: "{app}\mgstc.exe"
Name: "{group}\Uninstall MGS Tone Craft"; Filename: "{uninstallexe}"; Comment: "{cm:UninstallShortcut}"
[Code]
var
  RemoveUser, RemoveShared: TNewCheckBox;
  UserDataPath, DataHelper: String;

function Quote(const S: String): String;
begin
  Result := '"' + S + '"';
end;

procedure WriteHelperMessages(const Path: String);
var
  Messages: String;
begin
  Messages :=
    'HelperReparsePoint=' + CustomMessage('HelperReparsePoint') + #10 +
    'HelperDesktopUserUnknown=' + CustomMessage('HelperDesktopUserUnknown') + #10 +
    'HelperProfilePathUnresolved=' + CustomMessage('HelperProfilePathUnresolved') + #10 +
    'HelperAppDataRedirected=' + CustomMessage('HelperAppDataRedirected') + #10 +
    'HelperFilesInUse=' + CustomMessage('HelperFilesInUse') + #10 +
    'HelperSqliteSidecar=' + CustomMessage('HelperSqliteSidecar') + #10 +
    'HelperSourceRequired=' + CustomMessage('HelperSourceRequired') + #10 +
    'HelperPackagedHashMismatch=' + CustomMessage('HelperPackagedHashMismatch') + #10 +
    'HelperDatabaseOperationPending=' + CustomMessage('HelperDatabaseOperationPending') + #10 +
    'HelperStagedHashMismatch=' + CustomMessage('HelperStagedHashMismatch') + #10 +
    'HelperRollbackFailed=' + CustomMessage('HelperRollbackFailed') + #10 +
    'HelperReplacementRestored=' + CustomMessage('HelperReplacementRestored') + #10 +
    'HelperDatabaseInstalled=' + CustomMessage('HelperDatabaseInstalled') + #10 +
    'HelperDatabaseUninstallPending=' + CustomMessage('HelperDatabaseUninstallPending') + #10 +
    'HelperDatabaseRemoved=' + CustomMessage('HelperDatabaseRemoved') + #10 +
    'HelperUserPathMismatch=' + CustomMessage('HelperUserPathMismatch') + #10 +
    'HelperUserDataRemoved=' + CustomMessage('HelperUserDataRemoved');
  if not SaveStringToFile(Path, UTF8Encode(Messages), False) then
    RaiseException(FmtMessage(CustomMessage('CannotWriteMessages'), [Path]));
end;

function RunData(const Mode, Source, UserPath: String; var Detail: String): Boolean;
var
  Params, ResultPath, MessagesPath: String;
  ExitCode: Integer;
  Contents: AnsiString;
begin
  ResultPath := ExpandConstant('{tmp}\mgstc-data-result.txt');
  MessagesPath := ExpandConstant('{tmp}\mgstc-data-messages.txt');
  WriteHelperMessages(MessagesPath);
  DeleteFile(ResultPath);
  Params := '-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File ' + Quote(DataHelper) +
    ' -Mode ' + Mode + ' -ResultFile ' + Quote(ResultPath) + ' -MessagesFile ' + Quote(MessagesPath);
  if Source <> '' then Params := Params + ' -Source ' + Quote(Source);
  if UserPath <> '' then Params := Params + ' -UserPath ' + Quote(UserPath);
  Result := Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'), Params,
    '', SW_HIDE, ewWaitUntilTerminated, ExitCode);
  Detail := '';
  if LoadStringFromFile(ResultPath, Contents) then Detail := UTF8Decode(Contents);
  Result := Result and (ExitCode = 0);
  if (not Result) and (Detail = '') then Detail := FmtMessage(CustomMessage('OperationFailed'), [IntToStr(ExitCode)]);
  if (not Result) and (Detail <> '') then Detail := FmtMessage(CustomMessage('DataOperationError'), [Detail]);
  Log(Mode + ': ' + Detail);
end;

procedure RemoveDeselectedVst3;
var
  Root, FileName: String;
begin
  Root := ExpandConstant('{commoncf64}\VST3\MGS Tone Craft.vst3');
  FileName := Root + '\Contents\x86_64-win\MGS Tone Craft.vst3';
  if FileExists(FileName) and not DeleteFile(FileName) then
    RaiseException(FmtMessage(CustomMessage('CloseDawsToRemove'), [FileName]));
  FileName := Root + '\Contents\Resources\moduleinfo.json';
  if FileExists(FileName) and not DeleteFile(FileName) then
    RaiseException(FmtMessage(CustomMessage('CannotRemove'), [FileName]));
  RemoveDir(Root + '\Contents\x86_64-win');
  RemoveDir(Root + '\Contents\Resources');
  RemoveDir(Root + '\Contents');
  RemoveDir(Root);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Detail: String;
begin
  Result := '';
  ExtractTemporaryFile('patchslide.sqlite');
  ExtractTemporaryFile('patchslide_search.sqlite');
  ExtractTemporaryFile('manifest.json');
  // Use the staged helper before the application directory exists.
  ExtractTemporaryFile('Manage-InstallerData.ps1');
  DataHelper := ExpandConstant('{tmp}\Manage-InstallerData.ps1');
  if not RunData('InstallDb', ExpandConstant('{tmp}'), '', Detail) then
    Result := CustomMessage('CloseDatabaseUsers') + #13#10 + Detail;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssInstall) and not WizardIsComponentSelected('vst3') then
    RemoveDeselectedVst3;
end;

function InitializeUninstall: Boolean;
begin
  // Silent removal cannot express the two independent data choices.
  Result := not UninstallSilent;
end;

procedure SelectAndRemoveData;
var
  Form: TSetupForm;
  Info: TNewStaticText;
  Accept, CancelButton: TNewButton;
  Detail: String;
  CanResolve: Boolean;
begin
  DataHelper := ExpandConstant('{tmp}\Manage-InstallerData.ps1');
  if not FileCopy(ExpandConstant('{app}\installer\Manage-InstallerData.ps1'), DataHelper, False) then
    RaiseException(CustomMessage('MissingHelper'));
  CanResolve := RunData('GetUserPath', '', '', Detail);
  if CanResolve then UserDataPath := Trim(Detail) else UserDataPath := '';
  Form := CreateCustomForm(ScaleX(620), ScaleY(300), False, False);
  try
    Form.Caption := CustomMessage('DataRemovalTitle');
    Info := TNewStaticText.Create(Form);
    Info.Parent := Form;
    Info.SetBounds(ScaleX(16), ScaleY(16), ScaleX(586), ScaleY(100));
    Info.AutoSize := False;
    Info.WordWrap := True;
    Info.Caption := CustomMessage('DataRemovalInfo') + #13#10 +
      FmtMessage(CustomMessage('UserDataPath'), [UserDataPath]) + #13#10 +
      FmtMessage(CustomMessage('SharedDatabasePath'), [ExpandConstant('{commonappdata}\OPLL_PatchSlide_DB')]);
    if not CanResolve then Info.Caption := Info.Caption + #13#10 + FmtMessage(CustomMessage('UserDeletionUnavailable'), [Detail]);
    RemoveUser := TNewCheckBox.Create(Form);
    RemoveUser.Parent := Form;
    RemoveUser.SetBounds(ScaleX(16), ScaleY(125), ScaleX(586), ScaleY(30));
    RemoveUser.Caption := CustomMessage('DeleteUserData');
    RemoveUser.Checked := False;
    RemoveUser.Enabled := CanResolve;
    RemoveShared := TNewCheckBox.Create(Form);
    RemoveShared.Parent := Form;
    RemoveShared.SetBounds(ScaleX(16), ScaleY(165), ScaleX(586), ScaleY(30));
    RemoveShared.Caption := CustomMessage('DeleteSharedDatabase');
    RemoveShared.Checked := True;
    Accept := TNewButton.Create(Form);
    Accept.Parent := Form;
    Accept.SetBounds(ScaleX(380), ScaleY(245), ScaleX(100), ScaleY(28));
    Accept.Caption := CustomMessage('Continue');
    Accept.Default := True;
    Accept.ModalResult := mrOk;
    CancelButton := TNewButton.Create(Form);
    CancelButton.Parent := Form;
    CancelButton.SetBounds(ScaleX(490), ScaleY(245), ScaleX(100), ScaleY(28));
    CancelButton.Caption := CustomMessage('Cancel');
    CancelButton.Cancel := True;
    CancelButton.ModalResult := mrCancel;
    if Form.ShowModal <> mrOk then Abort;
    if RemoveUser.Checked then begin
      if MsgBox(CustomMessage('ConfirmDeleteUser') + #13#10 +
        UserDataPath + #13#10 + CustomMessage('ConfirmDeleteUserContents'), mbConfirmation, MB_YESNO or MB_DEFBUTTON2) <> IDYES then Abort;
    end;
    if RemoveShared.Checked then begin
      if MsgBox(CustomMessage('ConfirmDeleteShared'),
        mbConfirmation, MB_YESNO or MB_DEFBUTTON2) <> IDYES then Abort;
    end;
    if RemoveShared.Checked then
      if not RunData('RemoveDb', '', '', Detail) then begin
        MsgBox(Detail, mbError, MB_OK);
        Abort;
      end;
    if RemoveUser.Checked then
      if not RunData('RemoveUserData', '', UserDataPath, Detail) then begin
        MsgBox(Detail, mbError, MB_OK);
        Abort;
      end;
  finally
    Form.Free;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then SelectAndRemoveData;
end;
