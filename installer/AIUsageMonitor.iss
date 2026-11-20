#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#define Root ".."

[Setup]
AppId={{79F09206-FDE9-4AFE-A8C5-783D8B75FC45}
AppName=AI Usage Monitor
AppVersion={#AppVersion}
DefaultDirName={localappdata}\Programs\AI Usage Monitor
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
DisableProgramGroupPage=yes
DisableDirPage=auto
OutputDir={#Root}\dist
OutputBaseFilename=AIUsageMonitor-Setup-{#AppVersion}
SetupIconFile={#Root}\res\app.ico
UninstallDisplayIcon={app}\AIUsageMonitor.exe
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=no

[Tasks]
Name: "autostart"; Description: "Start AI Usage Monitor when I sign in to Windows"; Flags: unchecked
Name: "claudestatus"; Description: "Enable Claude Code live updates (updates the statusLine setting in your Claude Code settings.json)"; Flags: unchecked

[Files]
Source: "{#Root}\AIUsageMonitor.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "{#Root}\CREDITS.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\AI Usage Monitor"; Filename: "{app}\AIUsageMonitor.exe"

[Run]
Filename: "{app}\AIUsageMonitor.exe"; Description: "Start AI Usage Monitor"; Flags: nowait postinstall skipifsilent

[Code]
const
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';
  RunValue = 'AIUsageMonitor';

function RunApp(const Args: String): Integer;
begin
  if not Exec(ExpandConstant('{app}\AIUsageMonitor.exe'), Args, '', SW_HIDE, ewWaitUntilTerminated, Result) then
    Result := -1;
end;

function OwnStartupValue(): String;
begin
  Result := '"' + ExpandConstant('{app}\AIUsageMonitor.exe') + '"';
end;

procedure RemoveOwnStartup();
var Value: String;
begin
  if RegQueryStringValue(HKCU, RunKey, RunValue, Value) and (CompareText(Value, OwnStartupValue()) = 0) then
    RegDeleteValue(HKCU, RunKey, RunValue);
end;

procedure CurStepChanged(Step: TSetupStep);
var ResultCode: Integer;
var Details: String;
begin
  if (Step = ssInstall) and FileExists(ExpandConstant('{app}\AIUsageMonitor.exe')) then
    RunApp('--quit');
  if Step <> ssPostInstall then exit;
  if WizardIsTaskSelected('autostart') then
    RegWriteStringValue(HKCU, RunKey, RunValue, OwnStartupValue())
  else
    RemoveOwnStartup();
  if not WizardIsTaskSelected('claudestatus') then exit;
  ResultCode := RunApp('--enable-claude');
  if ResultCode = 0 then begin
    Log('Claude Code status line configured.');
    exit;
  end;
  if ResultCode = 2 then
    Details := 'Claude Code already has a custom status line. Setup kept it. See the project README to connect the monitor to that command.'
  else
    Details := 'Setup could not configure the Claude Code status line. Use Copy Claude Code live update settings in the monitor menu.';
  Log(Details + ' Exit code: ' + IntToStr(ResultCode));
  if not WizardSilent() then MsgBox(Details, mbInformation, MB_OK);
end;

procedure CurUninstallStepChanged(Step: TUninstallStep);
begin
  if Step = usUninstall then begin
    RunApp('--quit');
    Log('Claude Code status line cleanup exit code: ' + IntToStr(RunApp('--disable-claude')));
    RemoveOwnStartup();
  end;
end;
