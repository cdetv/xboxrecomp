# Partition images hold only what is written

Asset-free Windows regression for the partition device images
(`<save>\Partition1-5.img`, `kernel_path.c`, `kernel_file.c`).

The images used to be set to the full partition size (4.9 GB in all) as sparse
files. Copying the save folder wrote every zero out, and drives without sparse
files (exFAT, FAT32) held them in full. Now an image is only as long as what a
title wrote to it; the size a title is told comes from the partition table,
and a read past the image's end returns zeros up to the partition's end.

The test checks:

- new images start empty;
- an old full-size sparse image is cut after its data, which it keeps;
- an old full-size fully allocated image (a copied folder) is emptied;
- an image of any other length is left alone, also on a second start;
- reads inside the partition past the image are zeros, stop at the
  partition's end, then end of file; a sequential read moves the position on;
- a write lands where it is aimed; FileStandardInformation reports the
  partition's size; a closed handle is forgotten;
- the free-space open of a partition as a directory still works.

```powershell
cmake -S tests/partition_images -B _work/toolkit-tests/partition_images -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build _work/toolkit-tests/partition_images
ctest --test-dir _work/toolkit-tests/partition_images --output-on-failure
```
