/*
 * Metrics-fs
 * A simple in-memory filesystem inspired by ramfs which supports basic file
 * operations and exposes metrics on /metrics
 */

#include <linux/backing-dev.h>
#include <linux/fs.h> // Core kernel filesystem data structures and APIs (VFS)
#include <linux/fs_context.h> // The modern filesystem mounting API framework
#include <linux/fs_parser.h> // API for parsing filesystem mount options
#include <linux/highmem.h> // For managing memory in high memory zones
#include <linux/init.h> // For __init and fs_initcall macros
#include <linux/magic.h> // Contains magic numbers for various filesystems, e.g., MFS_MAGIC
#include <linux/mm.h> // Core memory management structures and functions
#include <linux/mman.h> // Memory management definitions, e.g., for mmap
#include <linux/module.h> // For building this code as a loadable kernel module (LKM)
#include <linux/pagemap.h> // Page cache management APIs
#include <linux/pagevec.h>
#include <linux/parser.h> // Parameter parsing API for mount options
#include <linux/ramfs.h>
#include <linux/sched.h>
#include <linux/seq_file.h> // For creating virtual files in /proc (used for show_options)
#include <linux/slab.h> // Kernel memory allocator for objects (e.g., kzalloc)
#include <linux/string.h>
#include <linux/time.h> // Time-related kernel functions
#include <linux/uaccess.h> // For safely copying data between kernel and user space

/*
 * Defines operations (and corresponding functions) that can be performed on a
 * regular file. The functions are called by the VFS when the user invokes the
 * corresponding operations.
 */
const struct file_operations mfs_file_ops = {
    /*
     * generic_file_read_iter is a kernel-provided helper function that
     * implements file reading. It works by reading data from the page cache. If
     * the data is not in the cache, it will be brought in by the address_space
     * operations.
     */
    .read_iter = generic_file_read_iter,
    /*
     * generic_file_write_iter is the corresponding helper for writing. It
     * writes data from user space into the page cache. The data is marked as
     * dirty and will be written to the backing store later (though for ramfs,
     * there is no physical backing store).
     */
    .write_iter = generic_file_write_iter,
    /*
     * Fsync is an operation for making sure that the data is persisted to disk.
     * Since this is an in-memory filesystem it doesn't make sense to fsync, so
     * we define it as a no-op.
     */
    .fsync = noop_fsync,
    /*
     * generic_file_llseek is a standard kernel helper function that implements
     * the llseek system call, allowing users to change the current read/write
     * offset in a file.
     */
    .llseek = generic_file_llseek,
};

/*
 * This structure defines the operations that can be performed on a regular
 * file's inode. An inode is the kernel's internal representation of a file.
 * These operations handle metadata changes like setting attributes
 * (permissions, etc.).
 */
const struct inode_operations mfs_file_inode_ops = {
    /*
     * simple_setattr is a generic helper for updating inode attributes (like
     * size, permissions, timestamps) from user-space calls like chmod(2) or
     * truncate(2).
     */
    .setattr = simple_setattr,
    /*
     * simple_getattr is a generic helper to get inode attributes. The VFS uses
     * this to populate the `stat` structure for system calls like stat(2).
     */
    .getattr = simple_getattr,
};

/*
 * =============================================================================
 * START - Core inode and filesystem logic
 * =============================================================================
 */

// A structure to hold mount-time options, in this case, the root directory
// mode.
struct ramfs_mount_opts {
    umode_t mode;
};

// A structure to hold filesystem-specific information, attached to the
// superblock.
struct ramfs_fs_info {
    struct ramfs_mount_opts mount_opts;
};

#define RAMFS_DEFAULT_MODE 0755

// Forward declarations for operations structures used below.
static const struct super_operations mfs_ops;
static const struct inode_operations mfs_dir_inode_ops;

/*
 * NOTE: The original ramfs code uses `ram_aops` which is defined in mm/shmem.c.
 * You will need to provide an implementation for these address_space_operations
 * or link against the existing one for this code to compile and link.
 * For example:
 * extern const struct address_space_operations ram_aops;
 */
extern const struct address_space_operations ram_aops;

/**
 * mfs_get_inode - Creates a new inode for a file or directory.
 * @sb: The superblock for this filesystem instance.
 * @dir: The parent directory's inode.
 * @mode: The file type (S_IFREG, S_IFDIR) and permissions.
 * @dev: The device number (for special files).
 *
 * This function is the core of object creation. It allocates an inode,
 * initializes its metadata, and assigns it the correct operations based on its
 * type.
 */
struct inode* mfs_get_inode(
    struct super_block* sb, const struct inode* dir, umode_t mode, dev_t dev)
{
    /*
     * Asks the kernel for a new inode object associated with our superblock.
     */
    struct inode* inode = new_inode(sb);

    if (inode) {
        /*
         * get_next_ino() allocates a new, unique inode number for this filesystem.
         */
        inode->i_ino = get_next_ino();
        /*
         * inode_init_owner() initializes the inode's user ID (uid), group ID (gid),
         * and permission bits (`mode`) based on the parent directory and the
         * current process's credentials.
         */
        inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
        /*
         * This is critical for an in-memory filesystem. It connects the inode's
         * address_space (which manages its data in the page cache) to the
         * address_space_operations for ramfs (`ram_aops`). These operations
         * handle how pages are read and written.
         */
        inode->i_mapping->a_ops = &ram_aops;
        /*
         * mapping_set_gfp_mask() sets the memory allocation flags for this inode's
         * pages. GFP_HIGHUSER allows allocating from high memory, suitable for
         * user data pages.
         */
        mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
        /*
         * This prevents the kernel from ever swapping out the pages belonging to
         * this inode. This is what makes it a true "RAM" filesystem.
         */
        mapping_set_unevictable(inode->i_mapping);
        /*
         * simple_inode_init_ts() is a helper that sets the inode's timestamps
         * (access, modification, and change times) to the current time.
         */
        simple_inode_init_ts(inode);
        /*
         * Set the corresponding operations for the file type.
         */
        switch (mode & S_IFMT) {
            // Regular file
        case S_IFREG:
            inode->i_op = &mfs_file_inode_ops;
            inode->i_fop = &mfs_file_ops;
            break;
            // Directory
        case S_IFDIR:
            inode->i_op = &mfs_dir_inode_ops;
            inode->i_fop = &simple_dir_operations; // Generic kernel operations for directories
            /*
             * inc_nlink() increments the hard link count of the inode. A new
             * directory starts with a link count of 2 (for its own entry "." and the
             * ".." entry in the new directory). The parent's link count will also be
             * incremented.
             */
            inc_nlink(inode);
            break;
        // These can be symlinks or other file types like devices, etc
        default:
            pr_info("File type not supported");
            /*
             * iput() decrements the inode's usage counter. If it drops to zero, the
             * kernel will free the inode. This is for cleanup on error.
             */
            iput(inode);
            break;
        }
    }
    return inode;
}

/**
 * mfs_mknod - Creates a filesystem node (file, directory, etc.).
 * @idmap: The ID mapping for the mount.
 * @dir: Inode of the parent directory.
 * @dentry: The directory entry (dentry) for the new file, which holds the name.
 * @mode: The file's mode and permissions.
 * @dev: The device number (if a device node).
 *
 * This is the generic node creation function called by create, mkdir, etc.
 */
static int mfs_mknod(
    struct mnt_idmap* idmap, struct inode* dir, struct dentry* dentry, umode_t mode, dev_t dev)
{
    struct inode* inode = mfs_get_inode(dir->i_sb, dir, mode, dev);
    int error = -ENOSPC; // Assume error "No space on device" by default

    if (inode) {
        /*
         * d_instantiate() connects the dentry (the name) with the inode (the data).
         * After this call, the file becomes visible in the filesystem.
         */
        d_instantiate(dentry, inode);
        /*
         * dget() increments the reference count of the dentry, preventing it from
         * being freed while in use.
         */
        dget(dentry);
        error = 0; // Success
        /*
         * inode_set_ctime_current() updates the parent directory's change time.
         * inode_set_mtime_to_ts() updates the parent directory's modification time.
         * This is required because we have modified the directory by adding a new
         * entry.
         */
        inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
    }
    return error;
}

// Wrapper around mfs_mknod for creating a directory.
static int mfs_mkdir(
    struct mnt_idmap* idmap, struct inode* dir, struct dentry* dentry, umode_t mode)
{
    /*
     * pr_info() prints a message to the kernel log buffer (viewable with dmesg).
     * '%pd' is a format specifier to print a dentry's path.
     */
    pr_info("ramfs: creating directory '%pd'\n", dentry);

    int retval = mfs_mknod(&nop_mnt_idmap, dir, dentry, mode | S_IFDIR, 0);
    if (!retval)
        /*
         * If mknod was successful, we increment the parent directory's link count
         * because the new subdirectory's ".." entry points back to it.
         */
        inc_nlink(dir);
    return retval;
}

// Wrapper around mfs_mknod for creating a regular file.
static int mfs_create(
    struct mnt_idmap* idmap, struct inode* dir, struct dentry* dentry, umode_t mode, bool excl)
{
    pr_info("ramfs: creating file '%pd'\n", dentry);
    return mfs_mknod(&nop_mnt_idmap, dir, dentry, mode | S_IFREG, 0);
}

// unlink operation for deleting a file.
static int mfs_unlink(struct inode* dir, struct dentry* dentry)
{
    pr_info("ramfs: unlinking file '%pd'\n", dentry);
    /*
     * simple_unlink() is a generic helper that performs the standard steps for
     * unlinking a file: it checks permissions, decrements the inode's link
     * count, and if it reaches zero, marks the inode for deletion.
     */
    return simple_unlink(dir, dentry);
}

// rmdir operation for deleting a directory.
static int mfs_rmdir(struct inode* dir, struct dentry* dentry)
{
    pr_info("ramfs: removing directory '%pd'\n", dentry);
    /*
     * simple_rmdir() is a generic helper that checks if a directory is empty
     * and, if so, unlinks it and decrements the parent directory's link count.
     */
    return simple_rmdir(dir, dentry);
}

/*
 * This structure defines the operations that can be performed on a directory's
 * inode. It's how the VFS handles creating, looking up, and deleting files
 * within this directory.
 */
static const struct inode_operations mfs_dir_inode_ops = {
    .create = mfs_create, // Called for the create(2) syscall.
    /*
     * simple_lookup() is a generic helper that looks for a dentry in a
     * directory. Since all dentries are kept in memory, it can just search for
     * it.
     */
    .lookup = simple_lookup,
    .unlink = mfs_unlink, // Called for the unlink(2) syscall.
    .mkdir = mfs_mkdir, // Called for the mkdir(2) syscall.
    .rmdir = mfs_rmdir, // Called for the rmdir(2) syscall.
    .mknod = mfs_mknod, // Called for the mknod(2) syscall.
    /*
     * simple_rename() is a generic helper that handles renaming/moving files.
     * It performs checks and updates directory entries atomically.
     */
    .rename = simple_rename,
};

/**
 * ramfs_show_options - Displays the current mount options in /proc/mounts.
 * @m: The seq_file handle to print to.
 * @root: The root dentry of the mounted filesystem.
 */
static int ramfs_show_options(struct seq_file* m, struct dentry* root)
{
    struct ramfs_fs_info* fsi = root->d_sb->s_fs_info;

    // Only show the mode option if it's not the default value.
    if (fsi->mount_opts.mode != RAMFS_DEFAULT_MODE)
        // seq_printf prints formatted text into the seq_file.
        seq_printf(m, ",mode=%o", fsi->mount_opts.mode);
    return 0;
}

/*
 * This structure defines operations that apply to the entire filesystem,
 * managed via the superblock.
 */
static const struct super_operations mfs_ops = {
    /*
     * simple_statfs() is a generic helper that fills in the statfs struct
     * with basic information, mostly constants since there's no real device
     * size.
     */
    .statfs = simple_statfs,
    /*
     * generic_delete_inode() is a generic helper called by the VFS when an
     * inode's reference count and link count both drop to zero. It handles
     * clearing out the inode's data from the page cache and freeing it.
     */
    .drop_inode = generic_delete_inode,
    .show_options = ramfs_show_options,
};

// Defines the parameters that can be passed at mount time.
enum ramfs_param {
    Opt_mode,
};

// Describes the "mode" parameter for the mount parser.
const struct fs_parameter_spec mfs_fs_parameters[] = {
    // Defines a parameter named "mode" that takes an octal unsigned 32-bit
    // integer.
    fsparam_u32oct("mode", Opt_mode), {}
};

/**
 * ramfs_parse_param - Parses a mount option.
 * @fc: The filesystem context for this mount operation.
 * @param: The parameter to parse.
 *
 * This function is called by the VFS for each mount option provided by the
 * user.
 */
static int ramfs_parse_param(struct fs_context* fc, struct fs_parameter* param)
{
    struct fs_parse_result result;
    struct ramfs_fs_info* fsi = fc->s_fs_info;
    int opt;

    /*
     * fs_parse() is a kernel helper that parses a mount parameter according to
     * the specifications in `mfs_fs_parameters`.
     */
    opt = fs_parse(fc, mfs_fs_parameters, param, &result);
    if (opt < 0)
        return opt;

    // Handle the parsed option.
    switch (opt) {
    case Opt_mode:
        // Store the provided mode in our filesystem-specific info struct.
        fsi->mount_opts.mode = result.uint_32 & S_IALLUGO;
        break;
    }

    return 0;
}

/**
 * ramfs_fill_super - Initializes the superblock for a new mount.
 * @sb: The superblock object to be filled.
 * @fc: The filesystem context containing mount options.
 *
 * This function sets up the core properties of the filesystem instance.
 */
static int ramfs_fill_super(struct super_block* sb, struct fs_context* fc)
{
    struct ramfs_fs_info* fsi = sb->s_fs_info;
    struct inode* inode;

    // Set filesystem properties.
    sb->s_maxbytes = MAX_LFS_FILESIZE; // Maximum file size.
    sb->s_blocksize = PAGE_SIZE; // Use the system's page size as the block size.
    sb->s_blocksize_bits = PAGE_SHIFT; // Bit shift equivalent of the page size.
    sb->s_magic = MFS_MAGIC; // Filesystem's unique identifier.
    sb->s_op = &mfs_ops; // Assign the superblock operations.
    sb->s_time_gran = 1; // Timestamp granularity in nanoseconds.

    // Create the root inode for the filesystem.
    inode = mfs_get_inode(sb, NULL, S_IFDIR | fsi->mount_opts.mode, 0);
    /*
     * d_make_root() allocates the root dentry ("/") for the filesystem and
     * associates it with the newly created root inode.
     */
    sb->s_root = d_make_root(inode);
    if (!sb->s_root)
        return -ENOMEM; // Return "Out of memory" if dentry creation fails.

    return 0;
}

// This function orchestrates the mounting process for a non-device-backed
// filesystem.
static int mfs_get_tree(struct fs_context* fc)
{
    /*
     * get_tree_nodev() is a VFS helper for mounting filesystems that do not
     * reside on a block device. It handles creating the superblock and then
     * calls our provided callback, `ramfs_fill_super`, to initialize it.
     */
    return get_tree_nodev(fc, ramfs_fill_super);
}

// Callback to free the filesystem context information when mounting is done or
// fails.
static void ramfs_free_fc(struct fs_context* fc)
{
    // kfree() is the standard kernel function to free memory allocated with
    // kzalloc/kmalloc.
    kfree(fc->s_fs_info);
}

// Defines the operations for the modern mounting API.
static const struct fs_context_operations ramfs_context_ops = {
    .free = ramfs_free_fc,
    .parse_param = ramfs_parse_param,
    .get_tree = mfs_get_tree,
};

/**
 * mfs_init_fs_context - Entry point for starting a mount operation.
 * @fc: The filesystem context allocated by the VFS.
 *
 * This function is the first one called when a user tries to mount this
 * filesystem type.
 */
int mfs_init_fs_context(struct fs_context* fc)
{
    pr_info("Initializing metric-fs");
    struct ramfs_fs_info* fsi;

    /*
     * kzalloc() allocates memory from the kernel's slab allocator and zeroes it.
     * GFP_KERNEL indicates a normal allocation that can sleep if necessary.
     */
    fsi = kzalloc(sizeof(*fsi), GFP_KERNEL);
    if (!fsi)
        return -ENOMEM;

    // Initialize default mount options.
    fsi->mount_opts.mode = RAMFS_DEFAULT_MODE;
    // Attach our private data and operations to the fs_context.
    fc->s_fs_info = fsi;
    fc->ops = &ramfs_context_ops;
    return 0;
}

// Called when the filesystem is unmounted, to clean up the superblock.
void mfs_kill_sb(struct super_block* sb)
{
    // Free the filesystem-specific info structure.
    kfree(sb->s_fs_info);
    /*
     * kill_litter_super() is a VFS helper that handles the teardown of a
     * simple superblock. It iterates through all inodes and dentries, cleans
     * them up, and frees the superblock itself.
     */
    kill_litter_super(sb);
}

/*
 * This is the main structure that describes the filesystem to the kernel.
 * It's what gets registered and unregistered.
 */
static struct file_system_type ramfs_fs_type = {
    .name = "mfs", // The name used in "mount -t mfs ..."
    /*
     * This function pointer is the entry point for mounting, using the modern
     * fs_context API.
     */
    .init_fs_context = mfs_init_fs_context,
    .parameters = mfs_fs_parameters, // Describes mount parameters.
    .kill_sb = mfs_kill_sb, // Function to call on unmount.
    .fs_flags = FS_USERNS_MOUNT, // Flags describing filesystem capabilities.
};

// Sets the license for this kernel module. It's required for some kernel
// symbols.
MODULE_LICENSE("GPL");

// This function is called when the module is loaded (or at kernel boot).
static int __init init_mfs_fs(void)
{
    /*
     * register_filesystem() tells the VFS about our new filesystem type, making
     * it available to be mounted.
     */
    return register_filesystem(&ramfs_fs_type);
}
/*
 * fs_initcall() is a macro that ensures this initialization function is called
 * at the appropriate time during kernel startup or when the module is loaded.
 */
fs_initcall(init_mfs_fs);
