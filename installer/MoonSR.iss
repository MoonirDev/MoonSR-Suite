; MoonSR installer (Inno Setup 6).
;
; Build with tools\make_installer.ps1, which passes:
;   /DMyAppVersion=<x.y.z>   version read from CMakeLists.txt (project(MoonSR VERSION x.y.z ...))
;   /DBuildBin=<dir>         folder holding the built MoonSR.exe, MoonSR_NrHost.exe, MoonSR_NrBridge.dll,
;                            NeuralRendering_nvngx.dll
;   /DOutDir=<dir>           optional, defaults to ..\dist
;
; Per-user ("Just me") install, no administrator rights, fully offline. Settings and data live in
; %LOCALAPPDATA%\MoonSR and are never touched by install or update; the uninstaller asks before removing them.

#ifndef MyAppVersion
  #error Pass /DMyAppVersion=x.y.z (tools\make_installer.ps1 does this).
#endif
#ifndef BuildBin
  #error Pass /DBuildBin=<folder with the built binaries> (tools\make_installer.ps1 does this).
#endif
#ifndef OutDir
  #define OutDir "..\dist"
#endif

; Numeric x.y.z.w form for the version resource (strips any "-suffix", pads to four fields).
#define MyNumericVersion Copy(MyAppVersion, 1, (Pos("-", MyAppVersion + "-") - 1))

#define MyAppName      "MoonSR"
#define MyAppExe       "MoonSR.exe"
#define MyPublisher    "MoonDev"
#define MyUrl          "https://github.com/MoonirDev/MoonSR-Suite"
; Must equal kMutexName in src\app\main.cpp (L"Local\\MoonSR.SingleInstance").
#define MyAppMutex     "Local\MoonSR.SingleInstance"
; AppId GUID: identifies the installation for updates and the Apps & Features entry.
; IT MUST NEVER CHANGE. Changing it makes the next installer create a second, separate installation.
#define MyAppGuid      "9D3A6C52-4B1E-4F7A-8E25-3C0B7A61D4F8"

[Setup]
AppId={{{#MyAppGuid}}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyPublisher}
AppPublisherURL={#MyUrl}
AppSupportURL={#MyUrl}
AppUpdatesURL={#MyUrl}
VersionInfoVersion={#MyNumericVersion}
VersionInfoProductVersion={#MyNumericVersion}
VersionInfoCompany={#MyPublisher}
VersionInfoDescription={#MyAppName} Setup
VersionInfoProductName={#MyAppName}

; Per-user, no UAC.
PrivilegesRequired=lowest
DefaultDirName={localappdata}\Programs\{#MyAppName}
UsePreviousAppDir=yes
DisableDirPage=auto
DisableProgramGroupPage=yes
DefaultGroupName={#MyAppName}
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0

; Running instance: the app creates this mutex; Setup and the uninstaller refuse to proceed (or ask the user to close
; MoonSR) while it exists. Nothing is ever closed silently without the user being asked.
AppMutex={#MyAppMutex}
CloseApplications=yes
RestartApplications=no

SetupIconFile=..\assets\brand\moonsr.ico
UninstallDisplayIcon={app}\{#MyAppExe}
UninstallDisplayName={#MyAppName}
WizardStyle=modern
LicenseFile=..\LICENSE

OutputDir={#OutDir}
OutputBaseFilename=MoonSR-Setup-{#MyAppVersion}

; Size first: one solid LZMA2 stream, maximum settings, single block (best ratio).
Compression=lzma2/ultra64
SolidCompression=yes
LZMAUseSeparateProcess=yes
LZMANumBlockThreads=1
LZMADictionarySize=65536
LZMANumFastBytes=273
LZMAAlgorithm=1

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Explicit list only. Never add vendor runtimes or models (nvngx_dlssnr.dll, nvngx.dll_dlssnr.dll, amd-nr\*, intel-nr\*,
; *.pak, *.safetensors), test or benchmark tools, fixtures, .pdb, .ilk or logs.
Source: "{#BuildBin}\MoonSR.exe";                    DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildBin}\MoonSR_NrHost.exe";             DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildBin}\MoonSR_NrBridge.dll";           DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildBin}\NeuralRendering_nvngx.dll";     DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE";                                DestDir: "{app}"; Flags: ignoreversion
Source: "..\THIRD_PARTY_NOTICES.md";                 DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md";                              DestDir: "{app}"; Flags: ignoreversion
; GPL-3.0 text that applies to MoonSR_NrBridge.dll (and the third-party notices that go with it).
Source: "..\addons\moonsr_nr_bridge\LICENSE";                                         DestDir: "{app}\licenses"; DestName: "GPL-3.0.txt"; Flags: ignoreversion
Source: "..\addons\moonsr_nr_bridge\neural_rendering\NOTICE.md";                      DestDir: "{app}\licenses"; DestName: "NeuralRendering-NOTICE.md"; Flags: ignoreversion
Source: "..\addons\moonsr_nr_bridge\neural_rendering\licenses\OptiScaler-GPL-3.0.txt"; DestDir: "{app}\licenses"; Flags: ignoreversion
Source: "..\addons\moonsr_nr_bridge\neural_rendering\licenses\RenoDX_ATTRIBUTION.txt"; DestDir: "{app}\licenses"; Flags: ignoreversion
Source: "..\addons\moonsr_nr_bridge\neural_rendering\licenses\video2dlssnr-MIT.txt";   DestDir: "{app}\licenses"; Flags: ignoreversion

[InstallDelete]
; On update, remove files an earlier version shipped that this one does not. Only list known old names here, never
; wildcards, and never anything under %LOCALAPPDATA%\MoonSR.
; Type: files; Name: "{app}\OldFileName.dll"

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"
Name: "{autodesktop}\{#MyAppName}";  Filename: "{app}\{#MyAppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExe}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Neural rendering runtimes the user imported through MoonSR's setup panel live beside MoonSR.exe. Updates keep them
; (Inno only replaces its own files); uninstalling MoonSR removes them with the program folder.
Type: filesandordirs; Name: "{app}\amd-nr"
Type: filesandordirs; Name: "{app}\intel-nr"
Type: dirifempty; Name: "{app}"
; %LOCALAPPDATA%\MoonSR (settings, models, profiles, recordings, logs) is handled in [Code] below, and only after the
; user is asked.

[Code]
const
  UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{{#MyAppGuid}}_is1';

var
  IsUpdate: Boolean;

// Compares the numeric x.y.z.w prefixes of two version strings ("1.0.1-test" compares as 1.0.1).
// Returns -1, 0 or 1 (A older, equal, newer).
function CompareVer(const A, B: String): Integer;
var
  SA, SB: String;
  NA, NB, I, P: Integer;
begin
  Result := 0;
  SA := A + '.';
  SB := B + '.';
  for I := 1 to 4 do
  begin
    NA := 0;
    NB := 0;
    P := Pos('.', SA);
    if P > 0 then
    begin
      NA := StrToIntDef(Copy(SA, 1, P - 1), 0);
      Delete(SA, 1, P);
    end;
    P := Pos('.', SB);
    if P > 0 then
    begin
      NB := StrToIntDef(Copy(SB, 1, P - 1), 0);
      Delete(SB, 1, P);
    end;
    if NA < NB then begin Result := -1; Exit; end;
    if NA > NB then begin Result := 1; Exit; end;
  end;
end;

function NumericPart(const V: String): String;
var
  P: Integer;
begin
  Result := V;
  P := Pos('-', Result);
  if P > 0 then Result := Copy(Result, 1, P - 1);
end;

function InitializeSetup(): Boolean;
var
  Installed: String;
begin
  Result := True;
  Installed := '';
  IsUpdate := RegQueryStringValue(HKCU, UninstallKey, 'DisplayVersion', Installed);
  if IsUpdate and (CompareVer(NumericPart(Installed), NumericPart('{#MyAppVersion}')) > 0) then
  begin
    // Silent installs answer "No" (refuse) because the message box is suppressed with the default below.
    if SuppressibleMsgBox(
         'MoonSR ' + Installed + ' is already installed, which is newer than the version in this installer ({#MyAppVersion}).' + #13#10 + #13#10 +
         'Install the older version anyway? Your settings are kept.',
         mbConfirmation, MB_YESNO or MB_DEFBUTTON2, IDNO) <> IDYES then
      Result := False;
  end;
end;

// On an update the folder and the ready page add nothing: skip them (the folder page is also skipped by DisableDirPage=auto).
function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := IsUpdate and (PageID = wpReady);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  DataDir: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    DataDir := ExpandConstant('{localappdata}\MoonSR');
    // Silent uninstalls keep the data. Interactive ones ask, default "No" (keep).
    if (not UninstallSilent) and DirExists(DataDir) then
    begin
      if MsgBox('Also delete MoonSR settings and data (profiles, downloaded models, recordings, logs)?' + #13#10 + #13#10 +
                DataDir + #13#10 + #13#10 +
                'Choose No to keep them, for example if you plan to reinstall MoonSR.',
                mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
        DelTree(DataDir, True, True, True);
    end;
  end;
end;
