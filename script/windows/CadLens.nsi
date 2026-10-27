Unicode true

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "WinVer.nsh"
!include "x64.nsh"

!define APP_NAME "${U+901F}${U+770B}CAD"
!define AUTHOR "Mr. ${U+738B}${U+5609}"

!ifndef STAGE
  !error "STAGE is required"
!endif
!ifndef OUTFILE
  !error "OUTFILE is required"
!endif
!ifndef APPICON
  !error "APPICON is required"
!endif

Name "${APP_NAME}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\${APP_NAME}"
InstallDirRegKey HKLM "Software\CADLens" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
SetCompressorDictSize 32

VIProductVersion "1.0.4.0"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "ProductVersion" "1.0.4"
VIAddVersionKey "CompanyName" "${AUTHOR}"
VIAddVersionKey "FileDescription" "${APP_NAME} Engineering Drawing Viewer"
VIAddVersionKey "FileVersion" "1.0.4.0"
VIAddVersionKey "LegalCopyright" "Copyright ${AUTHOR}"

!define MUI_ABORTWARNING
!define MUI_ICON "${APPICON}"
!define MUI_UNICON "${APPICON}"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "${APP_NAME}" SecMain
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "${APP_NAME} requires 64-bit Windows."
    Quit
  ${EndIf}
  SetRegView 64
  SetOutPath "$INSTDIR"
  File /r "${STAGE}/*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  CreateDirectory "$SMPROGRAMS\${APP_NAME}"
  CreateShortCut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\CadLens.exe"
  CreateShortCut "$SMPROGRAMS\${APP_NAME}\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
  CreateShortCut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\CadLens.exe"

  WriteRegStr HKLM "Software\CADLens" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CADLens" \
    "DisplayName" "${APP_NAME}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CADLens" \
    "DisplayVersion" "1.0.4"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CADLens" \
    "Publisher" "${AUTHOR}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CADLens" \
    "DisplayIcon" "$INSTDIR\CadLens.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CADLens" \
    "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
SectionEnd

Section "Uninstall"
  SetRegView 64
  Delete "$DESKTOP\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\Uninstall.lnk"
  RMDir "$SMPROGRAMS\${APP_NAME}"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CADLens"
  DeleteRegKey HKLM "Software\CADLens"
  RMDir /r "$INSTDIR"
SectionEnd
