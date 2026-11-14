#ifndef AppVersion
  #define AppVersion "0.5.3"
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
DisableWelcomePage=no
DisableDirPage=no
DisableReadyPage=yes
DisableFinishedPage=no
ShowLanguageDialog=no
UsePreviousLanguage=no
AlwaysShowDirOnReadyPage=yes
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

[Messages]
WelcomeLabel1=Welcome to StratoCam
WelcomeLabel2=Use your Android phone as a USB webcam for Windows, with live preview, camera controls, and local recording.
ClickNext=
SelectDirBrowseLabel=
WizardSelectTasks=Shortcuts
SelectTasksDesc=Desktop and Start Menu
SelectTasksLabel2=
FinishedHeadingLabel=StratoCam installation complete
FinishedLabel=
FinishedLabelNoIcons=
ClickFinish=

[CustomMessages]
english.Shortcuts=Shortcuts:
english.DesktopShortcut=Create a desktop shortcut
english.StartMenuShortcut=Create a Start Menu shortcut
english.OpenApp=Launch StratoCam
english.Destination=Installation folder:
english.RegisteringCamera=Registering StratoCam webcam…
english.InstallProgress=Installing files

[Tasks]
Name: "desktopicon"; Description: "{cm:DesktopShortcut}"; GroupDescription: "{cm:Shortcuts}"; Flags: unchecked
Name: "startmenuicon"; Description: "{cm:StartMenuShortcut}"; GroupDescription: "{cm:Shortcuts}"

[Files]
Source: "prepare-camera-update.ps1"; Flags: dontcopy
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\OBS\stratocam-obs.dll"; DestDir: "{commonappdata}\obs-studio\plugins\stratocam-obs\bin\64bit"; Flags: ignoreversion
Source: "{#StageDir}\OBS\LICENSE_NOTICE.md"; DestDir: "{commonappdata}\obs-studio\plugins\stratocam-obs"; Flags: ignoreversion

[Icons]
Name: "{group}\StratoCam"; Filename: "{app}\StratoCam.exe"; WorkingDir: "{app}"; Tasks: startmenuicon
Name: "{autodesktop}\StratoCam"; Filename: "{app}\StratoCam.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\StratoCam.exe"; Description: "{cm:OpenApp}"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{app}\platform-tools\adb.exe"; Parameters: "kill-server"; Flags: runhidden waituntilterminated skipifdoesntexist; RunOnceId: "StratoCamAdbServer"
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\Support\virtual-camera-registration.ps1"" -Action Uninstall -InstallRoot ""{app}"""; Flags: runhidden waituntilterminated; RunOnceId: "StratoCamVirtualCamera"

[UninstallDelete]
Type: filesandordirs; Name: "{commonappdata}\obs-studio\plugins\stratocam-obs"
Type: filesandordirs; Name: "{app}\platform-tools"

[InstallDelete]
Type: filesandordirs; Name: "{commonappdata}\obs-studio\plugins\android-cam-obs"

[Code]
var
  InstallationPathLabel: TNewStaticText;
  InstallationProgressLabel: TNewStaticText;
  CameraUpdatePrepared: Boolean;

function RunCameraUpdateAction(Action: String): Boolean;
var
  ResultCode: Integer;
  Parameters: String;
begin
  Parameters := '-NoProfile -ExecutionPolicy Bypass -File "' +
    ExpandConstant('{tmp}\prepare-camera-update.ps1') + '" -Action ' + Action +
    ' -StateFile "' + ExpandConstant('{tmp}\stratocam-frameserver.flag') + '"';
  Result := Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
    Parameters, '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Result := Result and (ResultCode = 0);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  ExtractTemporaryFile('prepare-camera-update.ps1');
  CameraUpdatePrepared := True;
  if not RunCameraUpdateAction('Stop') then
    Result := 'Could not release the webcam for update. Close camera applications and try again.'
  else
    Result := '';
end;

procedure DeinitializeSetup;
begin
  if CameraUpdatePrepared then
    if not RunCameraUpdateAction('Restore') then
      Log('Windows Camera Frame Server could not be restored automatically.');
end;

procedure InitializeWizard;
begin
  InstallationPathLabel := TNewStaticText.Create(WizardForm);
  InstallationPathLabel.Parent := WizardForm.InstallingPage;
  InstallationPathLabel.AutoSize := False;
  InstallationPathLabel.WordWrap := True;
  InstallationPathLabel.SetBounds(WizardForm.ProgressGauge.Left,
    WizardForm.ProgressGauge.Top + WizardForm.ProgressGauge.Height + ScaleY(14),
    WizardForm.ProgressGauge.Width, ScaleY(44));
  InstallationProgressLabel := TNewStaticText.Create(WizardForm);
  InstallationProgressLabel.Parent := WizardForm.InstallingPage;
  InstallationProgressLabel.AutoSize := False;
  InstallationProgressLabel.SetBounds(WizardForm.ProgressGauge.Left,
    InstallationPathLabel.Top + InstallationPathLabel.Height + ScaleY(8),
    WizardForm.ProgressGauge.Width, ScaleY(20));
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpSelectTasks then
    WizardForm.NextButton.Caption := SetupMessage(msgButtonInstall)
  else if CurPageID <> wpFinished then
    WizardForm.NextButton.Caption := SetupMessage(msgButtonNext);
  if CurPageID = wpInstalling then
    InstallationPathLabel.Caption := CustomMessage('Destination') + #13#10 +
      ExpandConstant('{app}');
end;

procedure CurInstallProgressChanged(CurProgress, MaxProgress: Integer);
begin
  if MaxProgress > 0 then
    InstallationProgressLabel.Caption := CustomMessage('InstallProgress') + ': ' +
      IntToStr(Round(100.0 * CurProgress / MaxProgress)) + '%';
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
  Parameters: String;
begin
  if CurStep = ssPostInstall then
  begin
    WizardForm.StatusLabel.Caption := CustomMessage('RegisteringCamera');
    InstallationProgressLabel.Caption := CustomMessage('RegisteringCamera');
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
    if not RunCameraUpdateAction('Restore') then
      RaiseException('Could not restart Windows Camera Frame Server after updating StratoCam.');
  end;
end;
