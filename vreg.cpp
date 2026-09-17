/************************************************************************
vreg.cpp - see vreg.h.
************************************************************************/

#include <windows.h>
#include <string.h>
#include <stdio.h>
#include "vreg.h"

#define VREG_MAX_ENTRIES  900
#define VREG_MAX_HANDLES  16
#define VREG_PATH_LEN     160
#define VREG_NAME_LEN     32
#define VREG_DATA_LEN     260

typedef struct {
   BOOL  fUsed;
   char  szPath[VREG_PATH_LEN];   // key path, relative to the virtual root
   char  szName[VREG_NAME_LEN];   // value name ("" is the unnamed/default value)
   DWORD dwType;
   BYTE  abData[VREG_DATA_LEN];
   DWORD cbData;
   } VREGENTRY;

typedef struct {
   BOOL  fUsed;
   BOOL  fDenied;                 // always resolves to "not found" (HKCR/HKU/HKCC)
   char  szPath[VREG_PATH_LEN];
   } VREGHANDLE;

static VREGENTRY  g_aEntries[VREG_MAX_ENTRIES];
static int        g_cEntries = 0;
static VREGHANDLE g_aHandles[VREG_MAX_HANDLES];

/* the ten import slots we rewrite, so VReg_Uninstall() can put them back */
static void **g_apPatchedSlots[10];
static void  *g_apPatchedOriginals[10];
static int    g_cPatched = 0;

/* a stable, out-of-range value so our handles can never collide with a
   real predefined key (those all have the top bit set, e.g.
   HKEY_LOCAL_MACHINE == 0x80000002) or with a plausible heap pointer */
/* VC6's own winnt.h has no ULONG_PTR (it predates the 32/64-bit split
   that introduced it), and this build is 32-bit only anyway, so plain
   DWORD stands in for a pointer-sized integer throughout this file. */
#define VREG_HANDLE_BASE ((DWORD) 0x56490000)   /* 'VI' */

static BOOL IsPredefinedKey(HKEY hKey)
{
   return (((DWORD) hKey) & 0x80000000u) != 0;
}

static void CopyPath(char *pszDest, const char *pszSrc)
{
   lstrcpynA(pszDest, pszSrc ? pszSrc : "", VREG_PATH_LEN);
}

static void JoinPath(char *pszDest, const char *pszBase, const char *pszSub)
{
   if (!pszBase || !*pszBase) {
      CopyPath(pszDest, pszSub);
   } else if (!pszSub || !*pszSub) {
      CopyPath(pszDest, pszBase);
   } else {
      char szTemp[VREG_PATH_LEN];
      wsprintfA(szTemp, "%s\\%s", pszBase, pszSub);
      CopyPath(pszDest, szTemp);
   }
}

/* ---- entry table ---- */

static VREGENTRY *FindEntry(const char *pszPath, const char *pszName)
{
   int i;
   if (!pszName) pszName = "";
   for (i = 0; i < g_cEntries; i++) {
      if (g_aEntries[i].fUsed &&
          !lstrcmpiA(g_aEntries[i].szPath, pszPath) &&
          !lstrcmpiA(g_aEntries[i].szName, pszName)) {
         return &g_aEntries[i];
         }
      }
   return NULL;
}

/* Whether `pszPath` names a key that has been seeded - either directly
   (it owns at least one value) or because something exists under it. */
static BOOL PathExists(const char *pszPath)
{
   int i;
   size_t cch = strlen(pszPath);
   if (cch == 0) {
      return TRUE;   // the virtual root itself
      }
   for (i = 0; i < g_cEntries; i++) {
      if (!g_aEntries[i].fUsed) continue;
      if (!lstrcmpiA(g_aEntries[i].szPath, pszPath)) {
         return TRUE;
         }
      if (_strnicmp(g_aEntries[i].szPath, pszPath, cch) == 0 &&
          g_aEntries[i].szPath[cch] == '\\') {
         return TRUE;
         }
      }
   return FALSE;
}

/* Collects the distinct immediate child key names directly under
   `pszPath` into `aszOut` (each up to VREG_NAME_LEN), returns the count.
   Small, linear, and called only a few dozen times at start-up, so no
   need for anything cleverer. */
static int CollectChildren(const char *pszPath, char aszOut[][VREG_NAME_LEN], int cMax)
{
   int i, j, cFound = 0;
   size_t cchPrefix = strlen(pszPath);

   for (i = 0; i < g_cEntries && cFound < cMax; i++) {
      const char *pszRest;
      char szChild[VREG_NAME_LEN];
      size_t cchChild;
      BOOL fSeen;

      if (!g_aEntries[i].fUsed) continue;
      if (cchPrefix == 0) {
         pszRest = g_aEntries[i].szPath;
         if (!*pszRest) continue;
         }
      else {
         if (_strnicmp(g_aEntries[i].szPath, pszPath, cchPrefix) != 0) continue;
         if (g_aEntries[i].szPath[cchPrefix] != '\\') continue;
         pszRest = g_aEntries[i].szPath + cchPrefix + 1;
         }

      cchChild = strcspn(pszRest, "\\");
      if (cchChild == 0 || cchChild >= VREG_NAME_LEN) continue;
      memcpy(szChild, pszRest, cchChild);
      szChild[cchChild] = '\0';

      fSeen = FALSE;
      for (j = 0; j < cFound; j++) {
         if (!lstrcmpiA(aszOut[j], szChild)) { fSeen = TRUE; break; }
         }
      if (!fSeen) {
         lstrcpynA(aszOut[cFound], szChild, VREG_NAME_LEN);
         cFound++;
         }
      }
   return cFound;
}

static VREGENTRY *AllocEntry(void)
{
   int i;
   for (i = 0; i < VREG_MAX_ENTRIES; i++) {
      if (!g_aEntries[i].fUsed) {
         memset(&g_aEntries[i], 0, sizeof(VREGENTRY));
         g_aEntries[i].fUsed = TRUE;
         if (i >= g_cEntries) g_cEntries = i + 1;
         return &g_aEntries[i];
         }
      }
   return NULL;
}

static BOOL SetValue(const char *pszPath, const char *pszName, DWORD dwType,
                      const void *pData, DWORD cbData)
{
   VREGENTRY *pEntry = FindEntry(pszPath, pszName);
   if (!pEntry) {
      pEntry = AllocEntry();
      if (!pEntry) return FALSE;
      CopyPath(pEntry->szPath, pszPath);
      lstrcpynA(pEntry->szName, pszName ? pszName : "", VREG_NAME_LEN);
      }
   pEntry->dwType = dwType;
   pEntry->cbData = (cbData < VREG_DATA_LEN) ? cbData : (VREG_DATA_LEN - 1);
   if (pData && pEntry->cbData) {
      memcpy(pEntry->abData, pData, pEntry->cbData);
      }
   return TRUE;
}

/* ---- public seeding API ---- */

BOOL VReg_SetString(const char *pszPath, const char *pszName, const char *pszValue)
{
   const char *psz = pszValue ? pszValue : "";
   return SetValue(pszPath, pszName, REG_SZ, psz, (DWORD) (strlen(psz) + 1));
}

void VReg_Reset(void)
{
   memset(g_aEntries, 0, sizeof(g_aEntries));
   g_cEntries = 0;
   memset(g_aHandles, 0, sizeof(g_aHandles));
}

/* ---- handle table ---- */

static VREGHANDLE g_hRoot   = { TRUE, FALSE, "" };
static VREGHANDLE g_hDenied = { TRUE, TRUE,  "" };

static VREGHANDLE *ResolveHandle(HKEY hKey)
{
   if (hKey == HKEY_LOCAL_MACHINE || hKey == HKEY_CURRENT_USER) {
      return &g_hRoot;
      }
   if (IsPredefinedKey(hKey)) {
      // HKCR/HKU/HKCC and friends: deliberately empty, so the engine
      // finding nothing there is proof the configuration came from here.
      return &g_hDenied;
      }
   {
      DWORD idx = ((DWORD) hKey) - VREG_HANDLE_BASE;
      if (idx < VREG_MAX_HANDLES && g_aHandles[idx].fUsed) {
         return &g_aHandles[idx];
         }
      }
   return NULL;
}

static HKEY MakeHandle(const char *pszPath)
{
   int i;
   for (i = 0; i < VREG_MAX_HANDLES; i++) {
      if (!g_aHandles[i].fUsed) {
         g_aHandles[i].fUsed = TRUE;
         g_aHandles[i].fDenied = FALSE;
         CopyPath(g_aHandles[i].szPath, pszPath);
         return (HKEY) (VREG_HANDLE_BASE + (DWORD) i);
         }
      }
   return NULL;
}

/* ---- the ten replacements. Signatures must match ADVAPI32's exactly. ---- */

static LONG WINAPI VReg_RegOpenKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD ulOptions,
                                       REGSAM samDesired, PHKEY phkResult)
{
   char szPath[VREG_PATH_LEN];
   VREGHANDLE *pParent = ResolveHandle(hKey);
   (void) ulOptions; (void) samDesired;
   if (!pParent || pParent->fDenied) return ERROR_FILE_NOT_FOUND;
   if (!phkResult) return ERROR_INVALID_PARAMETER;

   JoinPath(szPath, pParent->szPath, lpSubKey);
   if (!PathExists(szPath)) return ERROR_FILE_NOT_FOUND;

   *phkResult = MakeHandle(szPath);
   return *phkResult ? ERROR_SUCCESS : ERROR_TOO_MANY_OPEN_FILES;
}

static LONG WINAPI VReg_RegCreateKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD Reserved,
                                         LPSTR lpClass, DWORD dwOptions, REGSAM samDesired,
                                         LPSECURITY_ATTRIBUTES lpSA, PHKEY phkResult,
                                         LPDWORD lpdwDisposition)
{
   char szPath[VREG_PATH_LEN];
   BOOL fExisted;
   VREGHANDLE *pParent = ResolveHandle(hKey);
   (void) Reserved; (void) lpClass; (void) dwOptions; (void) samDesired; (void) lpSA;
   if (!pParent || pParent->fDenied) return ERROR_ACCESS_DENIED;
   if (!phkResult) return ERROR_INVALID_PARAMETER;

   JoinPath(szPath, pParent->szPath, lpSubKey);
   fExisted = PathExists(szPath);
   *phkResult = MakeHandle(szPath);
   if (!*phkResult) return ERROR_TOO_MANY_OPEN_FILES;
   if (lpdwDisposition) {
      *lpdwDisposition = fExisted ? REG_OPENED_EXISTING_KEY : REG_CREATED_NEW_KEY;
      }
   return ERROR_SUCCESS;
}

static LONG WINAPI VReg_RegQueryValueExA(HKEY hKey, LPCSTR lpValueName, LPDWORD lpReserved,
                                          LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
   VREGENTRY *pEntry;
   VREGHANDLE *pNode = ResolveHandle(hKey);
   (void) lpReserved;
   if (!pNode) return ERROR_INVALID_HANDLE;
   if (pNode->fDenied) return ERROR_FILE_NOT_FOUND;

   pEntry = FindEntry(pNode->szPath, lpValueName);
   if (!pEntry) return ERROR_FILE_NOT_FOUND;

   if (lpType) *lpType = pEntry->dwType;
   if (!lpData) {
      if (lpcbData) *lpcbData = pEntry->cbData;
      return ERROR_SUCCESS;
      }
   if (!lpcbData) return ERROR_INVALID_PARAMETER;
   if (*lpcbData < pEntry->cbData) {
      *lpcbData = pEntry->cbData;
      return ERROR_MORE_DATA;
      }
   memcpy(lpData, pEntry->abData, pEntry->cbData);
   *lpcbData = pEntry->cbData;
   return ERROR_SUCCESS;
}

static LONG WINAPI VReg_RegSetValueExA(HKEY hKey, LPCSTR lpValueName, DWORD Reserved,
                                        DWORD dwType, CONST BYTE *lpData, DWORD cbData)
{
   VREGHANDLE *pNode = ResolveHandle(hKey);
   (void) Reserved;
   if (!pNode) return ERROR_INVALID_HANDLE;
   if (pNode->fDenied) return ERROR_ACCESS_DENIED;
   // Kept in memory only, same as every other value here; nothing the
   // engine writes at run time needs to survive past this process.
   SetValue(pNode->szPath, lpValueName, dwType, lpData, cbData);
   return ERROR_SUCCESS;
}

static LONG WINAPI VReg_RegSetValueA(HKEY hKey, LPCSTR lpSubKey, DWORD dwType,
                                      LPCSTR lpData, DWORD cbData)
{
   char szPath[VREG_PATH_LEN];
   VREGHANDLE *pParent = ResolveHandle(hKey);
   if (!pParent) return ERROR_INVALID_HANDLE;
   if (pParent->fDenied) return ERROR_ACCESS_DENIED;
   JoinPath(szPath, pParent->szPath, lpSubKey);
   // RegSetValueA takes a NUL-terminated string and cbData excludes the NUL.
   SetValue(szPath, "", dwType, lpData, lpData ? (cbData + 1) : 0);
   return ERROR_SUCCESS;
}

static LONG WINAPI VReg_RegEnumKeyExA(HKEY hKey, DWORD dwIndex, LPSTR lpName,
                                       LPDWORD lpcchName, LPDWORD lpReserved, LPSTR lpClass,
                                       LPDWORD lpcchClass, PFILETIME lpftLastWriteTime)
{
   static char s_aszChildren[64][VREG_NAME_LEN];
   int cChildren;
   size_t cchName;
   VREGHANDLE *pNode = ResolveHandle(hKey);
   (void) lpReserved;
   if (!pNode) return ERROR_INVALID_HANDLE;
   if (pNode->fDenied) return ERROR_NO_MORE_ITEMS;

   cChildren = CollectChildren(pNode->szPath, s_aszChildren, 64);
   if ((DWORD) cChildren <= dwIndex) return ERROR_NO_MORE_ITEMS;
   if (!lpName || !lpcchName) return ERROR_INVALID_PARAMETER;

   cchName = strlen(s_aszChildren[dwIndex]);
   if (*lpcchName <= cchName) {
      *lpcchName = (DWORD) cchName;
      return ERROR_MORE_DATA;
      }
   memcpy(lpName, s_aszChildren[dwIndex], cchName + 1);
   *lpcchName = (DWORD) cchName;
   if (lpClass && lpcchClass) {
      if (*lpcchClass > 0) lpClass[0] = '\0';
      *lpcchClass = 0;
      }
   if (lpftLastWriteTime) {
      GetSystemTimeAsFileTime(lpftLastWriteTime);
      }
   return ERROR_SUCCESS;
}

static LONG WINAPI VReg_RegQueryInfoKeyA(HKEY hKey, LPSTR lpClass, LPDWORD lpcchClass,
                                          LPDWORD lpReserved, LPDWORD lpcSubKeys,
                                          LPDWORD lpcbMaxSubKeyLen, LPDWORD lpcbMaxClassLen,
                                          LPDWORD lpcValues, LPDWORD lpcbMaxValueNameLen,
                                          LPDWORD lpcbMaxValueLen, LPDWORD lpcbSecurityDescriptor,
                                          PFILETIME lpftLastWriteTime)
{
   static char s_aszChildren[64][VREG_NAME_LEN];
   int i, cChildren, cValues = 0;
   size_t cchMaxSubKey = 0, cchMaxName = 0, cbMaxValue = 0;
   VREGHANDLE *pNode = ResolveHandle(hKey);
   (void) lpReserved;
   if (!pNode) return ERROR_INVALID_HANDLE;
   if (pNode->fDenied) {
      if (lpcSubKeys) *lpcSubKeys = 0;
      if (lpcValues) *lpcValues = 0;
      return ERROR_SUCCESS;
      }

   cChildren = CollectChildren(pNode->szPath, s_aszChildren, 64);
   for (i = 0; i < cChildren; i++) {
      size_t cch = strlen(s_aszChildren[i]);
      if (cch > cchMaxSubKey) cchMaxSubKey = cch;
      }
   for (i = 0; i < g_cEntries; i++) {
      if (!g_aEntries[i].fUsed) continue;
      if (lstrcmpiA(g_aEntries[i].szPath, pNode->szPath)) continue;
      cValues++;
      if (strlen(g_aEntries[i].szName) > cchMaxName) cchMaxName = strlen(g_aEntries[i].szName);
      if (g_aEntries[i].cbData > cbMaxValue) cbMaxValue = g_aEntries[i].cbData;
      }

   if (lpClass && lpcchClass && *lpcchClass > 0) lpClass[0] = '\0';
   if (lpcchClass) *lpcchClass = 0;
   if (lpcSubKeys) *lpcSubKeys = (DWORD) cChildren;
   if (lpcbMaxSubKeyLen) *lpcbMaxSubKeyLen = (DWORD) cchMaxSubKey;
   if (lpcbMaxClassLen) *lpcbMaxClassLen = 0;
   if (lpcValues) *lpcValues = (DWORD) cValues;
   if (lpcbMaxValueNameLen) *lpcbMaxValueNameLen = (DWORD) cchMaxName;
   if (lpcbMaxValueLen) *lpcbMaxValueLen = (DWORD) cbMaxValue;
   if (lpcbSecurityDescriptor) *lpcbSecurityDescriptor = 0;
   if (lpftLastWriteTime) GetSystemTimeAsFileTime(lpftLastWriteTime);
   return ERROR_SUCCESS;
}

static LONG WINAPI VReg_RegCloseKey(HKEY hKey)
{
   DWORD idx = ((DWORD) hKey) - VREG_HANDLE_BASE;
   if (idx < VREG_MAX_HANDLES) {
      g_aHandles[idx].fUsed = FALSE;
      }
   return ERROR_SUCCESS;
}

static LONG WINAPI VReg_RegDeleteKeyA(HKEY hKey, LPCSTR lpSubKey)
{
   char szPath[VREG_PATH_LEN];
   int i;
   size_t cch;
   BOOL fFound = FALSE;
   VREGHANDLE *pParent = ResolveHandle(hKey);
   if (!pParent || pParent->fDenied) return ERROR_FILE_NOT_FOUND;

   JoinPath(szPath, pParent->szPath, lpSubKey);
   cch = strlen(szPath);
   for (i = 0; i < g_cEntries; i++) {
      if (!g_aEntries[i].fUsed) continue;
      if (!lstrcmpiA(g_aEntries[i].szPath, szPath) ||
          (_strnicmp(g_aEntries[i].szPath, szPath, cch) == 0 && g_aEntries[i].szPath[cch] == '\\')) {
         g_aEntries[i].fUsed = FALSE;
         fFound = TRUE;
         }
      }
   return fFound ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
}

static LONG WINAPI VReg_RegDeleteValueA(HKEY hKey, LPCSTR lpValueName)
{
   VREGENTRY *pEntry;
   VREGHANDLE *pNode = ResolveHandle(hKey);
   if (!pNode) return ERROR_INVALID_HANDLE;
   if (pNode->fDenied) return ERROR_FILE_NOT_FOUND;
   pEntry = FindEntry(pNode->szPath, lpValueName);
   if (!pEntry) return ERROR_FILE_NOT_FOUND;
   pEntry->fUsed = FALSE;
   return ERROR_SUCCESS;
}

/* ---- import address table patching ---- */

typedef struct {
   const char *pszName;
   void       *pReplacement;
   } VREGREDIRECT;

static const VREGREDIRECT g_aRedirects[] = {
   { "RegOpenKeyExA",    (void *) &VReg_RegOpenKeyExA },
   { "RegCreateKeyExA",  (void *) &VReg_RegCreateKeyExA },
   { "RegQueryValueExA", (void *) &VReg_RegQueryValueExA },
   { "RegSetValueExA",   (void *) &VReg_RegSetValueExA },
   { "RegSetValueA",     (void *) &VReg_RegSetValueA },
   { "RegEnumKeyExA",    (void *) &VReg_RegEnumKeyExA },
   { "RegQueryInfoKeyA", (void *) &VReg_RegQueryInfoKeyA },
   { "RegCloseKey",      (void *) &VReg_RegCloseKey },
   { "RegDeleteKeyA",    (void *) &VReg_RegDeleteKeyA },
   { "RegDeleteValueA",  (void *) &VReg_RegDeleteValueA },
   };
#define NUM_REDIRECTS (sizeof(g_aRedirects) / sizeof(g_aRedirects[0]))

/* Finds the slot `hModule` uses to call ADVAPI32.dll!pszFuncName. Only
   that one module's imports are affected - the exported function itself,
   and every other module's copy of the pointer to it, are untouched. */
static void **FindIatSlot(HMODULE hModule, const char *pszFuncName)
{
   BYTE *pBase = (BYTE *) hModule;
   IMAGE_DOS_HEADER *pDos = (IMAGE_DOS_HEADER *) pBase;
   IMAGE_NT_HEADERS *pNt;
   IMAGE_DATA_DIRECTORY *pDir;
   IMAGE_IMPORT_DESCRIPTOR *pDesc;

   if (pDos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
   pNt = (IMAGE_NT_HEADERS *) (pBase + pDos->e_lfanew);
   if (pNt->Signature != IMAGE_NT_SIGNATURE) return NULL;

   pDir = &pNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
   if (pDir->VirtualAddress == 0 || pDir->Size == 0) return NULL;

   pDesc = (IMAGE_IMPORT_DESCRIPTOR *) (pBase + pDir->VirtualAddress);
   for (; pDesc->Name; pDesc++) {
      const char *pszDllName = (const char *) (pBase + pDesc->Name);
      IMAGE_THUNK_DATA *pThunk, *pIat;

      if (lstrcmpiA(pszDllName, "ADVAPI32.dll") != 0) continue;

      // OriginalFirstThunk still holds the names after the loader has
      // bound the module; FirstThunk holds the addresses actually called.
      pThunk = (IMAGE_THUNK_DATA *)
         (pBase + (pDesc->OriginalFirstThunk ? pDesc->OriginalFirstThunk : pDesc->FirstThunk));
      pIat = (IMAGE_THUNK_DATA *) (pBase + pDesc->FirstThunk);

      for (; pThunk->u1.AddressOfData; pThunk++, pIat++) {
         IMAGE_IMPORT_BY_NAME *pImport;
         if (IMAGE_SNAP_BY_ORDINAL(pThunk->u1.Ordinal)) continue;
         // This SDK's IMAGE_THUNK_DATA32.u1.AddressOfData is typed as a
         // pointer even though it is really an RVA to be added to the
         // module base, so it has to go through an integer cast before
         // the addition below.
         pImport = (IMAGE_IMPORT_BY_NAME *) (pBase + (DWORD) pThunk->u1.AddressOfData);
         if (!strcmp((const char *) pImport->Name, pszFuncName)) {
            return (void **) &pIat->u1.Function;
            }
         }
      }
   return NULL;
}

static BOOL WriteSlot(void **ppSlot, void *pValue, void **ppPrevious)
{
   DWORD dwOldProtect, dwIgnored;
   if (!VirtualProtect(ppSlot, sizeof(void *), PAGE_READWRITE, &dwOldProtect)) {
      return FALSE;
      }
   if (ppPrevious) *ppPrevious = *ppSlot;
   *ppSlot = pValue;
   VirtualProtect(ppSlot, sizeof(void *), dwOldProtect, &dwIgnored);
   return TRUE;
}

BOOL VReg_Install(HMODULE hModule)
{
   unsigned i;
   BOOL fAll = TRUE;
   g_cPatched = 0;
   if (!hModule) return FALSE;

   for (i = 0; i < NUM_REDIRECTS; i++) {
      void **ppSlot = FindIatSlot(hModule, g_aRedirects[i].pszName);
      void *pOriginal = NULL;
      if (!ppSlot) { fAll = FALSE; continue; }
      if (!WriteSlot(ppSlot, g_aRedirects[i].pReplacement, &pOriginal)) { fAll = FALSE; continue; }
      g_apPatchedSlots[g_cPatched] = ppSlot;
      g_apPatchedOriginals[g_cPatched] = pOriginal;
      g_cPatched++;
      }
   return fAll;
}

void VReg_Uninstall(void)
{
   int i;
   for (i = g_cPatched - 1; i >= 0; i--) {
      WriteSlot(g_apPatchedSlots[i], g_apPatchedOriginals[i], NULL);
      }
   g_cPatched = 0;
}
