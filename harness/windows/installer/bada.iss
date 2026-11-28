; bada's installer for Windows (Inno Setup 6). Build it with tools\package.ps1, which passes:
;   Version   the repository's VERSION, e.g. 0.1.0
;   Build     harness\windows\build, holding x64 and x86
;   Sign      defined when the installer and its uninstaller are signed (SignTool "bada")
; It copies the text service for 64-bit and 32-bit apps, registers both (tools\register.ps1 does
; the same for a development build), and adds bada to the user's Japanese keyboards. It shares
; nothing with bada-windows or any other input method: its own AppId, CLSID, profile and folders.
; Adapted from jis-moonshot's installer.

#ifndef Version
  #error Build with tools\package.ps1
#endif
#define Root "..\..\.."
#define Clsid "{5B6D78A1-D25C-40FC-B4FF-4AA02120BD84}"
#define Publisher "Mirae Research"
#define DocsUrl "https://github.com/team-mirae/bada"

[Setup]
; Permanent: Windows finds an earlier install (to upgrade it) by this id. Never change it.
AppId={{E589995C-3B9F-493D-A08D-E60B4FEB7021}
AppName=bada
AppVersion={#Version}
AppVerName=bada {#Version}
AppPublisher={#Publisher}
AppPublisherURL={#DocsUrl}
AppSupportURL={#DocsUrl}
VersionInfoVersion={#Version}
AppCopyright=Copyright 2026 Mirae Research
DefaultDirName={autopf}\bada
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
DisableWelcomePage=no
LicenseFile={#Root}\LICENSE
PrivilegesRequired=admin
; Windows 11 is 64-bit only; the x86 DLL is for 32-bit apps on it.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
; Every app has the DLL loaded: never offer to close them. The old DLL is moved aside instead
; (RenameAside below), and apps load the new one when they restart.
CloseApplications=no
RestartApplications=no
SetupIconFile=..\src\tip\bada.ico
UninstallDisplayIcon={app}\bada.ico
UninstallDisplayName=bada
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
OutputDir={#Build}\dist
OutputBaseFilename=bada-{#Version}-setup
#ifdef Sign
SignTool=bada
SignedUninstaller=yes
#endif

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[Messages]
; Not the default "close all other applications": apps can stay open, and restart afterwards.
WelcomeLabel2=This will install [name/ver] on your computer.%n%nYou don't need to close other apps. Apps that are open keep the old version, or none, until they restart.
ConfirmUninstall=Remove bada from this PC and from your keyboards?
FinishedHeadingLabel=bada is installed
FinishedLabelNoIcons=Restart the apps that were open: they keep the old version, or none, until they restart.%n%nPress Win+Space and pick bada, under Japanese.%n%nA new install has no layouts chosen, so keys type as Windows has them until they are: bada settings, which opens next, asks for the two to start with.
FinishedLabel=Restart the apps that were open: they keep the old version, or none, until they restart.%n%nPress Win+Space and pick bada, under Japanese.%n%nA new install has no layouts chosen, so keys type as Windows has them until they are: bada settings, which opens next, asks for the two to start with.

[Files]
; The engine is inside the DLL.
Source: "{#Build}\x64\bada-tsf.dll"; DestDir: "{app}\x64"; Flags: ignoreversion
Source: "{#Build}\x86\bada-tsf.dll"; DestDir: "{app}\x86"; Flags: ignoreversion
; bada-tide's helper (docs/bada-tide-v1.md), started by the text service while the settings folder
; has a translator in tide\. One for both: the 32-bit text service starts the 64-bit one.
Source: "{#Build}\x64\bada-tide.exe"; DestDir: "{app}\x64"; Flags: ignoreversion
; The engine's data, for both: each file is the buffer of its name, mapped by the text service.
Source: "{#Build}\data\*"; DestDir: "{app}\data"; Flags: ignoreversion
; The command line, and the settings page it serves: "bada settings" opens it in the browser.
Source: "{#Build}\x64\bada.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Build}\settings\*"; DestDir: "{app}\settings"; Flags: ignoreversion
Source: "{#Root}\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\src\tip\bada.ico"; DestDir: "{app}"; Flags: ignoreversion
; Run by Setup and the uninstaller as the user, to change their language list.
Source: "keyboards.ps1"; DestDir: "{app}"; Flags: ignoreversion

[InstallDelete]
; The shortcut was "bada Settings" in 0.2.1's first builds. A file name's case doesn't change when
; it is written over, so the old one goes first.
Type: files; Name: "{autoprograms}\bada Settings.lnk"

[Icons]
; Minimized: the command line is a console program, and its window has nothing to show.
Name: "{autoprograms}\bada settings"; Filename: "{app}\bada.exe"; Parameters: "settings"; IconFilename: "{app}\bada.ico"; Flags: runminimized; Comment: "The settings of the bada input method"

[Run]
Filename: "{app}\bada.exe"; Parameters: "settings"; Description: "Open bada settings"; Flags: postinstall nowait skipifsilent runasoriginaluser runminimized

[Code]
const
  MOVEFILE_DELAY_UNTIL_REBOOT = 4;

var
  RemoveUserData: Boolean;
  LeftForRestart: Boolean;

{ MoveFileExW with no new name: delete the file when Windows restarts. }
function DeleteAtRestart(Existing: String; NoNewName, Flags: Cardinal): Boolean;
  external 'MoveFileExW@kernel32.dll stdcall';

function PowerShell(Params: String): String;
begin
  Result := '-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File "'
    + ExpandConstant('{app}\keyboards.ps1') + '" ' + Params;
end;

{ Runs keyboards.ps1 as the user who started Setup: the language list is per user, and the
  elevated process may be another account. The uninstaller can't do that (Inno Setup has no
  ExecAsOriginalUser there): it changes the list of the account it was elevated as, the same
  user unless they elevated with another account's password. }
function RunKeyboards(Params: String): Boolean;
var
  Exe: String;
  Code: Integer;
begin
  Exe := ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe');
  if IsUninstaller then
    Result := Exec(Exe, PowerShell(Params), '', SW_HIDE, ewWaitUntilTerminated, Code)
  else
    Result := ExecAsOriginalUser(Exe, PowerShell(Params), '', SW_HIDE, ewWaitUntilTerminated, Code);
  Result := Result and (Code = 0);
end;

{ Stops bada-tide's helper: its leases run out by themselves, and the keyboard gives the keys back
  to Windows within a few seconds. A text service starts the new one when it needs it. }
procedure StopTideHelper;
var
  Code: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM bada-tide.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

function RegSvr(Arch, Params: String): Boolean;
var
  Exe: String;
  Code: Integer;
begin
  if Arch = 'x64' then Exe := ExpandConstant('{sys}\regsvr32.exe')
  else Exe := ExpandConstant('{syswow64}\regsvr32.exe');
  Result := Exec(Exe, Params + ' "' + ExpandConstant('{app}\' + Arch + '\bada-tsf.dll') + '"',
    '', SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0);
end;

{ Deletes the copies earlier installs moved aside, where no process holds them any more. The
  others stay, or with AtRestart are deleted when Windows restarts; returns whether any did. }
function DeleteOldCopies(Dir: String; AtRestart: Boolean): Boolean;
var
  Find: TFindRec;
begin
  Result := False;
  if FindFirst(Dir + '\*.old-*', Find) then begin
    try
      repeat
        if not DeleteFile(Dir + '\' + Find.Name) and AtRestart then begin
          DeleteAtRestart(Dir + '\' + Find.Name, 0, MOVEFILE_DELAY_UNTIL_REBOOT);
          Result := True;
        end;
      until not FindNext(Find);
    finally
      FindClose(Find);
    end;
  end;
end;

{ A process with the text service loaded keeps the file open, but it can be renamed: move it
  aside so new processes load the new build. Returns the new name, or '' if the file is gone. }
function RenameAside(Target: String): String;
begin
  Result := '';
  if FileExists(Target) and not DeleteFile(Target) then begin
    Result := Target + '.old-' + GetDateTimeString('yyyymmddhhnnsszzz', #0, #0);
    if not RenameFile(Target, Result) then
      RaiseException('Can''t replace ' + Target + '.');
  end;
end;

{ The data files are mapped by every process that has the text service loaded: they are moved
  aside the same way. With AtRestart, what is left is deleted when Windows restarts; returns
  whether anything was. }
function MoveDataAside(AtRestart: Boolean): Boolean;
var
  Dir, Aside: String;
  Find: TFindRec;
  Names: TStringList;
  I: Integer;
begin
  Dir := ExpandConstant('{app}\data');
  Result := DeleteOldCopies(Dir, AtRestart);
  Names := TStringList.Create;
  try
    if FindFirst(Dir + '\*', Find) then begin
      try
        repeat
          if (Find.Attributes and FILE_ATTRIBUTE_DIRECTORY = 0) and (Pos('.old-', Find.Name) = 0) then
            Names.Add(Find.Name);
        until not FindNext(Find);
      finally
        FindClose(Find);
      end;
    end;
    for I := 0 to Names.Count - 1 do begin
      Aside := RenameAside(Dir + '\' + Names[I]);
      if (Aside <> '') and AtRestart then begin
        DeleteAtRestart(Aside, 0, MOVEFILE_DELAY_UNTIL_REBOOT);
        Result := True;
      end;
    end;
  finally
    Names.Free;
  end;
  if Result and AtRestart then DeleteAtRestart(Dir, 0, MOVEFILE_DELAY_UNTIL_REBOOT);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Arch: String;
  I: Integer;
begin
  if CurStep = ssInstall then begin
    { Another user's helper can't be stopped: its file is moved aside like a DLL in use. }
    StopTideHelper;
    RenameAside(ExpandConstant('{app}\x64\bada-tide.exe'));
    for I := 0 to 1 do begin
      if I = 0 then Arch := 'x64' else Arch := 'x86';
      DeleteOldCopies(ExpandConstant('{app}\' + Arch), False);
      RenameAside(ExpandConstant('{app}\' + Arch + '\bada-tsf.dll'));
    end;
    MoveDataAside(False);
    { The command line is running while its settings page is open: it is moved aside too, and
      that page goes on with the old one until it is closed. }
    DeleteOldCopies(ExpandConstant('{app}'), False);
    RenameAside(ExpandConstant('{app}\bada.exe'));
  end else if CurStep = ssPostInstall then begin
    if not RegSvr('x64', '/s') or not RegSvr('x86', '/s') then
      MsgBox('bada couldn''t be registered with Windows. Run Setup again; if it keeps failing, ' +
        'see ' + '{#DocsUrl}.', mbError, MB_OK);
    { As the user who started Setup: their keyboards. }
    if not RunKeyboards('') then
      MsgBox('bada is installed, but it couldn''t be added to your keyboards. Add it in ' +
        'Settings > Time & language > Language & region > Japanese > Language options.',
        mbInformation, MB_OK);
  end;
end;

{ After the uninstaller's own "remove bada?" (ConfirmUninstall): whether to delete the user's
  own files too. Unticked by default. }
procedure AskAboutUserData;
var
  Form: TSetupForm;
  Text: TNewStaticText;
  Box: TNewCheckBox;
  Ok: TNewButton;
begin
  Form := CreateCustomForm(ScaleX(400), ScaleY(150), False, True);
  try
    Form.Caption := 'Uninstall bada';
    Text := TNewStaticText.Create(Form);
    Text.Parent := Form;
    Text.Left := ScaleX(16);
    Text.Top := ScaleY(16);
    Text.Width := Form.ClientWidth - ScaleX(32);
    Text.WordWrap := True;
    Text.Caption := 'Your settings, layouts and dictionary stay on this PC, for when you ' +
      'install bada again.';
    Box := TNewCheckBox.Create(Form);
    Box.Parent := Form;
    Box.Left := ScaleX(16);
    Box.Top := Text.Top + Text.Height + ScaleY(16);
    Box.Width := Form.ClientWidth - ScaleX(32);
    Box.Height := ScaleY(34);
    Box.Caption := 'Delete them too (%APPDATA%\bada)';
    Ok := TNewButton.Create(Form);
    Ok.Parent := Form;
    Ok.Caption := 'Continue';
    Ok.ModalResult := mrOk;
    Ok.Default := True;
    Ok.Cancel := True;
    Ok.Width := ScaleX(80);
    Ok.Height := ScaleY(24);
    Ok.Top := Form.ClientHeight - Ok.Height - ScaleY(12);
    Ok.Left := Form.ClientWidth - Ok.Width - ScaleX(16);
    Form.ShowModal;
    RemoveUserData := Box.Checked;
  finally
    Form.Free;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Arch, Dir, Aside: String;
  I: Integer;
begin
  if CurUninstallStep = usUninstall then begin
    if not UninstallSilent then AskAboutUserData;
    RunKeyboards('-Remove');
    StopTideHelper;
    for I := 0 to 1 do begin
      if I = 0 then Arch := 'x64' else Arch := 'x86';
      if FileExists(ExpandConstant('{app}\' + Arch + '\bada-tsf.dll')) then RegSvr(Arch, '/s /u');
    end;
    { Nothing of bada is left in the registry, even if a DLL couldn't say so itself. }
    RegDeleteKeyIncludingSubkeys(HKLM64, 'SOFTWARE\Microsoft\CTF\TIP\{#Clsid}');
    RegDeleteKeyIncludingSubkeys(HKLM64, 'SOFTWARE\Classes\CLSID\{#Clsid}');
    RegDeleteKeyIncludingSubkeys(HKLM32, 'SOFTWARE\Classes\CLSID\{#Clsid}');
    { Processes that loaded the DLL keep it open until they exit: move it aside and delete it
      when Windows restarts. }
    for I := 0 to 1 do begin
      if I = 0 then Arch := 'x64' else Arch := 'x86';
      Dir := ExpandConstant('{app}\' + Arch);
      if DeleteOldCopies(Dir, True) then LeftForRestart := True;
      Aside := RenameAside(Dir + '\bada-tsf.dll');
      if Aside <> '' then begin
        DeleteAtRestart(Aside, 0, MOVEFILE_DELAY_UNTIL_REBOOT);
        LeftForRestart := True;
      end;
      Aside := RenameAside(Dir + '\bada-tide.exe');
      if Aside <> '' then begin
        DeleteAtRestart(Aside, 0, MOVEFILE_DELAY_UNTIL_REBOOT);
        LeftForRestart := True;
      end;
      if LeftForRestart then DeleteAtRestart(Dir, 0, MOVEFILE_DELAY_UNTIL_REBOOT);
    end;
    if MoveDataAside(True) then LeftForRestart := True;
    { The command line, while its settings page is open. }
    if DeleteOldCopies(ExpandConstant('{app}'), True) then LeftForRestart := True;
    Aside := RenameAside(ExpandConstant('{app}\bada.exe'));
    if Aside <> '' then begin
      DeleteAtRestart(Aside, 0, MOVEFILE_DELAY_UNTIL_REBOOT);
      LeftForRestart := True;
    end;
  end else if CurUninstallStep = usPostUninstall then begin
    if LeftForRestart then
      DeleteAtRestart(ExpandConstant('{app}'), 0, MOVEFILE_DELAY_UNTIL_REBOOT);
    if RemoveUserData then
      DelTree(ExpandConstant('{userappdata}\bada'), True, True, True);
    if LeftForRestart and not UninstallSilent then
      MsgBox('Some apps still have bada loaded. Its last files are deleted when Windows ' +
        'restarts.', mbInformation, MB_OK);
  end;
end;
