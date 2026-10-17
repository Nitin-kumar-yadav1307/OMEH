; OMEH Windows installer - VS Code-style per-user setup.
;
; Built by CI (.github/workflows/build.yml): the two exes are copied next
; to this file, then ISCC.exe desktop\windows\installer.iss runs from the
; repo root - relative Source/OutputDir resolve against THIS script's
; folder (SourceDir), so no command-line defines are needed. Locally
; run_win.bat already drops both exes here: ISCC.exe installer.iss
; Optional: /DStageDir=<dir-with-the-exes> to stage from somewhere else.
;
; Design decisions (why it is shaped like VS Code's installer):
;   * PrivilegesRequired=lowest + {localappdata}\Programs -> no UAC prompt,
;     no admin rights, nothing written to Program Files.
;   * Shortcuts point at sync_gui.exe only: it spawns the engine itself
;     with CREATE_NO_WINDOW, so the user never sees a console window.
;   * Both exes are statically linked (static SDL2 + /MT, see
;     desktop/CMakeLists.txt), so these two files are the entire install -
;     no DLLs, no VC++ Redistributable, no post-install steps.

#define MyAppName "OMEH"
; Keep in sync with desktop/vcpkg.json version-string.
#define MyAppVersion "1.0.0"
#define MyAppPublisher "OMEH contributors"
#define MyAppURL "https://github.com/Nitin-kumar-yadav1307/OMEH"
#define MyAppExeName "sync_gui.exe"
#ifndef StageDir
  #define StageDir "."
#endif

[Setup]
; Stable AppId: later versions upgrade in place over the same directory.
AppId={{7B2F4C61-9E0A-4D5B-B1C8-5A3E9F70D246}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
VersionInfoVersion={#MyAppVersion}
DefaultDirName={localappdata}\Programs\{#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir={#StageDir}
OutputBaseFilename=OMEH-Setup-{#MyAppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName}
; No SetupIconFile: the repo ships no .ico, Inno's default icon is used.

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"

[Files]
Source: "{#StageDir}\sync_gui.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\pipewire_sync_win.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{userprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{userdesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[UninstallDelete]
; Created at runtime by the engine (per-device remembered trims) - not an
; installed file, so the uninstall log doesn't know about it.
Type: files; Name: "{app}\pipewire_sync_win.trims"

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent