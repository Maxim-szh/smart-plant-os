@echo off
cd /d "%~dp0"
py -3 tools\flash.py %*
if "%~1"=="" pause
