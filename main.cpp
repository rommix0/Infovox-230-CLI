/************************************************************************
main.cpp - Minimal command-line TTS driver for the Infovox 230 v1.12
   speech engine (Ivx230nt.dll), used without SAPI/COM registration and
   without touching the Windows registry.

   Mechanism:
     1. Seed an in-memory registry (vreg.h/.cpp) with the engine's whole
        60-voice mode table before it is ever asked for anything - see
        the note below on why this step exists.
     2. LoadLibrary() the engine DLL directly.
     3. Call its exported DllGetClassObject() with the engine's own
        class id (taken from the infovox project's reverse-engineering of
        this exact dll - not a registry lookup, and not the same thing as
        any one mode's id) to get an IClassFactory - no CoGetClassObject.
     4. CreateInstance() the engine's mode enumerator.
     5. Select(gModeID, ...) the chosen voice, passing our own in-process
        audio destination object (CAOut, adapted from Microsoft's AudioSD
        SDK sample) as the audio hookup point.
     6. Drive the resulting ITTSCentralW::TextData() to speak text.

   Why the in-memory registry: Ivx230nt.dll reads its entire
   configuration - where its rule files live, and its whole voice table -
   from HKEY_LOCAL_MACHINE\Software\Telia Promotor\Infovox 230\1.1, and
   enumerates zero modes if that key is empty. Unlike a registered SAPI
   engine, this program never installs anything, so vreg.cpp rewrites the
   engine's own ten ADVAPI32 registry imports to read from a table built
   in this process's memory instead - the exact voice data, and the exact
   quirks below, are taken from the infovox project (also in this
   workspace), which drives this identical engine as part of a SAPI5
   wrapper.

   Two more things specific to this engine, also taken from that project:
     - TextDataDone means the engine has copied the text, not that it has
       finished saying it. Finished is TextDataDone *and*
       ITTSNotifySinkW::AudioStop (registered via ITTSCentralW::Register),
       with a short grace period for an utterance that turns out to have
       nothing to say and so never raises AudioStart at all.
     - Some of this engine's internal notification handling responds to
       an AudioStop notification by calling IAudio::UnClaim() again
       before the first call returns, which would recurse forever without
       a guard; see m_fInsideNotify in audio.h/audioout.cpp.
************************************************************************/

#include <windows.h>
#include <dbt.h>
#include <mmsystem.h>
#include <ocidl.h>
#include <stdio.h>
#include <initguid.h>
#include <speech.h>
#include "audio.h"
#include "vreg.h"
#include "ivx_voices.h"

HINSTANCE ghInstance = NULL;
LONG gEnumObjectCount = 0;

/* The class id Ivx230nt.dll's own DllGetClassObject accepts - passing
   this to the dll's own DllGetClassObject is what replaces
   CoCreateInstance, and with it the entire SAPI4 runtime and any need
   for the engine to be registered. Not a mode id (see ivx_voices.h for
   those) and not read from anywhere - it is a fixed property of this
   dll, taken from the infovox project's own reverse-engineering
   (infovox/src/ivx_sapi4.h: CLSID_InfovoxEngine). */
DEFINE_GUID(CLSID_InfovoxEngine, 0xC9C5EDA0, 0x7C89, 0x11D0,
            0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);

/* Where the engine expects its configuration to live. Only the name
   matters - nothing under it ever reaches the real registry; see
   vreg.h. */
#define IVX_ENGINE_ROOT "Software\\Telia Promotor\\Infovox 230\\1.1"
#define IVX_MODES_ROOT  "Software\\Telia Promotor\\Infovox 230\\1.1\\Modes"


/* ---- minimal ITTSBufNotifySink so we know when speaking is done ---- */

class CBufNotify : public ITTSBufNotifySink {
   private:
      ULONG m_cRef;
   public:
      BOOL m_fDone;

      CBufNotify(void) : m_cRef(1), m_fDone(FALSE) {}
      ~CBufNotify(void) {}

      STDMETHODIMP QueryInterface(REFIID riid, LPVOID FAR *ppv)
      {
         if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITTSBufNotifySinkW)) {
            *ppv = (LPVOID) this;
            AddRef();
            return NOERROR;
         }
         *ppv = NULL;
         return ResultFromScode(E_NOINTERFACE);
      }

      STDMETHODIMP_(ULONG) AddRef(void) { return ++m_cRef; }
      STDMETHODIMP_(ULONG) Release(void)
      {
         if (--m_cRef == 0) { delete this; return 0; }
         return m_cRef;
      }

      STDMETHODIMP TextDataDone(QWORD qwTime, DWORD dwFlags)
      {
         (void) qwTime; (void) dwFlags;
         m_fDone = TRUE;
         return NOERROR;
      }
      STDMETHODIMP TextDataStarted(QWORD qwTime) { (void) qwTime; return NOERROR; }
      STDMETHODIMP BookMark(QWORD qwTime, DWORD dwMarkID) { (void) qwTime; (void) dwMarkID; return NOERROR; }
      STDMETHODIMP WordPosition(QWORD qwTime, DWORD dwPos) { (void) qwTime; (void) dwPos; return NOERROR; }
   };


/* ---- ITTSNotifySinkW so we know when the engine's audio has actually
   stopped, not just that it has finished handing us text. Registered via
   ITTSCentralW::Register(); see the top-of-file note. ---- */

class CNotify : public ITTSNotifySinkW {
   private:
      ULONG m_cRef;
   public:
      BOOL m_fStarted;
      BOOL m_fStopped;

      CNotify(void) : m_cRef(1), m_fStarted(FALSE), m_fStopped(FALSE) {}
      ~CNotify(void) {}

      STDMETHODIMP QueryInterface(REFIID riid, LPVOID FAR *ppv)
      {
         if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITTSNotifySinkW)) {
            *ppv = (LPVOID) this;
            AddRef();
            return NOERROR;
         }
         *ppv = NULL;
         return ResultFromScode(E_NOINTERFACE);
      }

      STDMETHODIMP_(ULONG) AddRef(void) { return ++m_cRef; }
      STDMETHODIMP_(ULONG) Release(void)
      {
         if (--m_cRef == 0) { delete this; return 0; }
         return m_cRef;
      }

      STDMETHODIMP AttribChanged(DWORD dwAttrib) { (void) dwAttrib; return NOERROR; }
      STDMETHODIMP AudioStart(QWORD qwTime) { (void) qwTime; m_fStarted = TRUE; return NOERROR; }
      STDMETHODIMP AudioStop(QWORD qwTime) { (void) qwTime; m_fStopped = TRUE; return NOERROR; }
      STDMETHODIMP Visual(QWORD qwTime, WCHAR cIPA, WCHAR cEngine, DWORD dwHints, PTTSMOUTH pMouth)
      {
         (void) qwTime; (void) cIPA; (void) cEngine; (void) dwHints; (void) pMouth;
         return NOERROR;
      }
   };


/* ---- optional WAV-file audio destination (-o <file.wav>), used instead
   of live playback. Implements the same trio of interfaces CAOut does
   (IAudio/IAudioDest/IAudioMultiMediaDevice) - Select() can't tell the
   difference - but just appends whatever PCM bytes the engine hands us
   to a file, using the WAVEFORMATEX the engine itself supplies via
   WaveFormatSet() to fill in the WAV header at the end. */

class CWavOut;

class CWavIAudio : public IAudio {
   public:
      CWavOut *m_pParent;
      CWavIAudio(CWavOut *pParent) : m_pParent(pParent) {}
      STDMETHODIMP QueryInterface(REFIID riid, LPVOID FAR *ppv);
      STDMETHODIMP_(ULONG) AddRef(void);
      STDMETHODIMP_(ULONG) Release(void);
      STDMETHODIMP Flush(void) { return NOERROR; }
      STDMETHODIMP LevelGet(DWORD *pdw) { if (!pdw) return ResultFromScode(E_INVALIDARG); *pdw = 0xFFFF; return NOERROR; }
      STDMETHODIMP LevelSet(DWORD dw) { (void) dw; return NOERROR; }
      STDMETHODIMP PassNotify(PVOID p, IID iid) { (void) p; (void) iid; return NOERROR; }
      STDMETHODIMP PosnGet(PQWORD pq);
      STDMETHODIMP Claim(void) { return NOERROR; }
      STDMETHODIMP UnClaim(void) { return NOERROR; }
      STDMETHODIMP Start(void) { return NOERROR; }
      STDMETHODIMP Stop(void) { return NOERROR; }
      STDMETHODIMP TotalGet(PQWORD pq);
      STDMETHODIMP ToFileTime(PQWORD pq, FILETIME *pft);
      STDMETHODIMP WaveFormatGet(PSDATA pd);
      STDMETHODIMP WaveFormatSet(SDATA d);
   };

class CWavIAudioDest : public IAudioDest {
   public:
      CWavOut *m_pParent;
      CWavIAudioDest(CWavOut *pParent) : m_pParent(pParent) {}
      STDMETHODIMP QueryInterface(REFIID riid, LPVOID FAR *ppv);
      STDMETHODIMP_(ULONG) AddRef(void);
      STDMETHODIMP_(ULONG) Release(void);
      STDMETHODIMP FreeSpace(DWORD *pdw, BOOL *pf) { if (pdw) *pdw = 0x7FFFFFFF; if (pf) *pf = FALSE; return NOERROR; }
      STDMETHODIMP DataSet(PVOID pBuf, DWORD dwSize);
      STDMETHODIMP BookMark(DWORD dw) { (void) dw; return NOERROR; }
   };

class CWavIAudioMultiMediaDevice : public IAudioMultiMediaDevice {
   public:
      CWavOut *m_pParent;
      CWavIAudioMultiMediaDevice(CWavOut *pParent) : m_pParent(pParent) {}
      STDMETHODIMP QueryInterface(REFIID riid, LPVOID FAR *ppv);
      STDMETHODIMP_(ULONG) AddRef(void);
      STDMETHODIMP_(ULONG) Release(void);
      STDMETHODIMP CustomMessage(UINT u, SDATA d) { (void) u; (void) d; return NOERROR; }
      STDMETHODIMP DeviceNumGet(DWORD *pdw) { if (pdw) *pdw = 0; return NOERROR; }
      STDMETHODIMP DeviceNumSet(DWORD dw) { (void) dw; return NOERROR; }
   };

class CWavOut : public IUnknown {
   public:
      ULONG    m_cRef;
      FILE     *m_pFile;
      QWORD    m_qwTotal;      // bytes written so far (== "position", since writes are synchronous)
      BYTE     m_abFmt[64];    // raw WAVEFORMATEX bytes as given to WaveFormatSet()
      DWORD    m_dwFmtSize;

      CWavIAudio                  m_iAudio;
      CWavIAudioDest              m_iAudioDest;
      CWavIAudioMultiMediaDevice  m_iAudioMMDevice;

      CWavOut(void)
         : m_cRef(0), m_pFile(NULL), m_qwTotal(0), m_dwFmtSize(0),
           m_iAudio(this), m_iAudioDest(this), m_iAudioMMDevice(this)
      {
         memset(m_abFmt, 0, sizeof(m_abFmt));
      }
      ~CWavOut(void) { Close(); }

      BOOL Open(const char *pszPath)
      {
         m_pFile = fopen(pszPath, "wb");
         if (!m_pFile) return FALSE;
         BYTE hdr[44];
         memset(hdr, 0, sizeof(hdr));   // placeholder, patched by Close()
         fwrite(hdr, 1, sizeof(hdr), m_pFile);
         return TRUE;
      }

      /* Idempotent: safe to call more than once (e.g. explicitly, then
         again from the destructor if it ever runs). */
      void Close(void)
      {
         if (!m_pFile) return;

         WAVEFORMATEX wfx;
         memset(&wfx, 0, sizeof(wfx));
         if (m_dwFmtSize >= 16) {
            DWORD dwCopy = (m_dwFmtSize < sizeof(wfx)) ? m_dwFmtSize : sizeof(wfx);
            memcpy(&wfx, m_abFmt, dwCopy);
         } else {
            /* engine never called WaveFormatSet - shouldn't happen in
               practice, but fall back to something playable */
            wfx.wFormatTag = WAVE_FORMAT_PCM;
            wfx.nChannels = 1;
            wfx.nSamplesPerSec = 11025;
            wfx.wBitsPerSample = 8;
            wfx.nBlockAlign = 1;
            wfx.nAvgBytesPerSec = 11025;
         }

         DWORD dwDataSize = (DWORD) m_qwTotal;
         DWORD dwRiffSize = 36 + dwDataSize;
         DWORD dw16 = 16;

         fseek(m_pFile, 0, SEEK_SET);
         fwrite("RIFF", 1, 4, m_pFile);
         fwrite(&dwRiffSize, 4, 1, m_pFile);
         fwrite("WAVE", 1, 4, m_pFile);
         fwrite("fmt ", 1, 4, m_pFile);
         fwrite(&dw16, 4, 1, m_pFile);
         fwrite(&wfx.wFormatTag, 2, 1, m_pFile);
         fwrite(&wfx.nChannels, 2, 1, m_pFile);
         fwrite(&wfx.nSamplesPerSec, 4, 1, m_pFile);
         fwrite(&wfx.nAvgBytesPerSec, 4, 1, m_pFile);
         fwrite(&wfx.nBlockAlign, 2, 1, m_pFile);
         fwrite(&wfx.wBitsPerSample, 2, 1, m_pFile);
         fwrite("data", 1, 4, m_pFile);
         fwrite(&dwDataSize, 4, 1, m_pFile);

         fclose(m_pFile);
         m_pFile = NULL;
      }

      STDMETHODIMP QueryInterface(REFIID riid, LPVOID FAR *ppv)
      {
         if (IsEqualIID(riid, IID_IUnknown))
            *ppv = (LPVOID) (IUnknown*) this;
         else if (IsEqualIID(riid, IID_IAudio))
            *ppv = (LPVOID) (IAudio*) &m_iAudio;
         else if (IsEqualIID(riid, IID_IAudioDest))
            *ppv = (LPVOID) (IAudioDest*) &m_iAudioDest;
         else if (IsEqualIID(riid, IID_IAudioMultiMediaDevice))
            *ppv = (LPVOID) (IAudioMultiMediaDevice*) &m_iAudioMMDevice;
         else {
            *ppv = NULL;
            return ResultFromScode(E_NOINTERFACE);
         }
         ((LPUNKNOWN) *ppv)->AddRef();
         return NOERROR;
      }
      STDMETHODIMP_(ULONG) AddRef(void) { return ++m_cRef; }
      STDMETHODIMP_(ULONG) Release(void)
      {
         if (--m_cRef == 0) { delete this; return 0; }
         return m_cRef;
      }
   };

STDMETHODIMP CWavIAudio::QueryInterface(REFIID riid, LPVOID FAR *ppv) { return m_pParent->QueryInterface(riid, ppv); }
STDMETHODIMP_(ULONG) CWavIAudio::AddRef(void) { return m_pParent->AddRef(); }
STDMETHODIMP_(ULONG) CWavIAudio::Release(void) { return m_pParent->Release(); }

STDMETHODIMP CWavIAudio::PosnGet(PQWORD pq)
{
   if (!pq) return ResultFromScode(E_INVALIDARG);
   *pq = m_pParent->m_qwTotal;
   return NOERROR;
}
STDMETHODIMP CWavIAudio::TotalGet(PQWORD pq)
{
   if (!pq) return ResultFromScode(E_INVALIDARG);
   *pq = m_pParent->m_qwTotal;
   return NOERROR;
}
STDMETHODIMP CWavIAudio::ToFileTime(PQWORD pq, FILETIME *pft)
{
   if (!pq || !pft) return ResultFromScode(E_INVALIDARG);
   DWORD dwABPS = 11025;
   if (m_pParent->m_dwFmtSize >= 16) {
      WAVEFORMATEX *pwfx = (WAVEFORMATEX*) m_pParent->m_abFmt;
      if (pwfx->nAvgBytesPerSec) dwABPS = pwfx->nAvgBytesPerSec;
   }
   *((QWORD*) pft) = (*pq * (QWORD) 10000000) / dwABPS;
   return NOERROR;
}
STDMETHODIMP CWavIAudio::WaveFormatGet(PSDATA pd)
{
   if (!pd) return ResultFromScode(E_INVALIDARG);
   if (!m_pParent->m_dwFmtSize) return ResultFromScode(AUDERR_NEEDWAVEFORMAT);
   LPMALLOC pMalloc;
   if (FAILED(CoGetMalloc(MEMCTX_TASK, &pMalloc))) return ResultFromScode(E_OUTOFMEMORY);
   pd->pData = pMalloc->Alloc(m_pParent->m_dwFmtSize);
   pMalloc->Release();
   if (!pd->pData) return ResultFromScode(E_OUTOFMEMORY);
   memcpy(pd->pData, m_pParent->m_abFmt, m_pParent->m_dwFmtSize);
   pd->dwSize = m_pParent->m_dwFmtSize;
   return NOERROR;
}
STDMETHODIMP CWavIAudio::WaveFormatSet(SDATA d)
{
   if (!d.pData) return ResultFromScode(E_INVALIDARG);
   DWORD dwCopy = (d.dwSize < sizeof(m_pParent->m_abFmt)) ? d.dwSize : sizeof(m_pParent->m_abFmt);
   memcpy(m_pParent->m_abFmt, d.pData, dwCopy);
   m_pParent->m_dwFmtSize = dwCopy;
   return NOERROR;
}

STDMETHODIMP CWavIAudioDest::QueryInterface(REFIID riid, LPVOID FAR *ppv) { return m_pParent->QueryInterface(riid, ppv); }
STDMETHODIMP_(ULONG) CWavIAudioDest::AddRef(void) { return m_pParent->AddRef(); }
STDMETHODIMP_(ULONG) CWavIAudioDest::Release(void) { return m_pParent->Release(); }
STDMETHODIMP CWavIAudioDest::DataSet(PVOID pBuf, DWORD dwSize)
{
   if (!pBuf) return ResultFromScode(E_INVALIDARG);
   if (!m_pParent->m_pFile) return ResultFromScode(E_UNEXPECTED);
   fwrite(pBuf, 1, dwSize, m_pParent->m_pFile);
   m_pParent->m_qwTotal += dwSize;
   return NOERROR;
}

STDMETHODIMP CWavIAudioMultiMediaDevice::QueryInterface(REFIID riid, LPVOID FAR *ppv) { return m_pParent->QueryInterface(riid, ppv); }
STDMETHODIMP_(ULONG) CWavIAudioMultiMediaDevice::AddRef(void) { return m_pParent->AddRef(); }
STDMETHODIMP_(ULONG) CWavIAudioMultiMediaDevice::Release(void) { return m_pParent->Release(); }


typedef HRESULT (STDAPICALLTYPE *PFNDLLGETCLASSOBJECT)(REFCLSID, REFIID, LPVOID*);


int PrintHR(const char *pszWhat, HRESULT hr)
{
   fprintf(stderr, "%s failed: hr=0x%08X\n", pszWhat, (unsigned) hr);
   return 1;
}

static const IVXVOICE *FindVoice(const char *pszName)
{
   size_t i;
   for (i = 0; i < NUM_IVX_VOICES; i++) {
      if (!_stricmp(pszName, g_IvxVoices[i].pszModeKey)) return &g_IvxVoices[i];
   }
   return NULL;
}

static void ListVoices(void)
{
   size_t i;
   for (i = 0; i < NUM_IVX_VOICES; i++) {
      printf("%-28s (%s)\n", g_IvxVoices[i].pszModeKey, g_IvxVoices[i].pszLanguageName);
   }
}

/* Seeds every directory the engine looks in (it uses different names in
   different code paths, so all of them point at the folder holding the
   rule files) plus the full built-in voice table, exactly as the
   engine's own installer would have written it - see the infovox
   project's ivx_catalog.cpp, which this is ported from. */
static void SeedIvxRegistry(const char *pszEngineDir)
{
   static const char * const s_aszDirValues[] =
      { "LanguageDir", "LanguageDirectory", "LexiconDir", "LicenseDir", "LogStartupDir", "Path" };
   DWORD i;
   size_t j;

   for (i = 0; i < sizeof(s_aszDirValues) / sizeof(s_aszDirValues[0]); i++) {
      VReg_SetString(IVX_ENGINE_ROOT, s_aszDirValues[i], pszEngineDir);
   }

   for (j = 0; j < NUM_IVX_VOICES; j++) {
      const IVXVOICE *pV = &g_IvxVoices[j];
      char szPath[256];
      wsprintfA(szPath, "%s\\%s", IVX_MODES_ROOT, pV->pszModeKey);

      /* A value that is not set and a value set to "" are not the same
         thing to this engine (see the infovox project's write-up), so
         the data-file names are written only when there is a name. */
      VReg_SetString(szPath, "ModeGUID", pV->pszModeGuid);
      VReg_SetString(szPath, "LanguageID", pV->pszLanguageId);
      VReg_SetString(szPath, "LanguageFile", pV->pszLanguageFile);
      if (pV->pszLibraryFile[0]) VReg_SetString(szPath, "LibraryFile", pV->pszLibraryFile);
      if (pV->pszPhSymFile[0])   VReg_SetString(szPath, "PhSymFile", pV->pszPhSymFile);
      VReg_SetString(szPath, "SpeakerName", pV->pszSpeakerName);
      if (pV->pszSpeakerStyle[0]) VReg_SetString(szPath, "SpeakerStyle", pV->pszSpeakerStyle);
      VReg_SetString(szPath, "Gender", pV->pszGender);
      VReg_SetString(szPath, "Age", pV->pszAge);
      VReg_SetString(szPath, "Pitch", pV->pszPitch);
      VReg_SetString(szPath, "Dynamic", pV->pszDynamic);
      VReg_SetString(szPath, "Aspiration", pV->pszAspiration);
      VReg_SetString(szPath, "FormantNo", pV->pszFormantNo);
   }
}


int main(int argc, char *argv[])
{
   setvbuf(stdout, NULL, _IONBF, 0);
   int iArg = 1;
   const char *pszVoiceName = "American English Male";
   const char *pszWavPath = NULL;      // -o <file.wav>: write to file instead of playing
   char szExeDir[MAX_PATH];
   char szEngineDir[MAX_PATH];
   char szDllPath[MAX_PATH];

   /* -v and -o may appear in any order, before the text */
   while (iArg < argc) {
      if (!strcmp(argv[iArg], "-v") && iArg + 1 < argc) {
         pszVoiceName = argv[iArg + 1];
         iArg += 2;
      } else if (!strcmp(argv[iArg], "-o") && iArg + 1 < argc) {
         pszWavPath = argv[iArg + 1];
         iArg += 2;
      } else if (!strcmp(argv[iArg], "-list")) {
         ListVoices();
         return 0;
      } else {
         break;
      }
   }

   if (iArg >= argc) {
      fprintf(stderr, "usage: speak.exe [-v \"Voice Name\"] [-o out.wav] \"text to speak\"\n");
      fprintf(stderr, "       speak.exe -list\n");
      fprintf(stderr, "  -v selects one of the 60 Infovox 230 voices (default: American English Male).\n");
      fprintf(stderr, "  -o writes the audio to a WAV file instead of playing it live.\n");
      fprintf(stderr, "  -list prints every voice name and exits.\n");
      return 1;
   }

   const IVXVOICE *pVoice = FindVoice(pszVoiceName);
   if (!pVoice) {
      fprintf(stderr, "Unknown voice '%s'; run 'speak.exe -list' to see the choices.\n", pszVoiceName);
      return 1;
   }

   /* The engine dll and its rule files live right next to speak.exe. */
   GetModuleFileNameA(NULL, szExeDir, MAX_PATH);
   {
      char *pSlash = strrchr(szExeDir, '\\');
      if (pSlash) *(pSlash + 1) = '\0'; else szExeDir[0] = '\0';
   }
   lstrcpynA(szEngineDir, szExeDir, MAX_PATH);
   {
      size_t cch = strlen(szEngineDir);
      if (cch > 0 && szEngineDir[cch - 1] == '\\') szEngineDir[cch - 1] = '\0';
   }
   wsprintfA(szDllPath, "%s\\Ivx230nt.dll", szEngineDir);

   CLSID gModeID;
   {
      WCHAR wszGuid[64];
      MultiByteToWideChar(CP_ACP, 0, pVoice->pszModeGuid, -1, wszGuid, 64);
      if (FAILED(CLSIDFromString(wszGuid, &gModeID))) {
         fprintf(stderr, "Bad mode GUID string for voice '%s'\n", pVoice->pszModeKey);
         return 1;
      }
   }

   /* Build the wide-character text to speak */
   WCHAR wszText[16000];
   {
      /* Join remaining argv[] with spaces (mirrors the "speak.exe hello world" sample) */
      char szJoined[8000];
      int i;
      szJoined[0] = '\0';
      for (i = iArg; i < argc; i++) {
         if (i > iArg) strcat(szJoined, " ");
         strcat(szJoined, argv[i]);
      }
      MultiByteToWideChar(CP_ACP, 0, szJoined, -1, wszText, 16000);
   }

   printf("Engine : Infovox 230 v1.12\n");
   printf("Voice  : %s\n", pVoice->pszDisplayName);
   printf("DLL    : %s\n", szDllPath);
   printf("Text   : %S\n", wszText);
   if (pszWavPath)
      printf("Output : %s (file, no live playback)\n", pszWavPath);

   if (FAILED(CoInitialize(NULL))) {
      fprintf(stderr, "CoInitialize failed\n");
      return 1;
   }
   ghInstance = GetModuleHandle(NULL);

   /* ---- Step 1: seed the engine's configuration into memory before it
      is ever asked for anything. The data has to exist before
      VReg_Install() patches the loaded module's imports, but does not
      need the module handle itself, so this can happen first. ---- */
   VReg_Reset();
   SeedIvxRegistry(szEngineDir);

   /* ---- Step 2: load the engine DLL directly (no registry, no COM
      registration). LOAD_WITH_ALTERED_SEARCH_PATH makes the engine's own
      folder the first place its dependency sx32w.dll is looked for. ---- */
   HMODULE hEngine = LoadLibraryExA(szDllPath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
   if (!hEngine) {
      fprintf(stderr, "LoadLibrary('%s') failed, err=%lu\n", szDllPath, GetLastError());
      CoUninitialize();
      return 1;
   }

   /* ---- Step 3: redirect the engine's ten registry imports at the
      table just seeded - before anything asks it for a class object,
      which is the first point it reads its configuration. ---- */
   if (!VReg_Install(hEngine)) {
      fprintf(stderr, "Could not redirect all of the engine's registry imports; without a real "
                      "HKLM\\%s this engine will enumerate no voices.\n", IVX_ENGINE_ROOT);
      FreeLibrary(hEngine);
      CoUninitialize();
      return 1;
   }

   PFNDLLGETCLASSOBJECT pDllGetClassObject =
      (PFNDLLGETCLASSOBJECT) GetProcAddress(hEngine, "DllGetClassObject");
   if (!pDllGetClassObject) {
      fprintf(stderr, "GetProcAddress(DllGetClassObject) failed\n");
      FreeLibrary(hEngine);
      CoUninitialize();
      return 1;
   }

   /* ---- Step 4: get the class factory directly from the DLL ---- */
   IClassFactory *pFactory = NULL;
   HRESULT hr = pDllGetClassObject(CLSID_InfovoxEngine, IID_IClassFactory, (LPVOID*) &pFactory);
   if (FAILED(hr) || !pFactory) {
      PrintHR("DllGetClassObject", hr);
      FreeLibrary(hEngine);
      CoUninitialize();
      return 1;
   }
   printf("DllGetClassObject: OK, got IClassFactory\n");

   /* ---- Step 5: build our own audio destination object - either the
      waveOut-based CAOut (live playback), or the file-based CWavOut when
      -o was given. Either way Select() just sees an IUnknown and can't
      tell which. */
   PCAOut  pAOut  = NULL;
   CWavOut *pWavOut = NULL;
   LPUNKNOWN pAudioUnknown = NULL;

   if (pszWavPath) {
      pWavOut = new CWavOut();
      if (!pWavOut || !pWavOut->Open(pszWavPath)) {
         fprintf(stderr, "Could not open '%s' for writing\n", pszWavPath);
         pFactory->Release();
         FreeLibrary(hEngine);
         CoUninitialize();
         return 1;
      }
      pWavOut->AddRef();
      pAudioUnknown = (LPUNKNOWN) pWavOut;
   } else {
      pAOut = new CAOut(NULL, NULL);
      if (!pAOut || !pAOut->FInit(WAVE_MAPPER, NULL, 0)) {
         fprintf(stderr, "CAOut::FInit failed\n");
         pFactory->Release();
         FreeLibrary(hEngine);
         CoUninitialize();
         return 1;
      }
      pAOut->AddRef();
      pAudioUnknown = (LPUNKNOWN) pAOut;
   }

   /* ---- Step 6: get the mode enumerator - confirmed by the infovox
      project's own reverse-engineering of this identical engine: its
      CreateInstance() never builds the real engine directly, only an
      ITTSEnumW. ---- */
   PITTSENUMW pEnum = NULL;
   hr = pFactory->CreateInstance(NULL, IID_ITTSEnumW, (LPVOID*) &pEnum);
   if (FAILED(hr) || !pEnum) {
      PrintHR("CreateInstance(NULL, IID_ITTSEnumW)", hr);
      pFactory->Release();
      FreeLibrary(hEngine);
      CoUninitialize();
      return 1;
   }
   printf("CreateInstance(IID_ITTSEnumW): OK, got the enumerator\n");

   pFactory->Release();
   pFactory = NULL;

   /* ---- Step 7: the real audio hookup, via Select(). The chosen mode's
      GUID is already known from the seeded table - unlike the
      single-mode-per-DLL engines this driver used to target, one
      Infovox dll enumerates all 60 modes, so there's no need to walk
      Next() first. ---- */
   PITTSCENTRALW pCentral = NULL;
   hr = pEnum->Select(gModeID, &pCentral, pAudioUnknown);
   if (FAILED(hr) || !pCentral) {
      PrintHR("ITTSEnumW::Select", hr);
      pEnum->Release();
      FreeLibrary(hEngine);
      CoUninitialize();
      return 1;
   }
   printf("Select: OK, got ITTSCentralW with audio wired up\n");

   pEnum->Release();

   /* ---- Step 8: register for AudioStart/AudioStop - see the
      top-of-file note on why TextDataDone alone is not enough for this
      engine. Not fatal if it fails; the timeout below still applies. */
   CNotify *pCentralNotify = new CNotify();
   DWORD dwNotifyKey = 0;
   HRESULT hrReg = pCentral->Register((PVOID) (ITTSNotifySinkW*) pCentralNotify,
                                       IID_ITTSNotifySinkW, &dwNotifyKey);
   if (FAILED(hrReg)) {
      fprintf(stderr, "(ITTSCentralW::Register failed, hr=0x%08X - completion timing may be off)\n",
              (unsigned) hrReg);
      dwNotifyKey = 0;
   }

   /* ---- Step 9: speak the text ---- */
   CBufNotify *pNotify = new CBufNotify();

   SDATA d;
   d.pData = (PVOID) wszText;
   d.dwSize = (DWORD) ((wcslen(wszText) + 1) * sizeof(WCHAR));

   hr = pCentral->TextData(CHARSET_TEXT, 0, d, (PVOID) pNotify, IID_ITTSBufNotifySinkW);
   if (FAILED(hr)) {
      PrintHR("ITTSCentralW::TextData", hr);
   } else {
      printf("TextData: OK, speaking...\n");

      /* Pump messages (CAOut's hidden window needs MM_WOM_* / WM_TIMER)
         until we're done, or we time out. Finished means: the engine has
         let go of the text (TextDataDone) and, if it ever started
         playing, its audio has stopped (ITTSNotifySinkW::AudioStop) -
         both are needed, because TextDataDone arrives within a
         millisecond of handing the text over, well before the engine has
         actually said it. A short grace period covers a legitimately
         silent utterance (bare punctuation, say), which never raises
         AudioStart at all. */
      DWORD dwStart = GetTickCount();
      DWORD dwQuietDeadline = 0;
      for (;;) {
         MSG msg;
         if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
         } else {
            Sleep(10);
         }

         if (pNotify->m_fDone) {
            if (pCentralNotify->m_fStopped) break;
            if (!pCentralNotify->m_fStarted) {
               if (!dwQuietDeadline) dwQuietDeadline = GetTickCount() + 800;
               if (GetTickCount() > dwQuietDeadline) break;
            }
         }
         if (GetTickCount() - dwStart > 60000) {
            fprintf(stderr, "(timed out waiting for TextDataDone/AudioStop)\n");
            break;
         }
      }

      /* TextDataDone/AudioStop only means the engine finished handing us
         audio data - CAOut may still have several buffers queued/playing
         on the wave device. Wait for actual playback position to catch
         up to the total bytes written before tearing anything down,
         instead of guessing with a fixed Sleep(). */
      IAudio *pAudioIface = NULL;
      pAudioUnknown->QueryInterface(IID_IAudio, (LPVOID*) &pAudioIface);
      if (pAudioIface) {
         DWORD dwDrainStart = GetTickCount();
         for (;;) {
            MSG msg;
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
               TranslateMessage(&msg);
               DispatchMessage(&msg);
            }
            QWORD qwTotal = 0, qwPosn = 0;
            pAudioIface->TotalGet(&qwTotal);
            pAudioIface->PosnGet(&qwPosn);
            if (qwPosn >= qwTotal) break;
            if (GetTickCount() - dwDrainStart > 30000) {
               fprintf(stderr, "(timed out waiting for audio to drain: posn=%I64u total=%I64u)\n",
                  qwPosn, qwTotal);
               break;
            }
            Sleep(20);
         }
         pAudioIface->Release();
      } else {
         Sleep(300);
      }
   }

   if (dwNotifyKey) pCentral->UnRegister(dwNotifyKey);
   pNotify->Release();
   pCentralNotify->Release();
   pCentral->Release();

   if (pWavOut) {
      /* Finalize the WAV header now, independent of COM refcounting -
         see the note below on why we don't call Release() here. */
      pWavOut->Close();
      printf("Wrote %I64u bytes of audio to %s\n", pWavOut->m_qwTotal, pszWavPath);
   }

   /* Deliberately not releasing pAOut/pWavOut: some engines (observed
      with the German TruVoice build this driver used to target) over-
      release the shared audio object's refcount internally, which would
      make a final Release() here a use-after-free. The process is
      exiting right after anyway, so the OS reclaims the wave device/file
      handle regardless - not worth chasing a third-party refcounting bug
      for a value that's about to be torn down. */
   FreeLibrary(hEngine);
   CoUninitialize();

   printf("Done.\n");
   return 0;
}
