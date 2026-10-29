; Sovereign's installer (Inno Setup 6), built by .github/workflows/release.yml:
;
;   ISCC /DAppVersion=0.2.0 /DSourceDir=<staged files> /DOutputDir=<out> installer\sovereign.iss
;
; UTF-8 with a BOM: Inno Setup reads the Russian strings right only then.
;
; Installs into Program Files, registers the service (sovereign-core.exe
; --install: auto start, restart on failure, the ETW recorder) and starts it,
; puts the tray in the Start menu and starts it for the user. The same file
; updates an installed Sovereign - the tray's updater runs it with /SILENT
; (UAC), or the service does, /VERYSILENT /SESSION=<id> (no UAC: SelfUpdate,
; once the tray finds the computer quiet): the tray and the service are
; stopped first (they hold the files), started again after. Uninstalling stops and removes both; the user's settings in
; %LOCALAPPDATA%\Sovereign and the logs in %ProgramData%\Sovereign stay.

#ifndef AppVersion
  #error Pass /DAppVersion=MAJOR.MINOR.PATCH
#endif
#ifndef SourceDir
  #error Pass /DSourceDir=<the staged files>
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
; Never change AppId: it is how an update finds the installed Sovereign.
AppId={{6A4E1C3B-5D2F-4B8E-9C7A-1F0E3D2B4A61}
AppName=Sovereign
AppVersion={#AppVersion}
AppVerName=Sovereign {#AppVersion}
AppPublisher=sovereignbrains
AppPublisherURL=https://github.com/sovereignbrains/Sovereign
AppSupportURL=https://github.com/sovereignbrains/Sovereign/issues
AppUpdatesURL=https://github.com/sovereignbrains/Sovereign/releases
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\Sovereign
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir={#OutputDir}
OutputBaseFilename=Sovereign-Setup-{#AppVersion}
SetupIconFile=..\assets\sovereign.ico
UninstallDisplayIcon={app}\sovereign-tray.exe
UninstallDisplayName=Sovereign
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=force
RestartApplications=no

[Languages]
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs

[Icons]
Name: "{autoprograms}\Sovereign"; Filename: "{app}\sovereign-tray.exe"
Name: "{autodesktop}\Sovereign"; Filename: "{app}\sovereign-tray.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\sovereign-core.exe"; Parameters: "--install"; Flags: runhidden waituntilterminated; StatusMsg: "Регистрация службы…"
Filename: "{sys}\sc.exe"; Parameters: "start SovereignCore"; Flags: runhidden waituntilterminated; StatusMsg: "Запуск службы…"
; Runs in silent mode too (no skipifsilent): an update brings the tray back.
; Through explorer.exe, so the tray runs at the shell's level, not elevated -
; the updater starts this setup elevated directly, and runasoriginaluser has
; no original user to fall back to then.
Filename: "{win}\explorer.exe"; Parameters: """{app}\sovereign-tray.exe"""; Description: "Запустить Sovereign"; Flags: postinstall nowait; Check: not FromService
; Run by the service (SelfUpdate, /SESSION=<id>): this setup is SYSTEM's,
; explorer.exe would start the tray in no one's session - the core puts it
; back in the user's, as the user.
Filename: "{app}\sovereign-core.exe"; Parameters: "--launch-tray {param:SESSION}"; Flags: runhidden nowait; Check: FromService

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/IM sovereign-tray.exe /F"; Flags: runhidden waituntilterminated; RunOnceId: "StopTray"
Filename: "{app}\sovereign-core.exe"; Parameters: "--uninstall"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveService"
Filename: "{sys}\reg.exe"; Parameters: "delete HKCU\Software\Microsoft\Windows\CurrentVersion\Run /v Sovereign /f"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveAutostart"

[UninstallDelete]
; Whatever else turned up in the program's folder.
Type: filesandordirs; Name: "{app}"

[Code]
// The service runs this setup for an update while nobody is at the computer
// (src/service/self_update_win.cpp), with the session to bring the tray back to.
function FromService(): Boolean;
var
  Session: String;
  I: Integer;
begin
  Session := ExpandConstant('{param:SESSION|}');
  Result := Session <> '';
  for I := 1 to Length(Session) do
    if (Session[I] < '0') or (Session[I] > '9') then
      Result := False;
end;

// Before any file is replaced: the tray (in the user's session) and the
// service hold sovereign-*.exe and the DLLs. The tray first gets a normal
// close (its window quits and takes the icon away), then a forced one.
procedure StopRunning();
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/IM sovereign-tray.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Sleep(1500);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/IM sovereign-tray.exe /F', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  // net stop waits until the service has stopped (or says it isn't running).
  Exec(ExpandConstant('{sys}\net.exe'), 'stop SovereignCore', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopRunning();
  Result := '';
end;

// Nothing left behind, as Revo Uninstaller's leftover scan would find it
// (16.10.2026): the logs, the box's cache, the lists, the updates downloaded,
// Task Manager's autostart mark - always; the subscriptions and configs (the
// servers' keys) when the user says so, yes by default - "no" keeps them for
// a reinstall. Silent: all of it. An update never runs the uninstaller.
var
  KeepSettings: Boolean;

procedure DeleteLeftovers();
begin
  // After --uninstall: the ETW session is stopped, its .etl files free.
  DelTree(ExpandConstant('{commonappdata}\Sovereign'), True, True, True);
  DelTree(AddBackslash(GetTempDir()) + 'Sovereign-Update', True, True, True);
  RegDeleteValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run', 'Sovereign');
  if not KeepSettings then
    DelTree(ExpandConstant('{localappdata}\Sovereign'), True, True, True);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  // Asked before anything goes, not after: the answer is all that's waited for.
  if CurUninstallStep = usUninstall then
    // Silent: no question - and no MsgBox evaluated for it.
    if UninstallSilent then
      KeepSettings := False
    else
      KeepSettings := (MsgBox('Удалить также подписки, конфигурации и настройки Sovereign?' + #13#10#13#10 +
                      'Нет — оставить их на случай переустановки.', mbConfirmation, MB_YESNO) = IDNO);
  if CurUninstallStep = usPostUninstall then
    DeleteLeftovers();
end;
