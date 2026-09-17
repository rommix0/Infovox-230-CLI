/************************************************************************
vreg.h - A registry that exists only in this process's memory.

   The Infovox 230 v1.12 engine (Ivx230nt.dll) reads its entire
   configuration - where its rule files live, and its whole voice table -
   from HKEY_LOCAL_MACHINE\Software\Telia Promotor\Infovox 230\1.1. It
   reaches the registry through exactly ten imported ANSI functions, all
   from ADVAPI32.DLL:

       RegOpenKeyExA   RegCreateKeyExA  RegQueryValueExA  RegSetValueExA
       RegSetValueA    RegEnumKeyExA    RegQueryInfoKeyA  RegCloseKey
       RegDeleteKeyA   RegDeleteValueA

   Rewriting those ten slots in the engine module's import address table
   to point at the functions below is what lets speak.exe hand the engine
   a voice table without an administrator, and without ever touching the
   real Windows registry - the same "no registration" approach the rest
   of this program already uses for loading the engine itself.

   Ported (not copied wholesale) from the same idea in the infovox
   project's ivx_vregistry.cpp/h, which targets Visual Studio 2022. This
   version uses no STL and no C++11, so it can compile under Visual C++ 6
   like the rest of speak.exe: everything is fixed-size arrays and plain
   functions.
************************************************************************/

#ifndef _VREG_
#define _VREG_

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Seeds one string value at the given path (backslash-separated,
   relative to the virtual HKLM/HKCU root - e.g.
   "Software\\Telia Promotor\\Infovox 230\\1.1\\Modes\\American English Male").
   Must be called before VReg_Install(); returns FALSE if the table is
   full. */
BOOL VReg_SetString(const char *pszPath, const char *pszName, const char *pszValue);

/* Forgets everything seeded so far. */
void VReg_Reset(void);

/* Rewrites hModule's ten ADVAPI32 registry imports to read from the
   table built with VReg_SetString(). Returns FALSE if even one of the
   ten could not be found or patched, in which case the engine would
   still be able to reach the real registry through it. */
BOOL VReg_Install(HMODULE hModule);

/* Restores the ten import slots VReg_Install() rewrote. */
void VReg_Uninstall(void);

#ifdef __cplusplus
}
#endif

#endif /* _VREG_ */
