# Third-party notices

The project license does not replace the licenses of the following components.
Full dependency license notices are included under licenses/ in each release.

- CommonLibSSE-NG 7.2.0, alandtse and contributors, based on CommonLibSSE and
  CharmedBaryon's CommonLibSSE-NG: GPL-3.0-or-later with the additional permissions
  in EXCEPTIONS.md. Original MIT attributions are preserved as well.
  https://github.com/alandtse/CommonLibSSE-NG
- OpenVR, Valve: BSD-style license, preserved in licenses/OpenVR-LICENSE.txt.
  The complete pinned SDK snapshot accompanies CommonLib in the source archive.
- MinHook / Hacker Disassembler Engine 64, Tsuda Kageyu and Vyacheslav Patkov:
  BSD-style notices in licenses/MinHook-LICENSE.txt. CommonLib uses the instruction
  decoder for patch safety. Its pinned source is included under third-party-sources.
- fmt, spdlog, nlohmann-json, rapidcsv, DirectXMath and DirectXTK: upstream
  copyright/license texts are copied from the installed vcpkg packages. Matching
  installed metadata and available patched source trees accompany the source ZIP.
- SKSE Menu Framework API header, copied in include/SKSEMenuFramework.h:
  LGPL-2.1; see licenses/SKSEMenuFramework-LICENSE.txt. The external framework
  DLL is obtained separately and can be replaced independently.
- SmoothCam API, mwilsnd: include/SD/Compat/SmoothCamAPI.h preserves the upstream
  invitation to copy the header for API use. This release includes that API
  header, not the SmoothCam implementation or DLL.
  https://github.com/mwilsnd/SkyrimSE-SmoothCam
- Dragonborn ReVoiced API: include/SD/Compat/DBReV_API.h preserves its explicit
  permission to use, modify and redistribute the declaration header. DBReV's
  implementation and DLL are not bundled.
- SKSE and the Windows SDK/runtime remain separately supplied prerequisites.
  CommonLib's modding/linking exceptions describe their permitted interoperation.

Thanks to Alternate Conversation Camera's authors for earlier dialogue-camera
work, and to the authors of the compatible camera and player-voice mods.
