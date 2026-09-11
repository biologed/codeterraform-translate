@echo off

::Launches the game with translations-wrapped around injector.exe (--launch mode).
::Just double-click this file instead of code-terraform.exe.
::dp0 is the folder where this .bat file is located, so if you move the game's folders
::in their entirety (for example, to another drive), the paths will be recalculated automatically.

"%~dp0injector.exe" --launch "%~dp0code-terraform.exe" "%~dp0Translator.dll"
