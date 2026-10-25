@echo off
rem MIDIBottleneck Player v2, Android build (Milestone 8, U11). Works from any checkout path.
rem
rem   v2\android\build.cmd android6        the "Android 6+" build: player and self-test APKs (debug-signed)
rem   v2\android\build.cmd modern          the main build (Android 10+): player and self-test APKs
rem   v2\android\build.cmd all             all four APKs
rem   v2\android\build.cmd clean           deletes the Gradle build output
rem   v2\android\build.cmd <any Gradle arguments, e.g. assembleModernDebug --warning-mode all>
rem
rem The build kit is D:\Android (JDK 17, Android SDK + NDK, Gradle cache). The variables below are set for this process
rem only; nothing is written to the user or system environment. Override ANDROID_KIT before calling for another location.
setlocal
if "%ANDROID_KIT%"=="" set "ANDROID_KIT=D:\Android"
set "JAVA_HOME=%ANDROID_KIT%\jdk-17"
set "ANDROID_HOME=%ANDROID_KIT%\Sdk"
set "ANDROID_SDK_ROOT=%ANDROID_KIT%\Sdk"
set "GRADLE_USER_HOME=%ANDROID_KIT%\gradle-home"
set "ANDROID_USER_HOME=%ANDROID_KIT%\user-home"
set "ANDROID_AVD_HOME=%ANDROID_KIT%\avd"
cd /d "%~dp0"

set "TASKS=%*"
if /i "%~1"=="android6" set "TASKS=assembleAndroid6Debug"
if /i "%~1"=="modern" set "TASKS=assembleModernDebug"
if /i "%~1"=="all" set "TASKS=assembleModernDebug assembleAndroid6Debug"
if "%TASKS%"=="" set "TASKS=assembleModernDebug assembleAndroid6Debug"

call "%~dp0gradlew.bat" %TASKS%
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" exit /b %RC%
echo.
echo APKs:
for /r "%~dp0" %%f in (*-debug.apk) do echo   %%f
exit /b 0
