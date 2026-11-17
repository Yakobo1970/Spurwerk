; Spurwerk Setup - Inno Setup Skript
;
; Installiert das VST3-Plugin nach C:\Program Files\Common Files\VST3 und das
; Standalone-Programm nach C:\Program Files\Spurwerk (beides waehlbar).
;
; Vorher Release bauen:
;   cmake --build build --config Release --target Spurwerk_VST3 Spurwerk_Standalone
; Dann:
;   "C:\Program Files\Inno Setup 7\ISCC.exe" installer\Spurwerk.iss

#define AppName "Spurwerk"
#define AppVersion "1.0.1"
#define AppPublisher "Kay 'Yakobo' Fiedler (Yakobo1970)"
#define AppURL "https://www.undabtanzbar.de/software/spurwerk/"
#define AppExeName "Spurwerk.exe"
#define BuildDir "..\build\Spurwerk_artefacts\Release"
#define PlayerDir "..\build\SpurwerkPlayer_artefacts\Release"

[Setup]
AppId={{E4D3D095-F578-4B89-9758-4253A99EE0A0}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
AppUpdatesURL=https://github.com/Yakobo1970/Spurwerk/releases
AppComments=Tracker fuer MOD, XM, S3M, IT, MPTM, MTM, 669, FAR, MDL, OctaMED, Future Composer, TCB, OKT, YM und COSO mit Modul-Archiv - VST3 und Standalone, dazu Spurwerk Player
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\{#AppExeName}
OutputDir=..\dist_installer
OutputBaseFilename=Spurwerk-{#AppVersion}-Setup
Compression=lzma2
SolidCompression=yes
; Der VST3-Ordner unter Common Files braucht Administratorrechte.
PrivilegesRequired=admin
LicenseFile=..\LICENSE
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=yes
SetupIconFile=Spurwerk.ico

[Languages]
Name: "german"; MessagesFile: "compiler:Languages\German.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full"; Description: "VST3 + Standalone + Spurwerk Player"
Name: "custom"; Description: "Benutzerdefiniert"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3-Plugin (C:\Program Files\Common Files\VST3)"; Types: full custom
Name: "standalone"; Description: "Standalone-Programm"; Types: full custom
Name: "player"; Description: "Spurwerk Player (kleiner Sample-Player, VST3)"; Types: full custom

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Components: standalone

[Files]
Source: "{#BuildDir}\VST3\Spurwerk.vst3\*"; DestDir: "{commoncf64}\VST3\Spurwerk.vst3"; Components: vst3; Flags: recursesubdirs createallsubdirs ignoreversion
Source: "{#PlayerDir}\VST3\Spurwerk Player.vst3\*"; DestDir: "{commoncf64}\VST3\Spurwerk Player.vst3"; Components: player; Flags: recursesubdirs createallsubdirs ignoreversion
Source: "{#BuildDir}\Standalone\{#AppExeName}"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; DestName: "THIRD_PARTY_NOTICES.txt"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Components: standalone
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent; Components: standalone
