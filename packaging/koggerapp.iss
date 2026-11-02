; Inno Setup script for KoggerApp ME (was "KoggerApp myEdition" until 2026-10-18).
;
; Not meant to be opened and compiled by hand: scripts\win_package_release.ps1
; supplies every define and then calls ISCC. Compiling it directly fails on the
; missing defines, and that is deliberate - the version stamped into the
; installer and the payload staged on disk have to come from the same run.

#ifndef MyAppVersion
  #error Define MyAppVersion (scripts\win_package_release.ps1 passes it)
#endif
#ifndef MyAppVersionInfo
  #error Define MyAppVersionInfo (four numeric fields, for the Windows file version)
#endif
#ifndef PayloadDir
  #error Define PayloadDir (the deployed tree to package)
#endif
#ifndef OutputDir
  #error Define OutputDir
#endif
#ifndef OutputBase
  #error Define OutputBase (installer file name without .exe)
#endif
#ifndef IconFile
  #error Define IconFile
#endif

#define MyAppName "KoggerApp ME"
#define MyAppExe  "KoggerApp.exe"

[Setup]
; Never reuse this GUID in another product: it is what lets an upgrade replace
; an earlier install instead of piling a second copy beside it.
AppId={{713D2AC7-A546-48B4-BA22-8BA3CDC138CF}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
VersionInfoVersion={#MyAppVersionInfo}
VersionInfoProductName={#MyAppName}
DefaultDirName={autopf}\KoggerApp ME
; The old name's folder and Start-menu group are not reused: an upgrade goes to the new ones and
; removes the old (InstallDelete). Seen 2026-10-18: without UsePreviousGroup=no the new shortcuts
; went into a re-created "KoggerApp myEdition" group.
UsePreviousAppDir=no
UsePreviousGroup=no
DefaultGroupName={#MyAppName}
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={app}\{#MyAppExe}
SetupIconFile={#IconFile}
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBase}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; "lowest" plus the override keeps the common case free of a UAC prompt: it
; installs under the user's own Programs directory, and anyone who wants it in
; Program Files for every user can still say so in the wizard.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
DisableProgramGroupPage=yes
AllowNoIcons=yes
; the file associations ([Registry]): Explorer is told to reload them after install and uninstall
ChangesAssociations=yes
LicenseFile={#SourcePath}\..\LICENSE

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; Flags: unchecked
; One task per type the app opens from its first argument: an upgrade remembers a choice by the task's name, and a
; task it has not seen yet starts at its default. u-blox logs are u-center's first, so .ubx only when asked.
Name: "klfassoc"; Description: "{cm:AssocFileExtension,{#MyAppName},.klf}"
Name: "kmapassoc"; Description: "{cm:AssocFileExtension,{#MyAppName},.kmap}"
Name: "xtfassoc"; Description: "{cm:AssocFileExtension,{#MyAppName},.xtf}"
Name: "ubxassoc"; Description: "{cm:AssocFileExtension,{#MyAppName},.ubx}"; Flags: unchecked

[Registry]
; A double-click on a recording or a map opens it here (the app opens its first argument; a .kmap goes to My
; maps). The ProgIds are this app's own, so the official KoggerApp's (KoggerApp.klf) are left alone; HKA is HKCU
; or HKLM by the install mode. The ProgIds and the "Open with" entries are written whatever the tasks; a task
; makes its type open here by default. A choice the user made in Windows (UserChoice) still wins over that
; default: "Open with" picks this app then.
Root: HKA; Subkey: "Software\Classes\KoggerAppME.klf"; ValueType: string; ValueName: ""; ValueData: "Kogger log file (KLF)"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\KoggerAppME.klf\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExe},0"
Root: HKA; Subkey: "Software\Classes\KoggerAppME.klf\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExe}"" ""%1"""
Root: HKA; Subkey: "Software\Classes\.klf\OpenWithProgids"; ValueType: string; ValueName: "KoggerAppME.klf"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.klf"; ValueType: string; ValueName: ""; ValueData: "KoggerAppME.klf"; Flags: uninsdeletevalue; Tasks: klfassoc
Root: HKA; Subkey: "Software\Classes\KoggerAppME.kmap"; ValueType: string; ValueName: ""; ValueData: "KOGGER map (KMAP)"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\KoggerAppME.kmap\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExe},0"
Root: HKA; Subkey: "Software\Classes\KoggerAppME.kmap\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExe}"" ""%1"""
Root: HKA; Subkey: "Software\Classes\.kmap\OpenWithProgids"; ValueType: string; ValueName: "KoggerAppME.kmap"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.kmap"; ValueType: string; ValueName: ""; ValueData: "KoggerAppME.kmap"; Flags: uninsdeletevalue; Tasks: kmapassoc
Root: HKA; Subkey: "Software\Classes\KoggerAppME.xtf"; ValueType: string; ValueName: ""; ValueData: "Sonar recording (XTF)"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\KoggerAppME.xtf\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExe},0"
Root: HKA; Subkey: "Software\Classes\KoggerAppME.xtf\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExe}"" ""%1"""
Root: HKA; Subkey: "Software\Classes\.xtf\OpenWithProgids"; ValueType: string; ValueName: "KoggerAppME.xtf"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.xtf"; ValueType: string; ValueName: ""; ValueData: "KoggerAppME.xtf"; Flags: uninsdeletevalue; Tasks: xtfassoc
Root: HKA; Subkey: "Software\Classes\KoggerAppME.ubx"; ValueType: string; ValueName: ""; ValueData: "u-blox GNSS log (UBX)"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\KoggerAppME.ubx\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExe},0"
Root: HKA; Subkey: "Software\Classes\KoggerAppME.ubx\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExe}"" ""%1"""
Root: HKA; Subkey: "Software\Classes\.ubx\OpenWithProgids"; ValueType: string; ValueName: "KoggerAppME.ubx"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.ubx"; ValueType: string; ValueName: ""; ValueData: "KoggerAppME.ubx"; Flags: uninsdeletevalue; Tasks: ubxassoc

[InstallDelete]
; The same product (AppId) under its name until 2026-10-18: its default folder and shortcuts.
Type: filesandordirs; Name: "{autopf}\KoggerApp myEdition"
Type: filesandordirs; Name: "{autoprograms}\KoggerApp myEdition"
Type: files; Name: "{autodesktop}\KoggerApp myEdition.lnk"

[Files]
Source: "{#PayloadDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExe}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent
