# Infovox 230 CLI

A minimal Windows command-line text-to-speech tool. `speak.exe` drives the
**Infovox 230 v1.12** speech engine (`Ivx230nt.dll`) directly, with no SAPI
installation, no COM registration, and no changes to the Windows registry.

## How it works

- `Ivx230nt.dll` is loaded with `LoadLibrary()` and driven straight through
  its own `DllGetClassObject`/SAPI4 interfaces - never registered, never
  reached via `CoCreateInstance`.
- The engine reads its entire voice table from the registry on start-up, so
  before loading it, `vreg.cpp` patches the ten registry functions it imports
  to serve an in-memory copy of that table instead (see `ivx_voices.h`).
  Nothing is read from or written to the real registry.
- Audio either plays live through `waveOut` (`audio.h`/`audioout.cpp`, from
  Microsoft's old AudioSD SDK sample) or is captured straight to a WAV file.

## Build

Requires **Visual C++ 6 (VC98)** installed at the default path, to match the
vintage SAPI 4.0a ABI these headers target.

```bash
build.bat
```

This produces `speak.exe` in this folder. The engine DLL and its rule files
already live here too, so the folder is self-contained - copy the whole
thing anywhere and it still works.

## Usage

```
speak.exe [-v "Voice Name"] [-o out.wav] "text to speak"
speak.exe -list
```

- `-v` selects one of the 60 built-in voices (default: `American English Male`).
- `-o` writes the audio to a WAV file instead of playing it live.
- `-list` prints every voice name and exits.

Examples:

```bash
speak.exe "hello world"
speak.exe -v "British English Female" "good afternoon"
speak.exe -v "German Male" -o greeting.wav "guten tag"
```

## Credit

TruVoice multilingual driver by Anthony C. Bartman (@rommix0).

Adapted to drive Infovox 230 using engine research from the [infovox23012-sapi5](https://github.com/joshknnd1982/infovox23012-sapi5) project.
