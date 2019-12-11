@echo [INFO] Input PTEID_DIR_OPENSSL_098_X64=%PTEID_DIR_OPENSSL_098_X64%
@set FILE_TO_FIND="bin\libcrypto-1_1-x64.dll" "bin\libssl-1_1-x64.dll" "lib\libcrypto.lib" "lib\libssl.lib"
@echo [INFO] Looking for files: %FILE_TO_FIND%

@set FILE_NOT_FOUND=
@for %%i in (%FILE_TO_FIND%) do @if not exist "%PTEID_DIR_OPENSSL_098_X64%\%%~i" set FILE_NOT_FOUND=%%~i
@if "%FILE_NOT_FOUND%"=="" goto find_openssl_098
@echo        Not found in "%PTEID_DIR_OPENSSL_098_X64%"

@echo [ERROR] OpenSSL 1.1 could not be found
@exit /B 1

:find_openssl_098
@echo        Found in "%PTEID_DIR_OPENSSL_098_X64%"
@exit /B 0