# Guest text

A title's 8-bit text through the kernel's string calls, converted the Xbox's
way: one byte is one Latin-1 character (0xE9 is U+00E9), and back one byte a
character, a character past U+00FF becoming `?` (`src/kernel/guest_text.h`).

```
cmake -S tests/guest_text -B build/guest-text -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/guest-text
ctest --test-dir build/guest-text --output-on-failure
```

On Windows the test runs with a UTF-8 process code page (`utf8.manifest`), as
a game does that takes folder names with any letters. The first check fails if
the manifest didn't take effect, so a green run means the conversions really
ignore the PC's code page.

## What it guarantees

- The helpers: bytes to UTF-16 and back one character a byte, `?` for a
  character with no byte, size queries, and failure on too small a buffer.
- Through the kernel bridge (as `tests/kernel_bridge`): ordinals 260
  `RtlAnsiStringToUnicodeString`, 308 `RtlUnicodeStringToAnsiString` (its
  one-byte-a-character buffer must hold `é`), 299, 300, 310, 311 and 315
  (`RtlUpcaseUnicodeToMultiByteN`: `é` upcases to 0xC9).
- The host-side `xbox_RtlAnsiStringToUnicodeString` and
  `xbox_RtlUnicodeStringToAnsiString` (`kernel_rtl.c`).
