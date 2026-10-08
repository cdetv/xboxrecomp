# Path rules: title and user data stay out of the game dir

Asset-free Windows regression for `xbox_translate_path`. XAPI mounts T: and U:
by linking them to `\Device\Harddisk0\Partition1\TDATA\<title id>` and
`...\UDATA\<title id>`. Those used to fall to the general Partition1 rule and
land in the game dir -- for a recompiled title, the extracted disc. The test
checks that both (directly, as bare directories, and through a T: link) go to
the save dir, and that the rest of Partition1, `TDATAX`-style near misses and
D: still go to the game dir.

`Partition1\CACHE` is the same kind of leak: XMountUtilityDrive keeps its
`LocalCacheNN.bin` slot files there and creates the directory if it is
missing. The test checks it goes to `<save>\HddCache`, apart from `Cache`
(the Z: contents).

```powershell
cmake -S tests/kernel_path_rules -B build/path-rules -A x64
cmake --build build/path-rules --config Release
ctest --test-dir build/path-rules -C Release --output-on-failure
```
