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
; the .klf association ([Registry]): Explorer is told to reload it after install and uninstall
ChangesAssociations=yes
LicenseFile={#SourcePath}\..\LICENSE

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; Flags: unchecked
Name: "klfassoc"; Description: "{cm:AssocFileExtension,{#MyAppName},.klf}"

[Registry]
; A double-click on a .klf recording opens it here (the app opens its first argument). The ProgId is this app's
; own, so the official KoggerApp's (KoggerApp.klf) is left alone; HKA is HKCU or HKLM by the install mode. A choice
; the user made in Windows (UserChoice) still wins over the default set here: "Open with" picks this app then.
Root: HKA; Subkey: "Software\Classes\.klf"; ValueType: string; ValueName: ""; ValueData: "KoggerAppME.klf"; Flags: uninsdeletevalue; Tasks: klfassoc
Root: HKA; Subkey: "Software\Classes\.klf\OpenWithProgids"; ValueType: string; ValueName: "KoggerAppME.klf"; ValueData: ""; Flags: uninsdeletevalue; Tasks: klfassoc
Root: HKA; Subkey: "Software\Classes\KoggerAppME.klf"; ValueType: string; ValueName: ""; ValueData: "Kogger log file (KLF)"; Flags: uninsdeletekey; Tasks: klfassoc
Root: HKA; Subkey: "Software\Classes\KoggerAppME.klf\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExe},0"; Tasks: klfassoc
Root: HKA; Subkey: "Software\Classes\KoggerAppME.klf\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExe}"" ""%1"""; Tasks: klfassoc

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
