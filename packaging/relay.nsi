; Relay installer (NSIS). Built by package-windows.sh, which passes:
;   VERSION, SRC_DIR (the packaged Relay folder), UNINSTALL_LIST, ICON, OUT_FILE
;
; Installs for the current user (no administrator prompt) into
; %LOCALAPPDATA%\Programs\Relay. Running a newer installer updates in place:
; it closes Relay, removes the old program files with the old uninstaller and
; installs the new ones. Your playlists, settings, guide and recordings are
; stored elsewhere and are never touched by an update.

Unicode true
SetCompressor /SOLID lzma
RequestExecutionLevel user

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"

!define APP_NAME "Relay"
!define APP_EXE "relay.exe"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Relay"

Name "${APP_NAME} ${VERSION}"
OutFile "${OUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\Relay"
InstallDirRegKey HKCU "${UNINST_KEY}" "InstallLocation"
BrandingText "${APP_NAME} ${VERSION}"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "${APP_NAME} installer"
VIAddVersionKey "CompanyName" "${APP_NAME}"
VIAddVersionKey "LegalCopyright" ""

!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Start Relay"

!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; Ask a running Relay to close (it saves as it goes), then make sure.
!macro CloseRelay
    nsExec::Exec 'taskkill /IM ${APP_EXE}'
    Pop $0
    ${If} $0 == 0
        Sleep 2000
        nsExec::Exec 'taskkill /F /IM ${APP_EXE}'
        Pop $0
        Sleep 500
    ${EndIf}
!macroend

Section "Relay" SecMain
    SectionIn RO
    !insertmacro CloseRelay

    ; An earlier version: remove its program files first (quietly, keeping
    ; your data), so nothing stale is left behind.
    ${If} ${FileExists} "$INSTDIR\uninstall.exe"
        CopyFiles /SILENT "$INSTDIR\uninstall.exe" "$TEMP\relay-old-uninstall.exe"
        ExecWait '"$TEMP\relay-old-uninstall.exe" /S /UPDATE _?=$INSTDIR'
        Delete "$TEMP\relay-old-uninstall.exe"
    ${EndIf}

    SetOutPath "$INSTDIR"
    File /r "${SRC_DIR}\*"
    WriteUninstaller "$INSTDIR\uninstall.exe"

    CreateShortcut "$SMPROGRAMS\Relay.lnk" "$INSTDIR\${APP_EXE}"

    ; Apps & features entry
    WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "${APP_NAME}"
    WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "${APP_NAME}"
    WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\${APP_EXE}"
    WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
    WriteRegStr HKCU "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
    WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
    WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
    SectionGetSize ${SecMain} $0
    WriteRegDWORD HKCU "${UNINST_KEY}" "EstimatedSize" $0
SectionEnd

Section "Desktop shortcut" SecDesktop
    CreateShortcut "$DESKTOP\Relay.lnk" "$INSTDIR\${APP_EXE}"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
    !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} "The Relay player."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "A shortcut on your desktop."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ---- uninstall ------------------------------------------------------------------
Var RemoveData
Var Updating

Function un.onInit
    StrCpy $RemoveData 0
    StrCpy $Updating 0
    ${GetParameters} $0
    ClearErrors
    ${GetOptions} $0 "/UPDATE" $1
    ${IfNot} ${Errors}
        StrCpy $Updating 1
    ${EndIf}
    ; An update, or a quiet uninstall: never ask, never remove data.
    ${If} $Updating == 1
    ${OrIf} ${Silent}
        Return
    ${EndIf}
    MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 \
        "Also remove your Relay playlists, logins, settings, favorites and downloaded guide?$\n$\nRecordings in your Videos folder are kept either way." \
        IDNO +2
    StrCpy $RemoveData 1
FunctionEnd

Section "Uninstall"
    !insertmacro CloseRelay

    !include "${UNINSTALL_LIST}"
    Delete "$INSTDIR\uninstall.exe"
    RMDir "$INSTDIR"

    ; Shortcuts and the Apps & features entry stay during an update (the new
    ; installer writes them again).
    ${If} $Updating == 0
        Delete "$SMPROGRAMS\Relay.lnk"
        Delete "$DESKTOP\Relay.lnk"
        DeleteRegKey HKCU "${UNINST_KEY}"
    ${EndIf}

    ${If} $RemoveData == 1
        DeleteRegKey HKCU "Software\Relay"
        RMDir /r "$APPDATA\Relay"
        RMDir /r "$LOCALAPPDATA\Relay"
        ; Saved logins (Credential Manager entries named Relay/...).
        nsExec::Exec `powershell -NoProfile -ExecutionPolicy Bypass -Command "cmdkey /list | ForEach-Object { if ($$_ -match 'target=(Relay/\S+)') { cmdkey /delete:$$($$Matches[1]) | Out-Null } }"`
        Pop $0
    ${EndIf}
SectionEnd
