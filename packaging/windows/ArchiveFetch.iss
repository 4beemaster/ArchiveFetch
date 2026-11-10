#ifndef PackageDir
  #error PackageDir must point to a validated Windows portable package.
#endif
#ifndef ReleaseVersion
  #error ReleaseVersion must match the packaged app and payload.
#endif
#ifndef VersionNumber
  #error VersionNumber must be a four-part numeric version.
#endif
#ifndef OutputDir
  #error OutputDir is required.
#endif
#ifndef ProductId
  #define ProductId "ArchiveFetch-4beemaster"
#endif
#ifndef ProductName
  #define ProductName "ArchiveFetch"
#endif
#ifndef DataFolder
  #define DataFolder "ArchiveCatalog"
#endif

[Setup]
AppId={#ProductId}
AppName={#ProductName}
AppVersion={#ReleaseVersion}
AppVerName={#ProductName} {#ReleaseVersion}
AppPublisher=4beemaster
AppPublisherURL=https://github.com/4beemaster/ArchiveFetch
AppSupportURL=https://github.com/4beemaster/ArchiveFetch/issues
VersionInfoVersion={#VersionNumber}
DefaultDirName={localappdata}\Programs\{#ProductName}
DefaultGroupName={#ProductName}
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename=ArchiveFetch-{#ReleaseVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
LicenseFile={#PackageDir}\LICENSE
UninstallDisplayIcon={app}\archive_catalog_web.exe
CloseApplications=yes
RestartApplications=no
SetupLogging=yes
DisableProgramGroupPage=yes
ChangesAssociations=no

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"

[Dirs]
Name: "{localappdata}\{#DataFolder}"; Flags: uninsneveruninstall

[Files]
Source: "{#PackageDir}\*"; DestDir: "{app}"; Excludes: "START-HERE.txt"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{userprograms}\{#ProductName}"; Filename: "{app}\archive_catalog_web.exe"; WorkingDir: "{localappdata}\{#DataFolder}"
Name: "{userdesktop}\{#ProductName}"; Filename: "{app}\archive_catalog_web.exe"; WorkingDir: "{localappdata}\{#DataFolder}"; Tasks: desktopicon

[Run]
Filename: "{app}\archive_catalog_web.exe"; WorkingDir: "{localappdata}\{#DataFolder}"; Description: "Open {#ProductName}"; Flags: nowait postinstall skipifsilent

[Code]
function RuntimeAt(Root: Integer): Boolean;
var
  Version: String;
begin
  Result := RegQueryStringValue(Root,
    'Software\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}',
    'pv', Version) and (Version <> '') and (Version <> '0.0.0.0');
end;

function InitializeSetup(): Boolean;
var
  ErrorCode: Integer;
begin
  Result := RuntimeAt(HKCU32) or RuntimeAt(HKLM32) or RuntimeAt(HKCU64) or RuntimeAt(HKLM64);
  if not Result then begin
    if WizardSilent then
      Log('Microsoft Edge WebView2 Runtime is missing. Install it before running setup.')
    else if MsgBox('ArchiveFetch needs Microsoft Edge WebView2 Runtime. Setup will not install it automatically.' + #13#10 +
      'Open the official download page, install the Evergreen Runtime, then run setup again?',
      mbConfirmation, MB_YESNO) = IDYES then
      ShellExec('open', 'https://developer.microsoft.com/microsoft-edge/webview2/#download-section',
        '', '', SW_SHOWNORMAL, ewNoWait, ErrorCode);
  end;
end;
