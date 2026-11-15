@echo off
rem The simplest cook: copy_assets ROOT DEST copies one module's assets\ into its place in
rem a program's output tree. kormake.lua: cook "copy_assets {in} {out}". Nothing is
rem converted and nothing is removed; goldberg (the default cook) does both.
set "src=%~1"
set "dst=%~2"
xcopy /E /I /Q /Y "%src:/=\%" "%dst:/=\%" >nul
