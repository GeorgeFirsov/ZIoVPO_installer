#define AppName "ZIoVPO"
#define AppVersion "1.0.0"
#define ServiceName "ZIoVPO_service"
#define ServiceDisplayName "ZIoVPO Service"

[Setup]
AppId={{A6D8B02E-0E86-4C65-A79A-8A7C5B05B8E2}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=ZIoVPO
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
OutputDir=output
OutputBaseFilename=ZIoVPO_Setup
Compression=lzma2
SolidCompression=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
CloseApplications=yes
RestartApplications=no
UninstallDisplayName={#AppName}

[Languages]
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Dirs]
Name: "{commonappdata}\ZIoVPO"

[Files]
Source: "payload\Application\*"; DestDir: "{app}\Application"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "payload\Service\*"; DestDir: "{app}\Service"; Flags: ignoreversion recursesubdirs createallsubdirs
#ifexist "payload\redist\VC_redist.x64.exe"
Source: "payload\redist\VC_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall
#endif

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\Application\Application.exe"; WorkingDir: "{app}\Application"

[Run]
#ifexist "payload\redist\VC_redist.x64.exe"
Filename: "{tmp}\VC_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing Microsoft Visual C++ Runtime..."; Flags: runhidden waituntilterminated
#endif
Filename: "{sys}\sc.exe"; Parameters: "create {#ServiceName} binPath= ""{app}\Service\ZIoVPO_service.exe"" start= demand DisplayName= ""{#ServiceDisplayName}"""; StatusMsg: "Registering Windows service..."; Flags: runhidden waituntilterminated
Filename: "{sys}\sc.exe"; Parameters: "description {#ServiceName} ""ZIoVPO protection service"""; Flags: runhidden waituntilterminated

[UninstallDelete]
Type: filesandordirs; Name: "{commonappdata}\ZIoVPO"

[Code]
function RunSc(Params: String): Integer;
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\sc.exe'), Params, '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Result := ResultCode;
end;

function ServiceExists(ServiceName: String): Boolean;
begin
  Result := RunSc('query "' + ServiceName + '"') = 0;
end;

procedure StopAndDeleteService();
var
  ResultCode: Integer;
  ApplicationExe: String;
begin
  if ServiceExists('{#ServiceName}') then
  begin
    ApplicationExe := ExpandConstant('{app}\Application\Application.exe');

    { Normal stop path: the application already contains the protected RPC stop command. }
    if FileExists(ApplicationExe) then
    begin
      Exec(ApplicationExe, '--secure-stop', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
      Sleep(3000);
    end;

    { Fallback for broken/partial installations. }
    RunSc('stop {#ServiceName}');
    Sleep(3000);
    RunSc('delete {#ServiceName}');
    Sleep(1500);
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopAndDeleteService();
  Result := '';
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    StopAndDeleteService();
  end;
end;
