Unicode true

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "WinVer.nsh"
!include "x64.nsh"

!ifndef STAGE
  !error "STAGE is required"
!endif
!ifndef OUTFILE
  !error "OUTFILE is required"
!endif
!ifndef APPICON
  !error "APPICON is required"
!endif

Name "CAD Lens"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\CAD Lens"
InstallDirRegKey HKLM "Software\CADLens" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
SetCompressorDictSize 32

VIProductVersion "1.0.0.0"
VIAddVersionKey "ProductName" "CAD Lens"
VIAddVersionKey "ProductVersion" "1.0.0"
VIAddVersionKey "CompanyName" "CAD Lens"
VIAddVersionKey "FileDescription" "CAD Lens Engineering Drawing Viewer"
VIAddVersionKey "FileVersion" "1.0.0.0"
VIAddVersionKey "LegalCopyright" "Copyright CAD Lens"

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

Section "CAD Lens" SecMain
  ${IfNot} ${AtLeastWin10}
    MessageBox MB_ICONSTOP "CAD Lens requires 64-bit Windows 10 or later."
    Quit
  ${EndIf}
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "CAD Lens requires 64-bit Windows 10 or later."
    Quit
  ${EndIf}
  SetRegView 64
  SetOutPath "$INSTDIR"
  File /r "${STAGE}/*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  CreateDirectory "$SMPROGRAMS\CAD Lens"
  CreateShortCut "$SMPROGRAMS\CAD Lens\CAD Lens.lnk" "$INSTDIR\CadLens.exe"
  CreateShortCut "$SMPROGRAMS\CAD Lens\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
  CreateShortCut "$DESKTOP\CAD Lens.lnk" "$INSTDIR\CadLens.exe"

  WriteRegStr HKLM "Software\CADLens" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CADLens" \
    "DisplayName" "CAD Lens"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CADLens" \
    "DisplayVersion" "1.0.0"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CADLens" \
    "Publisher" "CAD Lens"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CADLens" \
    "DisplayIcon" "$INSTDIR\CadLens.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CADLens" \
    "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
SectionEnd

Section "Uninstall"
  SetRegView 64
  Delete "$DESKTOP\CAD Lens.lnk"
  Delete "$SMPROGRAMS\CAD Lens\CAD Lens.lnk"
  Delete "$SMPROGRAMS\CAD Lens\Uninstall.lnk"
  RMDir "$SMPROGRAMS\CAD Lens"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CADLens"
  DeleteRegKey HKLM "Software\CADLens"
  RMDir /r "$INSTDIR"
SectionEnd
