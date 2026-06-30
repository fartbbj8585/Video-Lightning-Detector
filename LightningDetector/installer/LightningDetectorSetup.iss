; LightningDetectorSetup.iss
; Inno Setup script - produces a single self-contained installer
; (LightningDetectorSetup.exe) that needs NOTHING pre-installed on the
; target PC: every DLL the app needs (OpenCV, FFmpeg, etc.) is bundled,
; and the .exe itself was built with a statically-linked CRT, so the
; Visual C++ Redistributable is not required either.
;
; HOW TO BUILD THE INSTALLER (one-time, on the developer's machine):
;   1. Build the app normally:        .\build.ps1
;   2. Install Inno Setup (free):     winget install JRSoftware.InnoSetup
;   3. Compile this script:           iscc installer\LightningDetectorSetup.iss
;   4. The installer appears at:      installer\Output\LightningDetectorSetup.exe
;
; Anyone you give that single .exe to can just double-click it - no
; internet connection, no separate downloads, no manual DLL copying.

#define MyAppName "Lightning Detector"
#define MyAppVersion "1.0"
#define MyAppPublisher "Lightning Detector"
#define MyAppExeName "LightningDetector.exe"
; Folder produced by build.ps1, relative to this script's location
#define BuildOutDir "..\build\Release"

[Setup]
AppId={{B6F2B9B1-7B5E-4B33-9B3B-8F2C2E6E2B11}}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputBaseFilename=LightningDetectorSetup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
UninstallDisplayIcon={app}\{#MyAppExeName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"

[Files]
; The exe and every DLL build.ps1 copied next to it - bundled wholesale so
; nothing needs to be downloaded separately on the target machine.
Source: "{#BuildOutDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildOutDir}\*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion isreadme

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName} now"; Flags: nowait postinstall skipifsilent
