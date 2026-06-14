; LitePDF installer — Phase 10. Per-user default, opt-in per-machine.
; Build:  ISCC.exe /DMyAppVersion=0.0.12 installer\litepdf.iss
; Output: installer\Output\litepdf-setup-0.0.12.exe

#ifndef MyAppVersion
  #error "MyAppVersion must be passed: ISCC /DMyAppVersion=x.y.z"
#endif

#define MyAppName "LitePDF"
#define MyAppExeName "litepdf.exe"
#define MyAppPublisher "LitePDF"
#define MyAppURL "https://github.com/jeffchen1981-fu/litepdf"

[Setup]
; AppId is the stable identity key — generated once, NEVER change it.
AppId={{62012304-133C-41C1-98E8-CCA248396FFF}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases
; {autopf} is install-mode-aware: per-user -> %LOCALAPPDATA%\Programs, per-machine
; -> Program Files. A fixed {localappdata} path would mis-place a per-machine
; install (HKLM registry pointing at one user's profile; unusable by others).
DefaultDirName={autopf}\LitePDF
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
; Per-user by default (no UAC); allow opt-in elevation to per-machine.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
UsePreviousPrivileges=yes
; 64-bit only (the exe is built -A x64).
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Win10 1903+ (Direct2D/DWrite + ms-settings: URI).
MinVersion=10.0.18362
; Offer to close a running instance before overwriting the in-use exe.
AppMutex=Local\LitePDF_SingleInstance_v1
CloseApplications=yes
RestartApplications=no
OutputDir=Output
OutputBaseFilename=litepdf-setup-{#MyAppVersion}
SetupIconFile=..\assets\icon\litepdf-app.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
; Informational disclosure page (NO agree/disagree radios by design).
InfoBeforeFile=LICENSE-DISPLAY.rtf
; --- Code signing hook (disabled — app is unsigned in Phase 10; see README
;     SmartScreen note). To enable later: configure a SignTool in Inno and
;     uncomment the next line.
; SignTool=mysigntool $f

[Languages]
Name: "zh_tw"; MessagesFile: "lang\ChineseTraditional.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "assocpdf"; Description: "設為 .pdf 預設程式 (稍後在 Windows 設定中確認)"; Flags: unchecked
Name: "assocothers"; Description: "關聯 .epub / .cbz / .xps"; Flags: unchecked
Name: "contextmenu"; Description: "新增「以 LitePDF 開啟」右鍵選單"; Flags: unchecked

[Files]
Source: "..\build\Release\litepdf.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; HKA = HKEY_AUTO: resolves to HKCU (per-user) or HKLM (per-machine) by scope.
; --- PDF ProgID (red IDI_PDFDOC icon via negative resource id -102) ---
Root: HKA; Subkey: "Software\Classes\LitePDF.pdf"; ValueType: string; ValueName: ""; ValueData: "LitePDF PDF Document"; Flags: uninsdeletekey; Tasks: assocpdf
Root: HKA; Subkey: "Software\Classes\LitePDF.pdf\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},-102"; Tasks: assocpdf
Root: HKA; Subkey: "Software\Classes\LitePDF.pdf\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Tasks: assocpdf
Root: HKA; Subkey: "Software\Classes\.pdf\OpenWithProgids"; ValueType: string; ValueName: "LitePDF.pdf"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assocpdf
; --- ePub / CBZ / XPS ProgIDs (default app icon -101) ---
Root: HKA; Subkey: "Software\Classes\LitePDF.epub"; ValueType: string; ValueName: ""; ValueData: "LitePDF ePub Document"; Flags: uninsdeletekey; Tasks: assocothers
Root: HKA; Subkey: "Software\Classes\LitePDF.epub\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},-101"; Tasks: assocothers
Root: HKA; Subkey: "Software\Classes\LitePDF.epub\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Tasks: assocothers
Root: HKA; Subkey: "Software\Classes\.epub\OpenWithProgids"; ValueType: string; ValueName: "LitePDF.epub"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assocothers
Root: HKA; Subkey: "Software\Classes\LitePDF.cbz"; ValueType: string; ValueName: ""; ValueData: "LitePDF Comic Archive"; Flags: uninsdeletekey; Tasks: assocothers
Root: HKA; Subkey: "Software\Classes\LitePDF.cbz\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},-101"; Tasks: assocothers
Root: HKA; Subkey: "Software\Classes\LitePDF.cbz\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Tasks: assocothers
Root: HKA; Subkey: "Software\Classes\.cbz\OpenWithProgids"; ValueType: string; ValueName: "LitePDF.cbz"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assocothers
Root: HKA; Subkey: "Software\Classes\LitePDF.xps"; ValueType: string; ValueName: ""; ValueData: "LitePDF XPS Document"; Flags: uninsdeletekey; Tasks: assocothers
Root: HKA; Subkey: "Software\Classes\LitePDF.xps\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},-101"; Tasks: assocothers
Root: HKA; Subkey: "Software\Classes\LitePDF.xps\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Tasks: assocothers
Root: HKA; Subkey: "Software\Classes\.xps\OpenWithProgids"; ValueType: string; ValueName: "LitePDF.xps"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assocothers
; --- Default Programs capabilities (so LitePDF appears in Settings > Default apps) ---
Root: HKA; Subkey: "Software\LitePDF\Capabilities"; ValueType: string; ValueName: "ApplicationName"; ValueData: "LitePDF"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\LitePDF\Capabilities"; ValueType: string; ValueName: "ApplicationDescription"; ValueData: "Lightweight PDF / ePub / CBZ / XPS reader"
Root: HKA; Subkey: "Software\LitePDF\Capabilities\FileAssociations"; ValueType: string; ValueName: ".pdf"; ValueData: "LitePDF.pdf"; Tasks: assocpdf
Root: HKA; Subkey: "Software\LitePDF\Capabilities\FileAssociations"; ValueType: string; ValueName: ".epub"; ValueData: "LitePDF.epub"; Tasks: assocothers
Root: HKA; Subkey: "Software\LitePDF\Capabilities\FileAssociations"; ValueType: string; ValueName: ".cbz"; ValueData: "LitePDF.cbz"; Tasks: assocothers
Root: HKA; Subkey: "Software\LitePDF\Capabilities\FileAssociations"; ValueType: string; ValueName: ".xps"; ValueData: "LitePDF.xps"; Tasks: assocothers
Root: HKA; Subkey: "Software\RegisteredApplications"; ValueType: string; ValueName: "LitePDF"; ValueData: "Software\LitePDF\Capabilities"; Flags: uninsdeletevalue
; Own the whole Software\LitePDF subtree; remove it wholesale on uninstall.
; (uninsdeletekeyifempty would no-op here: LIFO uninstall evaluates the parent
;  before its Capabilities child is removed, so it's never empty at that point.)
Root: HKA; Subkey: "Software\LitePDF"; Flags: uninsdeletekey
; Per-machine cleanup gap: HKA above resolves to HKLM for an all-users install,
; but the app's runtime MRU (core::MruList) is ALWAYS written under HKCU. So an
; all-users uninstall removes HKLM\Software\LitePDF yet leaves the user's
; HKCU\Software\LitePDF\MRU orphaned (reproduced in Windows Sandbox 2026-06-03).
; This explicit HKCU entry deletes that subtree on uninstall. dontcreatekey: do
; NOT plant an empty key at install time (the app creates it at runtime); only
; remove it on uninstall if present. Known limitation: a per-machine install
; whose uninstall runs under a DIFFERENT user than the one who populated the MRU
; cannot be cleaned from here -- a machine-wide uninstall has no handle on another
; profile's HKCU.
Root: HKCU; Subkey: "Software\LitePDF"; Flags: uninsdeletekey dontcreatekey
; --- Context menu "Open with LitePDF" for .pdf via SystemFileAssociations ---
; Registered under SystemFileAssociations\.pdf (not the private LitePDF.pdf
; ProgID), so the verb shows for every .pdf regardless of the current default
; app, works without the assocpdf task, and is self-contained: uninstall removes
; only this key (no orphaned LitePDF.pdf ProgID left behind).
Root: HKA; Subkey: "Software\Classes\SystemFileAssociations\.pdf\shell\openWithLitePDF"; ValueType: string; ValueName: ""; ValueData: "以 LitePDF 開啟"; Flags: uninsdeletekey; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\SystemFileAssociations\.pdf\shell\openWithLitePDF\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Tasks: contextmenu

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent
; runasoriginaluser: in a per-machine (elevated) install, open Settings in the
; invoking user's session, not the admin context. Best-effort deep-link to LitePDF.
Filename: "ms-settings:defaultapps?registeredAppUser=LitePDF"; Description: "在 Windows 設定中將 LitePDF 設為預設"; Flags: shellexec runasoriginaluser postinstall skipifsilent; Tasks: assocpdf

[Code]
procedure SHChangeNotify(wEventId: Integer; uFlags: Cardinal; dwItem1, dwItem2: Cardinal);
  external 'SHChangeNotify@shell32.dll stdcall';

procedure CurStepChanged(CurStep: TSetupStep);
begin
  // SHCNE_ASSOCCHANGED = $08000000, SHCNF_IDLIST = $0000.
  // Refresh Explorer's icon/assoc cache so .pdf icons update without logoff.
  if CurStep = ssPostInstall then
    SHChangeNotify($08000000, $0000, 0, 0);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ConfigDir: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    // Prompt whether to keep user config. Default = No (keep): MB_DEFBUTTON2.
    ConfigDir := ExpandConstant('{localappdata}\LitePDF');
    if DirExists(ConfigDir) then
    begin
      if MsgBox('要一併刪除 LitePDF 的設定、工作階段與當機記錄嗎?' + #13#10 + ConfigDir + #13#10#13#10 +
                '選「否」會保留這些資料 (預設)。', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
        DelTree(ConfigDir, True, True, True);
    end;
    // Refresh the shell after removing associations.
    SHChangeNotify($08000000, $0000, 0, 0);
  end;
end;
