; YouShadow Inno Setup 스크립트. scripts\package.ps1 이 /DAppVersion /DStageDir /DOutDir 를 넘겨 컴파일한다.
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  #define StageDir "..\dist\YouShadow"
#endif
#ifndef OutDir
  #define OutDir "..\dist"
#endif

[Setup]
AppId={{7E1D3C2A-5B7F-4C1E-9C55-0A1B2C3D4E5F}
AppName=YouShadow
AppVersion={#AppVersion}
AppVerName=YouShadow v{#AppVersion}
AppPublisher=juzerokr
AppPublisherURL=https://github.com/JuZeroKR/YouShadow
AppSupportURL=https://github.com/JuZeroKR/YouShadow/issues
DefaultDirName={autopf}\YouShadow
DefaultGroupName=YouShadow
; 관리자 권한 없이 사용자 폴더(%LOCALAPPDATA%\Programs)에 설치. 서명 없는 앱에 UAC 창을 띄우지 않기 위해서다.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
OutputDir={#OutDir}
OutputBaseFilename=YouShadow-Setup-v{#AppVersion}
SetupIconFile=..\assets\youshadow.ico
UninstallDisplayIcon={app}\YouShadow.exe
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile=..\LICENSE

[Languages]
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\YouShadow"; Filename: "{app}\YouShadow.exe"
Name: "{group}\학습 데이터 폴더"; Filename: "{localappdata}\YouShadow"
Name: "{autodesktop}\YouShadow"; Filename: "{app}\YouShadow.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\YouShadow.exe"; Description: "{cm:LaunchProgram,YouShadow}"; Flags: nowait postinstall skipifsilent

; 제거해도 학습 데이터(%LOCALAPPDATA%\YouShadow)는 남긴다
