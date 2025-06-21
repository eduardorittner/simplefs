# Metrics-fs

Simple in-memory filesystem with global, per-dir and per-file stats.

## Running

1. Compile the module along with a compatible (v6.8) kernel
2. `insmod mfs.ko`
3. `mkdir /mnt/mfs`
4. `mount -t mfs none /mnt/mfs`

And you're good to go!

