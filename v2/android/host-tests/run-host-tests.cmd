@echo off
rem Compiles the pure-Java parts of the routes and runs the plain-Java test (no JUnit, no Android classes).
rem JDK from the Android kit (per process only).
setlocal
if "%ANDROID_KIT%"=="" set ANDROID_KIT=D:\Android
set JAVA=%ANDROID_KIT%\jdk-17\bin
set HERE=%~dp0
set ROUTE=%HERE%..\core\src\main\java\org\midibottleneck\core
set OUT=%HERE%..\..\out\android-host-tests
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%"
"%JAVA%\javac.exe" -d "%OUT%" "%ROUTE%\NativeCore.java" "%ROUTE%\route\MidiRoute.java" "%ROUTE%\route\BoundedCall.java" "%ROUTE%\route\MidiCodec.java" "%ROUTE%\route\ByteSink.java" "%ROUTE%\route\MidiBatcher.java" "%ROUTE%\route\RouteDevice.java" "%ROUTE%\route\FakeMidiRoute.java" "%ROUTE%\route\RouteBench.java" "%ROUTE%\route\RouteNative.java" "%ROUTE%\PlayerBridge.java" "%HERE%..\player\src\main\java\org\midibottleneck\player\RouteCatalog.java" "%HERE%CatalogHostTest.java" "%HERE%PacingHostTest.java" "%HERE%RouteHostTest.java" "%HERE%..\player\src\main\java\org\midibottleneck\player\Palette.java" "%HERE%PaletteHostTest.java" "%HERE%..\player\src\main\java\org\midibottleneck\player\Prefs.java" "%HERE%PrefsHostTest.java" "%HERE%..\player\src\main\java\org\midibottleneck\player\SliderCurve.java" "%HERE%..\player\src\main\java\org\midibottleneck\player\NumberScrub.java" "%HERE%TouchHostTest.java"
if errorlevel 1 exit /b 1
"%JAVA%\java.exe" -cp "%OUT%" PaletteHostTest "%HERE%..\player\src\main\res"
if errorlevel 1 exit /b 1
"%JAVA%\java.exe" -cp "%OUT%" org.midibottleneck.player.PrefsHostTest
if errorlevel 1 exit /b 1
"%JAVA%\java.exe" -cp "%OUT%" org.midibottleneck.player.TouchHostTest
if errorlevel 1 exit /b 1
"%JAVA%\java.exe" -cp "%OUT%" RouteHostTest
exit /b %errorlevel%
