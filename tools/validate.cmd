@echo off
setlocal
if not exist artifacts mkdir artifacts
call tools\dev.cmd cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSTFE_INTEGRATION=ON >artifacts\debug-configure.txt 2>&1
if errorlevel 1 exit /b 1
call tools\dev.cmd cmake --build build-debug >artifacts\debug-build.txt 2>&1
if errorlevel 1 exit /b 1
ctest --test-dir build-debug --output-on-failure >artifacts\debug-ctest.txt 2>&1
if errorlevel 1 exit /b 1
call tools\dev.cmd cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTFE_INTEGRATION=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >artifacts\release-configure.txt 2>&1
if errorlevel 1 exit /b 1
call tools\dev.cmd cmake --build build-release >artifacts\release-build.txt 2>&1
if errorlevel 1 exit /b 1
ctest --test-dir build-release --output-on-failure >artifacts\release-ctest.txt 2>&1
if errorlevel 1 exit /b 1
call tools\dev.cmd cmake -S . -B build-analyze -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTFE_ANALYZE=ON -DSTFE_INTEGRATION=ON >artifacts\analyze-configure.txt 2>&1
if errorlevel 1 exit /b 1
call tools\dev.cmd cmake --build build-analyze >artifacts\analyze-build.txt 2>&1
if errorlevel 1 exit /b 1
ctest --test-dir build-analyze --output-on-failure >artifacts\analyze-ctest.txt 2>&1
if errorlevel 1 exit /b 1
call tools\dev.cmd cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSTFE_ASAN=ON -DSTFE_INTEGRATION=ON >artifacts\asan-configure.txt 2>&1
if errorlevel 1 exit /b 1
call tools\dev.cmd cmake --build build-asan >artifacts\asan-build.txt 2>&1
if errorlevel 1 exit /b 1
if not "%STFE_SKIP_ASAN_RUNTIME%"=="1" (
  ctest --test-dir build-asan --output-on-failure >artifacts\asan-ctest.txt 2>&1
  if errorlevel 1 exit /b 1
)
for %%F in (src\model.cpp src\acquire.cpp src\main.cpp tests\tests.cpp) do (
  call tools\dev.cmd "C:\Program Files\LLVM\bin\clang-tidy.exe" -p build-release %%F >artifacts\tidy-%%~nF.txt 2>&1
  if errorlevel 1 exit /b 1
)
"C:\Program Files\Cppcheck\cppcheck.exe" --enable=warning,style,performance,portability --error-exitcode=1 --std=c++17 --platform=win64 --library=windows --library=microsoft_sal --inline-suppr -Isrc -D_WIN32 -D_MSC_VER=1951 -DUNICODE -D_UNICODE src tests >artifacts\cppcheck.txt 2>&1
if errorlevel 1 exit /b 1
node tools\check-json.js build-release >artifacts\json-check.txt 2>&1
if errorlevel 1 exit /b 1
call tools\dev.cmd cmake -S . -B build-independent -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTFE_INTEGRATION=ON >artifacts\independent-configure.txt 2>&1
if errorlevel 1 exit /b 1
call tools\dev.cmd cmake --build build-independent >artifacts\independent-build.txt 2>&1
if errorlevel 1 exit /b 1
ctest --test-dir build-independent --output-on-failure >artifacts\independent-ctest.txt 2>&1
if errorlevel 1 exit /b 1
cmake --install build-release --prefix install >artifacts\install.txt 2>&1
if errorlevel 1 exit /b 1
install\bin\scheduled-task-failure-explainer.exe --json "\Microsoft\Windows\Defrag\ScheduledDefrag" >artifacts\installed-real-task.json 2>artifacts\installed-stderr.txt
if errorlevel 1 exit /b 1
node -e "JSON.parse(require('fs').readFileSync('artifacts/installed-real-task.json','utf8')); console.log('Installed JSON parsed');" >>artifacts\install.txt 2>&1
if errorlevel 1 exit /b 1
if "%STFE_SKIP_ASAN_RUNTIME%"=="1" (
  echo Validation passed with ASan runtime skipped. Run that suite separately and retain its output.
) else (
  echo All validation commands passed. See artifacts and VALIDATION.md.
)
