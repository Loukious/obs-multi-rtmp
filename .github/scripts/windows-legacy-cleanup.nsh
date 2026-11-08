!include "LogicLib.nsh"

Var LegacyCleanupFailed

; Each function takes an exact root path on the stack. Only this plugin's
; named files are deleted. Empty directories are removed without recursion.
Function RemoveLegacyProgramData
    Exch $0
    StrCmp $0 "" done_programdata
    ClearErrors
    Delete "$0\obs-multi-rtmp\bin\64bit\obs-multi-rtmp.dll"
    Delete "$0\obs-multi-rtmp\bin\64bit\obs-multi-rtmp.pdb"
    IfErrors failed_programdata
    RMDir "$0\obs-multi-rtmp\bin\64bit"
    RMDir "$0\obs-multi-rtmp\bin"
    Goto done_programdata
    failed_programdata:
        StrCpy $LegacyCleanupFailed 1
    done_programdata:
        Pop $0
FunctionEnd

Function RemoveLegacyObsInstallation
    Exch $0
    StrCmp $0 "" done_obs
    ; Registry/default paths must point to an actual OBS installation.
    IfFileExists "$0\bin\64bit\obs64.exe" 0 done_obs
    IfFileExists "$0\obs-plugins\64bit\obs-multi-rtmp.dll" 0 done_obs
    ClearErrors
    Delete "$0\obs-plugins\64bit\obs-multi-rtmp.dll"
    Delete "$0\obs-plugins\64bit\obs-multi-rtmp.pdb"
    IfErrors failed_obs
    RMDir /r "$0\data\obs-plugins\obs-multi-rtmp"
    IfErrors failed_obs done_obs
    failed_obs:
        StrCpy $LegacyCleanupFailed 1
    done_obs:
        Pop $0
FunctionEnd

Function RemoveDetectedLegacyCopies
    Push $0
    Push $INSTDIR
    Call RemoveLegacyProgramData
    SetRegView 64
    ReadRegStr $0 HKLM "SOFTWARE\OBS Studio" ""
    Push $0
    Call RemoveLegacyObsInstallation
    ReadRegStr $0 HKCU "SOFTWARE\OBS Studio" ""
    Push $0
    Call RemoveLegacyObsInstallation
    SetRegView 32
    ReadRegStr $0 HKLM "SOFTWARE\OBS Studio" ""
    Push $0
    Call RemoveLegacyObsInstallation
    ReadRegStr $0 HKCU "SOFTWARE\OBS Studio" ""
    Push $0
    Call RemoveLegacyObsInstallation
    SetRegView 64
    Push "$PROGRAMFILES64\obs-studio"
    Call RemoveLegacyObsInstallation
    Push "$PROGRAMFILES32\obs-studio"
    Call RemoveLegacyObsInstallation
    Pop $0
FunctionEnd
