; Inno Setup 6 script - NadirStation GK-2A HRIT Decoder
; Build with scripts\build_release.bat (it creates the "dist" folder first), or open this file in Inno Setup.
#define MyAppName "NadirStation GK-2A HRIT Decoder"
#define MyAppVersion "1.0.5"
#define MyAppPublisher "AGcov（BI2QYN）"
#define MyAppExe "NadirStation_GK2A-HRIT_decoder.exe"
#define DistDir "..\dist"

[Setup]
; The AppId identifies this program for upgrades and uninstall. Do not change it between versions.
AppId={{7C1E5A52-3B6D-4E4A-9C0B-5A1D8E2F6B91}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
VersionInfoVersion={#MyAppVersion}
VersionInfoCompany=AGcov (BI2QYN)
VersionInfoDescription={#MyAppName} Setup
DefaultDirName={autopf}\NadirStation_GK2A-HRIT_decoder
DefaultGroupName=NadirStation GK-2A HRIT Decoder
DisableProgramGroupPage=yes
LicenseFile=EULA.txt
OutputDir=output
OutputBaseFilename=NadirStation_GK2A-HRIT_decoder_Setup_{#MyAppVersion}
SetupIconFile=..\resources\icon\app.ico
UninstallDisplayIcon={app}\{#MyAppExe}
Compression=lzma2/ultra64
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
WizardStyle=modern
ShowLanguageDialog=auto
MinVersion=10.0

[Languages]
#if FileExists(CompilerPath + "Languages\ChineseSimplified.isl")
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
#elif FileExists(CompilerPath + "Languages\Unofficial\ChineseSimplified.isl")
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\Unofficial\ChineseSimplified.isl"
#elif FileExists(SourcePath + "ChineseSimplified.isl")
Name: "chinesesimplified"; MessagesFile: "ChineseSimplified.isl"
#endif
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#DistDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "EULA.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\NadirStation GK-2A HRIT Decoder"; Filename: "{app}\{#MyAppExe}"
Name: "{autodesktop}\NadirStation GK-2A HRIT Decoder"; Filename: "{app}\{#MyAppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExe}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
