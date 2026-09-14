# Building Cinematic Conversation Camera

Use Windows x64, Visual Studio 2022 with the v143 C++ toolset, a Windows SDK,
CMake (3.20 or newer), Git, and vcpkg. Run commands from this source directory
in a Developer PowerShell. The release uses the `x64-windows-static-md` triplet.

The source archive includes CommonLibSSE-NG 7.2.0 at commit
`7a60f4de794095d7b0f8928d1b930a52e9a7da83` and OpenVR at
`60eb187801956ad277f1cae6680e3a410ee0873b`.
No submodule download is needed when building the archive. When building a Git
checkout instead, first run `git submodule update --init --recursive`.

```powershell
$env:VCPKG_ROOT = 'C:\dev\vcpkg' # Set to your vcpkg checkout.
cmake --preset skyrim -DSD_BUILD_TESTS=ON -DCOMMONLIB_PREBUILT=OFF -DFETCHCONTENT_SOURCE_DIR_HDE64="$PWD/third-party-sources/minhook"
cmake --build --preset skyrim-release --parallel 8
ctest --test-dir build/skyrim -C Release --output-on-failure
```

For a Git checkout without the packaged dependency sources, omit
`FETCHCONTENT_SOURCE_DIR_HDE64`; CMake downloads the pinned MinHook revision.

The output is `build/skyrim/Release/SceneDirector.dll`. Install it and
`config/SD.ini` under `Data/SKSE/Plugins` with the runtime requirements in README.md.
Do not replace SD_user.ini. No game files or optional regional head asset are
needed to compile. Automated tests do not certify behavior inside Skyrim.

The vcpkg registry baseline is pinned in vcpkg-configuration.json. The archive's
third-party-sources directory contains the available patched source trees used
for the release dependencies; third-party-build-info includes installed version
and SPDX records. Normal CMake configuration uses vcpkg to fetch/build the pinned
dependencies and needs internet access. These source copies are supplied for
inspection and modification; this is not an offline toolchain distribution.

## Packaging

`tools/Package.ps1` builds and tests the release, then creates binary and source
ZIPs plus SHA256 sidecars. Pass `-CMake <path-to-cmake.exe>` if CMake is not on
PATH, and `-VcpkgRoot <path>` to locate dependency sources. `-SkipBuild` is for
an already verified build; it still runs tests and checks the DLL version.
The package includes licensing/readme files and never includes SD_user.ini,
game assets, personal logs, or Git history. Packaging uses a fresh staging
directory and backs up any existing same-version artifacts.
