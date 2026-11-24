// COM server and TSF registration (regsvr32, as an administrator; tools/register.ps1). This is all
// bada keeps in the registry: its settings are files (docs/bada-compatible-v2.md §10.1, §12).
#pragma once

#include <windows.h>

namespace bada::tip {

HRESULT RegisterServer(HINSTANCE module);
HRESULT UnregisterServer();
HRESULT RegisterProfiles(HINSTANCE module);
HRESULT UnregisterProfiles();
HRESULT RegisterCategories();
HRESULT UnregisterCategories();

}  // namespace bada::tip
