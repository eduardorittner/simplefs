# Metrics-fs (mfs)

Simple in-memory filesystem with global stats.

## Running

1. Compile the module along with a compatible (v6.8) kernel
2. `insmod mfs.ko`
3. `mkdir /mnt/mfs`
4. `mount -t mfs none /mnt/mfs`

And you're good to go!

## Structure

mfs follows from this [filesystem tutorial](https://linux-kernel-labs.github.io/refs/heads/master/labs/filesystems_part1.html), which is itself inspired by ramfs, a simple in-memory filesystem in the linux kernel used mostly for educational purposes. It leverages the kernel's internal functions for most of the file and directory operations.

Mfs collects very basic global metrics for the current mounted instance, such as:
- Total bytes written
- Total bytes read
- Total write operations
- Total read operations
- Total files created/deleted
- Total directories created/deleted

These metrics are available through a special virtual "/metrics" file, which has custom write (no-op) and read operations, and is immutable so cannot be renamed, deleted or moved.
