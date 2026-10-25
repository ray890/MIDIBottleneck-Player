@echo off
setlocal
set "RESULT=%~dp0direct-check-result-%COMPUTERNAME%.txt"
echo.
echo MIDIBottleneck - Windows 11 MIDI driver check
echo.
echo This check is silent: it plays no sound, changes no setting and installs nothing.
echo It takes about a minute. Please close other music and MIDI programs first,
echo then wait for the "finished" message.
echo The result will be saved as:
echo   direct-check-result-%COMPUTERNAME%.txt
echo in this same folder.
echo.
"%~dp0midibn-direct-check.exe" --out "%RESULT%" --seconds 1 --cycles 5
set "RC=%errorlevel%"
echo.
echo The check has finished (exit code %RC%).
echo Please send this one file back:
echo   %RESULT%
echo.
if not defined CHECK_NOPAUSE pause
exit /b %RC%
