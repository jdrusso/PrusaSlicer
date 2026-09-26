; Windows installer for the PrusaSlicer-Sync fork, built by .github/workflows/build.yml:
;   ISCC /DAppVersion=2.9.6-sync.3 /DSourceDir=<packaged PrusaSlicer folder> PrusaSlicer-Sync.iss
;
; Installs per user (no admin rights), so the in-app updater can run it silently:
;   PrusaSlicer-Sync-Setup-x64.exe /SILENT /SP- /SUPPRESSMSGBOXES /NOCANCEL /WAITPID=<pid>
; /WAITPID makes setup wait for that (closing) PrusaSlicer process before replacing its files.
; PrusaSlicer is started again when setup finishes.

#ifndef AppVersion
  #define AppVersion "0.0.0-dev"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\out\PrusaSlicer"
#endif

[Setup]
; Never change AppId: upgrades find the existing installation through it.
AppId={{0CFB67D7-554B-46CA-96D7-6209929749B9}
AppName=PrusaSlicer-Sync
AppVersion={#AppVersion}
AppVerName=PrusaSlicer-Sync {#AppVersion}
AppPublisher=jd
AppPublisherURL=https://github.com/jdrusso/PrusaSlicer
AppUpdatesURL=https://github.com/jdrusso/PrusaSlicer/releases
PrivilegesRequired=lowest
DefaultDirName={autopf}\PrusaSlicer-Sync
DisableProgramGroupPage=yes
DisableDirPage=auto
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=yes
RestartApplications=no
SetupIconFile=..\..\resources\icons\PrusaSlicer.ico
UninstallDisplayIcon={app}\prusa-slicer.exe
UninstallDisplayName=PrusaSlicer-Sync
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
OutputBaseFilename=PrusaSlicer-Sync-Setup-x64

[Tasks]
Name: desktopicon; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[InstallDelete]
; Upgrades: drop resources removed in the new version instead of leaving stale files behind.
Type: filesandordirs; Name: "{app}\resources"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\PrusaSlicer-Sync"; Filename: "{app}\prusa-slicer.exe"
Name: "{autoprograms}\PrusaSlicer-Sync G-code Viewer"; Filename: "{app}\prusa-gcodeviewer.exe"
Name: "{autodesktop}\PrusaSlicer-Sync"; Filename: "{app}\prusa-slicer.exe"; Tasks: desktopicon

[Run]
; Also runs for /SILENT installs (no skipifsilent), which restarts PrusaSlicer after an in-app update.
Filename: "{app}\prusa-slicer.exe"; Description: "{cm:LaunchProgram,PrusaSlicer-Sync}"; Flags: nowait postinstall

[Code]
const
  SYNCHRONIZE = $00100000;

function OpenProcess(dwDesiredAccess: Cardinal; bInheritHandle: BOOL; dwProcessId: Cardinal): THandle;
  external 'OpenProcess@kernel32.dll stdcall';
function WaitForSingleObject(hHandle: THandle; dwMilliseconds: Cardinal): Cardinal;
  external 'WaitForSingleObject@kernel32.dll stdcall';
function CloseHandle(hObject: THandle): BOOL;
  external 'CloseHandle@kernel32.dll stdcall';

function InitializeSetup(): Boolean;
var
  Pid: Cardinal;
  Process: THandle;
begin
  // Started by the in-app updater: give that PrusaSlicer up to a minute to finish closing.
  // Anything still running after that is handled by CloseApplications.
  Pid := StrToIntDef(ExpandConstant('{param:WAITPID|0}'), 0);
  if Pid <> 0 then
  begin
    Process := OpenProcess(SYNCHRONIZE, False, Pid);
    if Process <> 0 then
    begin
      WaitForSingleObject(Process, 60000);
      CloseHandle(Process);
    end;
  end;
  Result := True;
end;
