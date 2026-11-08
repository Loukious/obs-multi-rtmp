!include "..\.github\scripts\windows-legacy-cleanup.nsh"
OutFile "${TEST_OUTPUT}"
RequestExecutionLevel user
SilentInstall silent
Section
    Push "${TEST_ROOT}\ProgramData plugins"
    Call RemoveLegacyProgramData
    Push "${TEST_ROOT}\Custom OBS"
    Call RemoveLegacyObsInstallation
    Push "${TEST_ROOT}\Invalid OBS"
    Call RemoveLegacyObsInstallation
    ; A repeated migration must be harmless.
    Push "${TEST_ROOT}\ProgramData plugins"
    Call RemoveLegacyProgramData
    Push "${TEST_ROOT}\Custom OBS"
    Call RemoveLegacyObsInstallation
    WriteINIStr "${TEST_ROOT}\result.ini" "Migration" "Failed" "$LegacyCleanupFailed"
    Push "${TEST_ROOT}\Locked OBS"
    Call RemoveLegacyObsInstallation
    WriteINIStr "${TEST_ROOT}\result.ini" "Locked" "Failed" "$LegacyCleanupFailed"
SectionEnd
