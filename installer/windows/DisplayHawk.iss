; Display Hawk installer for Windows (Inno Setup 6).
;   iscc /DAppVersion=0.1.0 /DSourceDir=<deployed folder> /DOutputDir=<where the setup goes> DisplayHawk.iss
; SourceDir holds DisplayHawk.exe after windeployqt (with vc_redist.x64.exe beside it).

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\dist"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\installer-out"
#endif

[Setup]
AppId={{6F1D2B6A-3C51-4E7B-9C1E-5A0D7B2F4E11}
AppName=Display Hawk
AppVersion={#AppVersion}
AppVerName=Display Hawk {#AppVersion}
AppPublisher=Display Hawk
AppCopyright=Copyright (c) 2026 Kane McNamara. MIT licence.
LicenseFile=..\..\LICENSE
DefaultDirName={autopf}\Display Hawk
DefaultGroupName=Display Hawk
UninstallDisplayIcon={app}\DisplayHawk.exe
OutputDir={#OutputDir}
OutputBaseFilename=DisplayHawk-{#AppVersion}-windows-x64-setup
SetupIconFile=..\..\assets\icon\displayhawk.ico
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
; Installs for everyone by default; the user can choose just themselves (no admin rights needed).
PrivilegesRequiredOverridesAllowed=dialog
ChangesAssociations=yes
CloseApplications=yes

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "association"; Description: "Open Display Hawk setups (.dhawk) with Display Hawk"; GroupDescription: "File types:"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "vc_redist*.exe"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall skipifsourcedoesntexist

[Icons]
Name: "{group}\Display Hawk"; Filename: "{app}\DisplayHawk.exe"
Name: "{group}\Uninstall Display Hawk"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Display Hawk"; Filename: "{app}\DisplayHawk.exe"; Tasks: desktopicon

[Registry]
Root: HKA; Subkey: "Software\Classes\.dhawk"; ValueType: string; ValueName: ""; ValueData: "DisplayHawk.Setup"; Flags: uninsdeletevalue; Tasks: association
Root: HKA; Subkey: "Software\Classes\DisplayHawk.Setup"; ValueType: string; ValueName: ""; ValueData: "Display Hawk setup"; Flags: uninsdeletekey; Tasks: association
Root: HKA; Subkey: "Software\Classes\DisplayHawk.Setup\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\DisplayHawk.exe,0"; Tasks: association
Root: HKA; Subkey: "Software\Classes\DisplayHawk.Setup\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\DisplayHawk.exe"" ""%1"""; Tasks: association

[Run]
; The Visual C++ runtime, when the machine does not have it yet.
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing the Visual C++ runtime..."; Check: NeedsVcRuntime; Flags: waituntilterminated skipifdoesntexist
Filename: "{app}\DisplayHawk.exe"; Description: "{cm:LaunchProgram,Display Hawk}"; Flags: nowait postinstall skipifsilent

[Code]
function NeedsVcRuntime: Boolean;
var
  Installed: Cardinal;
begin
  Result := not (RegQueryDWordValue(HKLM64, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Installed', Installed)
                 and (Installed = 1));
end;
