; ─────────────────────────────────────────────────────────────────────────
;  Inno Setup скрипт установщика Starostin VPN.
; ─────────────────────────────────────────────────────────────────────────

#define AppName "Starostin VPN"
#define AppVersion "1.0.1"
#define AppPublisher "Starostin"
#define AppExe "StarostinVPN.exe"

[Setup]
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\Starostin VPN
DefaultGroupName=Starostin VPN
DisableProgramGroupPage=yes
OutputDir=output
OutputBaseFilename=StarostinVPN-Setup
Compression=lzma2/max
SolidCompression=yes
PrivilegesRequired=admin
SetupIconFile=..\assets\app.ico
UninstallDisplayIcon={app}\{#AppExe}
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Свой AppId, не пересекается с AmSalesVPN — это другая программа,
; ставится в свою папку.
AppId={{D2E94A11-1B5C-4E37-AB99-7F45D8E3CC42}
CloseApplications=force
CloseApplicationsFilter=*.exe
RestartApplications=yes

[Languages]
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "Создать ярлык на рабочем столе"; GroupDescription: "Дополнительно:"
Name: "autostart"; Description: "Запускать при старте Windows"; GroupDescription: "Дополнительно:"; Flags: unchecked

[Files]
Source: "..\build\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\Starostin VPN"; Filename: "{app}\{#AppExe}"; IconFilename: "{app}\{#AppExe}"
Name: "{group}\Удалить Starostin VPN"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Starostin VPN"; Filename: "{app}\{#AppExe}"; IconFilename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "StarostinVPN"; ValueData: """{app}\{#AppExe}"""; Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{app}\{#AppExe}"; Description: "Запустить Starostin VPN"; Flags: postinstall skipifsilent shellexec runasoriginaluser nowait
Filename: "{app}\{#AppExe}"; Flags: shellexec nowait; Check: WizardSilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}\engine"
