# Attribute query: missing leaf vs missing parent

Asset-free Windows regression for `xbox_NtQueryFullAttributesFile` (ordinal
210, what XAPI's `GetFileAttributes` calls). A missing file in an existing
directory must return `STATUS_OBJECT_NAME_NOT_FOUND` (Win32 error 2); a path
whose parent directory is missing must return `STATUS_OBJECT_PATH_NOT_FOUND`
(error 3). The Win32 backend used to fold both into NAME_NOT_FOUND.

That matters to titles that build directory trees with a recursive helper:
probe the directory's attributes, on error 3 create the parent first, on error
2 create just the directory. With both folded to error 2 the helper skips the
parents, the leaf create fails, and the title treats its cache as broken
(Conker: Live & Reloaded copying textures into an empty `Z:` cache).

```powershell
cmake -S tests/kernel_query_attributes -B build/query-attrs -A x64
cmake --build build/query-attrs --config Release
ctest --test-dir build/query-attrs -C Release --output-on-failure
```
