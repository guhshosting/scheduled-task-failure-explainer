# Validation and release context: scheduled-task-failure-explainer 1.0.0

Development/build validation: Windows Server 2025 x64. The exact executable completed physical Windows 11 Pro 25H2 x64 qualification (build 26200.9550) and Windows 10 Pro 22H2 x64 qualification in a dedicated QEMU VM (build 19045.6466). Physical Windows 10 hardware and ARM64 were not qualified. Windows 11 completion is user supplied with retained raw results; the consolidated report is unavailable. Historical Windows 10 detailed ledgers are unavailable; completion is user supplied.

The final ZIP was assembled after executable qualification around the unchanged qualified EXE. Execution of this final assembled package on Windows is not claimed. Development validation is distinct from client qualification.

Qualified executable SHA-256: `6f7aab8a24103e47657a8efcb6177e40d1b331d8377c3ea278ea2f27a73dec00`.

No separately imported MSVC C++ runtime DLLs were found; Windows system libraries remain required.

See application documentation for query, output and safety limits. Windows may perform internal activity while servicing read-only API calls.
