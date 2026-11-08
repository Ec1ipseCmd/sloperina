@echo off
rem Starts the ootmenu overlay with no console window. Start it before or after lichii.exe.
start "" pythonw "%~dp0ootmenu.pyw" %*
