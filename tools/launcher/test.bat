@echo off
rem ABOUTME: Builds and runs the test of the lobby client and the player-hosted server (LobbyTest.cs + LobbyClient.cs + ServerHost.cs + FirewallCheck.cs + Reach.cs).
rem ABOUTME: Needs native\net\build\wotm_server.exe (tools\build_net.bat). Exits with 0 on success.
setlocal
set "CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if not exist "%CSC%" set "CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe"
set "OUT=%~dp0..\..\native\net\build\LobbyTest.exe"
"%CSC%" /nologo /target:exe /optimize+ /out:"%OUT%" /r:Microsoft.CSharp.dll "%~dp0LobbyTest.cs" "%~dp0LobbyClient.cs" "%~dp0ServerHost.cs" "%~dp0FirewallCheck.cs" "%~dp0Reach.cs" || exit /b 1
"%OUT%" "%~dp0..\..\native\net\build\wotm_server.exe"
