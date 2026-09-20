@echo off
rem msvc_env.bat — 设置 MSVC x64 构建环境 (V6-R3)
rem 用法: scripts\msvc_env.bat <command args...>
call "%ProgramFiles%\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 exit /b 1
%*
exit /b %errorlevel%
