@echo [INFO] Input PTEID_DIR_XERCES_31=%PTEID_DIR_XERCES_31%
@set FILE_TO_FIND="bin\xerces-c_3_2.dll" "lib\xerces-c_3.lib"
@echo [INFO] Looking for files: %FILE_TO_FIND%

@set FILE_NOT_FOUND=
@for %%i in (%FILE_TO_FIND%) do @if not exist "%PTEID_DIR_XERCES_31%\%%~i" set FILE_NOT_FOUND=%%~i
@if "%FILE_NOT_FOUND%"=="" goto find_xerces_32
@echo        Not found in "%PTEID_DIR_XERCES_31%"

@echo [ERROR] Xerces 3.2 could not be found
@exit /B 1

:find_xerces_32
@echo        Found in "%PTEID_DIR_XERCES_31%"
@exit /B 0