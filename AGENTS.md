# Agent instructions for this OpenRCT2 checkout

Local guidance for working on the game source. User instructions take precedence.

## Sources and scope

Consult these when their subject is relevant; prefer the checked-in build files and
formatting configuration when an older wiki example disagrees with this revision.

- [Contributing](CONTRIBUTING.md)
- [How to contribute](https://github.com/OpenRCT2/OpenRCT2/wiki/How-To-Contribute)
- [Coding style](https://github.com/OpenRCT2/OpenRCT2/wiki/Coding-Style)
- [Windows builds](https://github.com/OpenRCT2/OpenRCT2/wiki/Building-OpenRCT2-on-Windows)
- Build/test implementation: `openrct2.proj`, `openrct2.sln`,
  `openrct2.common.props`, `test/tests/tests.vcxproj`, `scripts/run-tests`.

## Working on changes

- Our maintained base branch is `agents`. Start future feature and bug-fix
  branches from the latest `origin/agents` so they inherit this file. Merge
  completed local changes back into `agents` when requested.
- Our sync and push remote is `origin`: https://github.com/jantoney/openrct2.git.
  `upstream` is https://github.com/OpenRCT2/OpenRCT2.git. Incorporate upstream
  `develop` updates into `agents` when requested, preserving this guidance.
- For contributions to the official project, upstream PRs target `develop`.
  Keep unrelated changes out of a task and preserve existing user work.
- For work intended for upstream, check existing issues/PRs. Upstream requests
  discussion with its team before new features. Local experiments can proceed;
  do not post messages or comments on the user's behalf without authorization.
- Reproduce bugs with a small save or clear steps when possible. Verify changed
  gameplay in the game as well as with relevant automated tests.
- Report what changed, validation actually performed, and any blocked checks.
  A successful build does not mean tests or an in-game check passed.

## Code conventions

- The current build uses C++20. Follow nearby code and existing abstractions.
- Use `.editorconfig` and `.clang-format`: UTF-8, four spaces, final newline,
  and a 128-column formatting limit. Format changed code without unrelated churn.
  CI's `scripts/check-code-formatting` uses clang-format 20 and modifies files.
- Follow the official naming examples: PascalCase functions, camelCase locals
  and arguments, `_`-prefixed private members, and `g`-prefixed globals.
  Match established conventions in the specific module; avoid wholesale renaming.
- Use explicit `nullptr` checks and `auto*` for deduced pointers. Explain intent
  and non-obvious constraints in comments rather than restating the code.
- Add base-language strings to `data/language/en-GB.txt`; translations belong in
  OpenRCT2/Localisation. The current core string-ID header is
  `src/openrct2/localisation/StringIds.h` (CONTRIBUTING.md has an older path).
- New source/test files may need registration in both CMake and MSVC project
  files. For tests, check `test/tests/CMakeLists.txt` and `test/tests/tests.vcxproj`.

## Windows build (PowerShell)

Run from the repository root. Prerequisites: Visual Studio 2022 or Build Tools
with C++ tools and a Windows SDK. The first build downloads project dependencies.
Release/x64 is the normal local configuration; Debug is available for debugging.

Discover MSBuild rather than assuming it is on PATH:

```powershell
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) { throw 'Visual Studio C++ build tools not found.' }
$msbuild = Join-Path $vsInstall 'MSBuild\Current\Bin\MSBuild.exe'
& $msbuild openrct2.proj /t:Build /p:Platform=x64 /p:Configuration=Release
if ($LASTEXITCODE -ne 0) { throw 'OpenRCT2 build failed.' }
```

The official Developer Command Prompt equivalent is:

```text
msbuild openrct2.proj /t:Build /p:Platform=x64 /p:Configuration=Release
```

The game is `bin/openrct2.exe`. Its original game assets must be configured for
interactive checks. Use a copy of a save when testing changes that alter parks.

## Windows tests (PowerShell)

The MSVC solution includes the `tests` project and produces `bin/tests.exe`.
Its post-build step copies fixtures into `bin/testdata`. Run from `bin`, since
fixture paths are relative. Scan objects first, matching `scripts/run-tests`:

```powershell
New-Item -ItemType Directory -Force artifacts | Out-Null
Push-Location bin
try {
    & .\openrct2-cli.exe scan-objects
    if ($LASTEXITCODE -ne 0) { throw 'Object scanning failed.' }
    & .\tests.exe --gtest_output=xml:../artifacts/test-results.xml
    if ($LASTEXITCODE -ne 0) { throw 'OpenRCT2 tests failed.' }
} finally {
    Pop-Location
}
```

For focused checks, run `tests.exe --gtest_list_tests` from `bin`, then pass an
appropriate `--gtest_filter=SuiteName.*` to the test invocation above. Describe
filtered runs as such. Resolve missing data/dependencies rather than interpreting
missing tests or fixture failures as successful validation.

CMake is an alternative build path: it requires CMake 3.24+, dependencies, and
`WITH_TESTS=ON`; the test target is `OpenRCT2Tests`, discovered through CTest.
Do not use CTest commands against the MSBuild output directory.

## Verification record

Checked on 2026-09-30 against checkout `e93160cebb` and the official guides.
MSBuild 17.14.40.60911 runs from `C:\BuildTools`; vswhere confirms the C++ tools.
MSBuild property evaluation confirmed the test output as
`D:\DevWork\OpenRCT2\bin\tests.exe`. Build/test commands above were cross-checked
against project files and the Windows path in `scripts/run-tests`.
A full build, dependency download, test run, and in-game check have not been
performed as part of creating this document. Recheck tools and targets if the
checkout or machine changes; do not treat this record as a passing test result.
