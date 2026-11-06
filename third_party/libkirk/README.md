# libkirk

The PSP's KIRK crypto engine (AES, SHA-1, the KIRK commands and keys), used by
host/chnnlsv.c to read save data that a PSP or PPSSPP encrypted.

Copied unchanged from PPSSPP's ext/libkirk, commit
b69424e8f20e28ca48557011c5b992a265a115ae (https://github.com/hrydgard/ppsspp).

- kirk_engine.c/.h: Draan and the PSP scene, GNU GPL v3 or later
- bn.c, ec.c: Segher Boessenkool, GNU GPL v2 or v3
- AES.c/.h: Rijndael reference code, public domain
- SHA1.c/.h: David Ireland, adapted from A.M. Kuchling's Python Cryptography Toolkit; no licence statement
  in the file

amctrl.c (PPSSPP's, by tpu, with no licence) is not used and not copied.
