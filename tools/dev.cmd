@echo off
setlocal
for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "STFE_VS=%%I"
if not defined STFE_VS exit /b 1
call "%STFE_VS%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
%*
exit /b %errorlevel%
