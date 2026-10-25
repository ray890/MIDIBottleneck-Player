@echo off
rem P13-USB host tests (plain Java, no Android classes): never-freeze (fake routes that block on open, write, close),
rem USB-MIDI packet encoding, the USB sender thread, the write trace. JDK from the Android kit (per process only).
setlocal
if "%ANDROID_KIT%"=="" set ANDROID_KIT=D:\Android
set JAVA=%ANDROID_KIT%\jdk-17\bin
set HERE=%~dp0
set CORE=%HERE%..\core\src\main\java\org\midibottleneck\core
set PLAYER=%HERE%..\player\src\main\java\org\midibottleneck\player
set OUT=%HERE%..\..\out\android-usb-host-tests
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%"
"%JAVA%\javac.exe" -d "%OUT%" "%CORE%\NativeCore.java" "%CORE%\PlayerBridge.java" "%CORE%\route\MidiRoute.java" "%CORE%\route\RouteDevice.java" "%CORE%\route\BoundedCall.java" "%CORE%\route\RouteTrace.java" "%CORE%\route\UsbMidiPackets.java" "%CORE%\route\UsbTransport.java" "%CORE%\route\UsbMidiSender.java" "%PLAYER%\RouteCatalog.java" "%PLAYER%\OutputOpener.java" "%HERE%org\midibottleneck\player\OutputOpenerTestAccess.java" "%HERE%NeverFreezeHostTest.java" "%HERE%UsbHostTest.java"
if errorlevel 1 exit /b 1
"%JAVA%\java.exe" -cp "%OUT%" NeverFreezeHostTest
if errorlevel 1 exit /b 1
"%JAVA%\java.exe" -cp "%OUT%" UsbHostTest
exit /b %errorlevel%
