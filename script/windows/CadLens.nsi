Unicode true

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "WinVer.nsh"
!include "x64.nsh"

!define APP_NAME "${U+901F}${U+770B}CAD"
!define AUTHOR "Mr. ${U+738B}${U+5609}"
!define APP_REGISTERED_NAME "${U+901F}${U+770B}CAD"

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

  ; Register all three formats in the per-user "Open With" list. DWG and DXF
  ; become defaults only when explicitly enabled in the application settings.
  WriteRegStr HKCU "Software\Classes\Sukancad.DWG" "" "DWG 图纸"
  WriteRegStr HKCU "Software\Classes\Sukancad.DWG\Application" \
    "ApplicationName" "${APP_NAME}"
  WriteRegStr HKCU "Software\Classes\Sukancad.DWG\DefaultIcon" \
    "" "$\"$INSTDIR\CadLens.exe$\",2"
  WriteRegStr HKCU "Software\Classes\Sukancad.DWG\shell\open\command" \
    "" "$\"$INSTDIR\CadLens.exe$\" $\"%1$\""

  WriteRegStr HKCU "Software\Classes\Sukancad.DXF" "" "DXF 图纸"
  WriteRegStr HKCU "Software\Classes\Sukancad.DXF\Application" \
    "ApplicationName" "${APP_NAME}"
  WriteRegStr HKCU "Software\Classes\Sukancad.DXF\DefaultIcon" \
    "" "$\"$INSTDIR\CadLens.exe$\",3"
  WriteRegStr HKCU "Software\Classes\Sukancad.DXF\shell\open\command" \
    "" "$\"$INSTDIR\CadLens.exe$\" $\"%1$\""

  WriteRegStr HKCU "Software\Classes\Sukancad.PDF" "" "PDF 文档"
  WriteRegStr HKCU "Software\Classes\Sukancad.PDF\Application" \
    "ApplicationName" "${APP_NAME}"
  WriteRegStr HKCU "Software\Classes\Sukancad.PDF\DefaultIcon" \
    "" "$\"$INSTDIR\CadLens.exe$\",0"
  WriteRegStr HKCU "Software\Classes\Sukancad.PDF\shell\open\command" \
    "" "$\"$INSTDIR\CadLens.exe$\" $\"%1$\""

  WriteRegStr HKCU "Software\Classes\.dwg\OpenWithProgids" \
    "Sukancad.DWG" ""
  WriteRegStr HKCU "Software\Classes\.dxf\OpenWithProgids" \
    "Sukancad.DXF" ""
  WriteRegStr HKCU "Software\Classes\.pdf\OpenWithProgids" \
    "Sukancad.PDF" ""
  WriteRegStr HKCU "Software\Classes\.dwg\OpenWithList\CadLens.exe" "" ""
  WriteRegStr HKCU "Software\Classes\.dxf\OpenWithList\CadLens.exe" "" ""
  WriteRegStr HKCU "Software\Classes\.pdf\OpenWithList\CadLens.exe" "" ""

  WriteRegStr HKCU "Software\Classes\Applications\CadLens.exe" \
    "ApplicationName" "${APP_NAME}"
  WriteRegStr HKCU "Software\Classes\Applications\CadLens.exe\shell\open\command" \
    "" "$\"$INSTDIR\CadLens.exe$\" $\"%1$\""
  WriteRegStr HKCU "Software\Classes\Applications\CadLens.exe\SupportedTypes" \
    ".dwg" ""
  WriteRegStr HKCU "Software\Classes\Applications\CadLens.exe\SupportedTypes" \
    ".dxf" ""
  WriteRegStr HKCU "Software\Classes\Applications\CadLens.exe\SupportedTypes" \
    ".pdf" ""

  WriteRegStr HKCU "Software\CADLens\Capabilities" \
    "ApplicationName" "${APP_NAME}"
  WriteRegStr HKCU "Software\CADLens\Capabilities" \
    "ApplicationDescription" "DWG、DXF、PDF 工程施工看图与测量"
  WriteRegStr HKCU "Software\CADLens\Capabilities" \
    "ApplicationIcon" "$\"$INSTDIR\CadLens.exe$\",0"
  WriteRegStr HKCU "Software\CADLens\Capabilities\FileAssociations" \
    ".dwg" "Sukancad.DWG"
  WriteRegStr HKCU "Software\CADLens\Capabilities\FileAssociations" \
    ".dxf" "Sukancad.DXF"
  WriteRegStr HKCU "Software\CADLens\Capabilities\FileAssociations" \
    ".pdf" "Sukancad.PDF"
  WriteRegStr HKCU "Software\RegisteredApplications" \
    "${APP_REGISTERED_NAME}" "Software\CADLens\Capabilities"

  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0x0000, p 0, p 0)'
SectionEnd

Section "Uninstall"
  SetRegView 64
  ReadRegStr $0 HKCU "Software\Classes\.dwg" ""
  ${If} $0 == "Sukancad.DWG"
    DeleteRegValue HKCU "Software\Classes\.dwg" ""
  ${EndIf}
  ReadRegStr $0 HKCU "Software\Classes\.dxf" ""
  ${If} $0 == "Sukancad.DXF"
    DeleteRegValue HKCU "Software\Classes\.dxf" ""
  ${EndIf}
  DeleteRegValue HKCU "Software\Classes\.dwg\OpenWithProgids" "Sukancad.DWG"
  DeleteRegValue HKCU "Software\Classes\.dxf\OpenWithProgids" "Sukancad.DXF"
  DeleteRegValue HKCU "Software\Classes\.pdf\OpenWithProgids" "Sukancad.PDF"
  DeleteRegValue HKCU "Software\Classes\.dwg\OpenWithList" "CadLens.exe"
  DeleteRegValue HKCU "Software\Classes\.dxf\OpenWithList" "CadLens.exe"
  DeleteRegValue HKCU "Software\Classes\.pdf\OpenWithList" "CadLens.exe"
  DeleteRegKey HKCU "Software\Classes\Sukancad.DWG"
  DeleteRegKey HKCU "Software\Classes\Sukancad.DXF"
  DeleteRegKey HKCU "Software\Classes\Sukancad.PDF"
  DeleteRegKey HKCU "Software\Classes\Applications\CadLens.exe"
  DeleteRegValue HKCU "Software\RegisteredApplications" "${APP_REGISTERED_NAME}"
  DeleteRegKey HKCU "Software\CADLens\Capabilities"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0x0000, p 0, p 0)'
  Delete "$DESKTOP\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\Uninstall.lnk"
  RMDir "$SMPROGRAMS\${APP_NAME}"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CADLens"
  DeleteRegKey HKLM "Software\CADLens"
  RMDir /r "$INSTDIR"
SectionEnd
