#ifndef AppVersion
  #define AppVersion "0.5.1"
#endif
#ifndef StageDir
  #define StageDir "..\release\staging\StratoCam"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif
#ifndef SetupBaseName
  #define SetupBaseName "StratoCam-Setup-v" + AppVersion
#endif

[Setup]
AppId={{A413FD10-B2C6-4944-8870-658CE5D9D274}
AppName=StratoCam
AppVersion={#AppVersion}
AppVerName=StratoCam {#AppVersion}
AppPublisher=StratoCam contributors
DefaultDirName={autopf}\StratoCam
DefaultGroupName=StratoCam
DisableProgramGroupPage=yes
OutputDir={#OutputDir}
OutputBaseFilename={#SetupBaseName}
SetupIconFile=..\windows\resources\stratocam.ico
UninstallDisplayIcon={app}\StratoCam.exe
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern dynamic
PrivilegesRequired=admin
SetupArchitecture=x64
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.22000
CloseApplications=yes
RestartApplications=no
UseSetupLdr=x64
VersionInfoVersion={#AppVersion}.0
VersionInfoProductName=StratoCam
VersionInfoDescription=StratoCam x64 installer

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked
Name: "obsplugin"; Description: "Install the StratoCam source for OBS Studio 32.2.2"; GroupDescription: "Integrations:"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\OBS\stratocam-obs.dll"; DestDir: "{commonappdata}\obs-studio\plugins\stratocam-obs\bin\64bit"; Tasks: obsplugin; Flags: ignoreversion
Source: "{#StageDir}\OBS\LICENSE_NOTICE.md"; DestDir: "{commonappdata}\obs-studio\plugins\stratocam-obs"; Tasks: obsplugin; Flags: ignoreversion

[Icons]
Name: "{group}\StratoCam"; Filename: "{app}\StratoCam.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\StratoCam"; Filename: "{app}\StratoCam.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\StratoCam.exe"; Description: "Open StratoCam"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{app}\platform-tools\adb.exe"; Parameters: "kill-server"; Flags: runhidden waituntilterminated skipifdoesntexist; RunOnceId: "StratoCamAdbServer"
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\Support\virtual-camera-registration.ps1"" -Action Uninstall -InstallRoot ""{app}"""; Flags: runhidden waituntilterminated; RunOnceId: "StratoCamVirtualCamera"

[UninstallDelete]
Type: filesandordirs; Name: "{commonappdata}\obs-studio\plugins\stratocam-obs"
Type: filesandordirs; Name: "{app}\platform-tools"

[InstallDelete]
Type: filesandordirs; Name: "{commonappdata}\obs-studio\plugins\android-cam-obs"; Tasks: obsplugin

[Code]
procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
  Parameters: String;
begin
  if CurStep = ssPostInstall then
  begin
    WizardForm.StatusLabel.Caption := 'Registering StratoCam webcam…';
    Parameters := '-NoProfile -ExecutionPolicy Bypass -File "' +
      ExpandConstant('{app}\Support\virtual-camera-registration.ps1') +
      '" -Action Install -InstallRoot "' + ExpandConstant('{app}') + '"';
    if not Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
      Parameters, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
      RaiseException('Could not start StratoCam webcam registration. Windows error: ' +
        IntToStr(ResultCode));
    if ResultCode <> 0 then
      RaiseException('StratoCam webcam registration failed (code ' + IntToStr(ResultCode) +
        '). Run the installer again to repair the installation.');
  end;
end;
