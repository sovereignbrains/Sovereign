; Sovereign's installer (Inno Setup 6), built by .github/workflows/release.yml:
;
;   ISCC /DAppVersion=0.2.0 /DSourceDir=<staged files> /DOutputDir=<out> installer\sovereign.iss
;
; UTF-8 with a BOM: Inno Setup reads the Russian strings right only then.
;
; Installs into Program Files, registers the service (sovereign-core.exe
; --install: auto start, restart on failure, the ETW recorder) and starts it,
; puts the tray in the Start menu and starts it for the user. The same file
; updates an installed Sovereign - the tray's updater runs it with /SILENT:
; the tray and the service are stopped first (they hold the files), started
; again after. Uninstalling stops and removes both; the user's settings in
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
Filename: "{win}\explorer.exe"; Parameters: """{app}\sovereign-tray.exe"""; Description: "Запустить Sovereign"; Flags: postinstall nowait

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/IM sovereign-tray.exe /F"; Flags: runhidden waituntilterminated; RunOnceId: "StopTray"
Filename: "{app}\sovereign-core.exe"; Parameters: "--uninstall"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveService"
Filename: "{sys}\reg.exe"; Parameters: "delete HKCU\Software\Microsoft\Windows\CurrentVersion\Run /v Sovereign /f"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveAutostart"

[Code]
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
