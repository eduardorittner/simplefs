/*
 * mfs.c - A simple in-memory filesystem using direct allocation for both
 * files (kmalloc) and directories (linked list).
 */

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/magic.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h> // Required for mutexes
#include <linux/slab.h> // Required for kzalloc/kfree
#include <linux/uaccess.h> // Required for copy_*_iter

/*
 * =============================================================================
 * Data Structures
 * =============================================================================
 */

// Private data for a regular file (stores content)
struct mfs_file_private {
    void* data;
    size_t size;
};

// Represents one entry (file or subdir) in a directory's linked list
struct mfs_dir_entry {
    char name[NAME_MAX + 1];
    struct inode* inode;
    struct mfs_dir_entry* next;
};

// Private data for a directory (stores linked list and a lock)
struct mfs_dir_private {
    struct mutex lock;
    struct mfs_dir_entry* head;
};

/*
 * =============================================================================
 * Forward Declarations for our custom operations
 * =============================================================================
 */
static int mfs_iterate(struct file* filp, struct dir_context* ctx);
static int mfs_lookup(struct inode* dir, struct dentry* dentry, unsigned int flags);
static int mfs_create(
    struct mnt_idmap* idmap, struct inode* dir, struct dentry* dentry, umode_t mode, bool excl);
static int mfs_mkdir(
    struct mnt_idmap* idmap, struct inode* dir, struct dentry* dentry, umode_t mode);
static int mfs_unlink(struct inode* dir, struct dentry* dentry);
static int mfs_rmdir(struct inode* dir, struct dentry* dentry);
struct inode* ramfs_get_inode(
    struct super_block* sb, const struct inode* dir, umode_t mode, dev_t dev);

/*
 * =============================================================================
 * File Operations (for regular files)
 * =============================================================================
 */

static ssize_t mfs_write_iter(struct kiocb* iocb, struct iov_iter* from)
{
    struct inode* inode = file_inode(iocb->ki_filp);
    struct mfs_file_private* p = inode->i_private;
    void* new_data;
    size_t new_size = iov_iter_count(from);
    if (new_size == 0)
        return 0;
    new_data = kmalloc(new_size, GFP_KERNEL);
    if (!new_data)
        return -ENOMEM;
    if (copy_from_iter(new_data, new_size, from) != new_size) {
        kfree(new_data);
        return -EFAULT;
    }
    kfree(p->data);
    p->data = new_data;
    p->size = new_size;
    inode->i_size = new_size;
    inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
    return new_size;
}

static ssize_t mfs_read_iter(struct kiocb* iocb, struct iov_iter* to)
{
    struct inode* inode = file_inode(iocb->ki_filp);
    struct mfs_file_private* p = inode->i_private;
    loff_t pos = iocb->ki_pos;
    size_t count = iov_iter_count(to);
    if (pos >= p->size)
        return 0;
    count = min(count, (size_t)(p->size - pos));
    if (copy_to_iter(p->data + pos, count, to) != count)
        return -EFAULT;
    iocb->ki_pos += count;
    return count;
}

const struct file_operations mfs_file_operations = {
    .read_iter = mfs_read_iter,
    .write_iter = mfs_write_iter,
    .llseek = generic_file_llseek,
};

const struct inode_operations mfs_file_inode_operations = {
    .setattr = simple_setattr,
    .getattr = simple_getattr,
};

/*
 * =============================================================================
 * Directory Operations (our custom implementation)
 * =============================================================================
 */

// The 'readdir' implementation. Called by VFS to list directory contents.
static int mfs_iterate(struct file* filp, struct dir_context* ctx)
{
    struct inode* dir_inode = file_inode(filp);
    struct mfs_dir_private* p = dir_inode->i_private;
    struct mfs_dir_entry* entry;
    int i = 0;

    mutex_lock(&p->lock);

    // The first two entries are always '.' and '..'
    if (ctx->pos == 0) {
        if (!dir_emit(ctx, ".", 1, dir_inode->i_ino, DT_DIR))
            goto out;
        ctx->pos++;
    }
    if (ctx->pos == 1) {
        if (!dir_emit(ctx, "..", 2, filp->f_path.dentry->d_parent->d_inode->i_ino, DT_DIR))
            goto out;
        ctx->pos++;
    }

    // Find the entry in our list corresponding to the current position.
    entry = p->head;
    for (i = 0; i < ctx->pos - 2 && entry; i++, entry = entry->next)
        ;

    // Iterate through the rest of the list, emitting entries.
    while (entry) {
        if (!dir_emit(ctx, entry->name, strlen(entry->name), entry->inode->i_ino,
                inode_is_dir(entry->inode) ? DT_DIR : DT_REG))
            goto out;

        ctx->pos++;
        entry = entry->next;
    }

out:
    mutex_unlock(&p->lock);
    return 0;
}

// 'lookup' is called by the VFS to find a file in a directory by name.
static int mfs_lookup(struct inode* dir, struct dentry* dentry, unsigned int flags)
{
    struct mfs_dir_private* p = dir->i_private;
    struct mfs_dir_entry* entry;

    mutex_lock(&p->lock);
    for (entry = p->head; entry; entry = entry->next) {
        if (strcmp(entry->name, dentry->d_name.name) == 0) {
            // Found it. Connect the VFS dentry to our found inode.
            d_add(dentry, entry->inode);
            mutex_unlock(&p->lock);
            return 0;
        }
    }
    mutex_unlock(&p->lock);

    // If we reach here, the entry was not found. We add a "negative" dentry.
    d_add(dentry, NULL);
    return 0;
}

// Helper function to add a new file/dir to our linked list.
static int mfs_create_entry(struct inode* dir, struct dentry* dentry, umode_t mode)
{
    struct mfs_dir_private* p = dir->i_private;
    struct mfs_dir_entry* new_entry;
    struct inode* inode;

    // First, create the actual inode for the new file/dir.
    inode = ramfs_get_inode(dir->i_sb, dir, mode, 0);
    if (!inode)
        return -ENOSPC;

    // Allocate our linked list entry structure.
    new_entry = kzalloc(sizeof(struct mfs_dir_entry), GFP_KERNEL);
    if (!new_entry)
        return -ENOMEM;

    // Populate the entry.
    strcpy(new_entry->name, dentry->d_name.name);
    new_entry->inode = inode;

    // Lock and add the new entry to the head of the list.
    mutex_lock(&p->lock);
    new_entry->next = p->head;
    p->head = new_entry;
    mutex_unlock(&p->lock);

    // Tell the VFS to connect the dentry with the new inode.
    d_instantiate(dentry, inode);
    inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));

    return 0;
}

static int mfs_create(
    struct mnt_idmap* idmap, struct inode* dir, struct dentry* dentry, umode_t mode, bool excl)
{
    return mfs_create_entry(dir, dentry, mode | S_IFREG);
}

static int mfs_mkdir(
    struct mnt_idmap* idmap, struct inode* dir, struct dentry* dentry, umode_t mode)
{
    int err = mfs_create_entry(dir, dentry, mode | S_IFDIR);
    if (!err)
        inc_nlink(dir); // For the '..' entry in the new dir.
    return err;
}

// unlink/rmdir helper to remove an entry from the linked list.
static int mfs_remove_entry(struct inode* dir, struct dentry* dentry)
{
    struct mfs_dir_private* p = dir->i_private;
    struct mfs_dir_entry *entry, *prev = NULL;
    int ret = -ENOENT; // "No such file or directory"

    mutex_lock(&p->lock);
    for (entry = p->head; entry; prev = entry, entry = entry->next) {
        if (strcmp(entry->name, dentry->d_name.name) == 0) {
            // For rmdir, check if the directory is empty.
            if (inode_is_dir(entry->inode)) {
                struct mfs_dir_private* child_p = entry->inode->i_private;
                if (child_p->head) {
                    ret = -ENOTEMPTY;
                    goto out;
                }
            }
            // Unlink from the list
            if (prev)
                prev->next = entry->next;
            else
                p->head = entry->next;

            // Decrement link counts and free memory.
            drop_nlink(entry->inode);
            if (inode_is_dir(entry->inode))
                drop_nlink(dir);

            kfree(entry);
            ret = 0;
            goto out;
        }
    }
out:
    mutex_unlock(&p->lock);
    return ret;
}

static int mfs_unlink(struct inode* dir, struct dentry* dentry)
{
    return mfs_remove_entry(dir, dentry);
}
static int mfs_rmdir(struct inode* dir, struct dentry* dentry)
{
    return mfs_remove_entry(dir, dentry);
}

const struct inode_operations mfs_dir_inode_operations = {
    .lookup = mfs_lookup,
    .create = mfs_create,
    .mkdir = mfs_mkdir,
    .unlink = mfs_unlink,
    .rmdir = mfs_rmdir,
};

const struct file_operations mfs_dir_file_operations = {
    .iterate_shared = mfs_iterate,
    .llseek = generic_file_llseek,
};

/*
 * =============================================================================
 * Filesystem Boilerplate (get_inode, super_operations, etc.)
 * =============================================================================
 */

struct ramfs_mount_opts {
    umode_t mode;
};
struct ramfs_fs_info {
    struct ramfs_mount_opts mount_opts;
};
#define RAMFS_DEFAULT_MODE 0755

struct inode* ramfs_get_inode(
    struct super_block* sb, const struct inode* dir, umode_t mode, dev_t dev)
{
    struct inode* inode = new_inode(sb);
    if (inode) {
        inode->i_ino = get_next_ino();
        inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
        simple_inode_init_ts(inode);
        switch (mode & S_IFMT) {
        case S_IFREG: {
            struct mfs_file_private* p = kzalloc(sizeof(*p), GFP_KERNEL);
            if (!p) {
                iput(inode);
                return NULL;
            }
            inode->i_private = p;
            inode->i_op = &mfs_file_inode_operations;
            inode->i_fop = &mfs_file_operations;
            break;
        }
        case S_IFDIR: {
            struct mfs_dir_private* p = kzalloc(sizeof(*p), GFP_KERNEL);
            if (!p) {
                iput(inode);
                return NULL;
            }
            mutex_init(&p->lock);
            inode->i_private = p;
            inode->i_op = &mfs_dir_inode_operations;
            inode->i_fop = &mfs_dir_file_operations;
            inc_nlink(inode); // For '.'
            break;
        }
        default:
            init_special_inode(inode, mode, dev);
            break;
        }
    }
    return inode;
}

static void mfs_evict_inode(struct inode* inode)
{
    clear_inode(inode);
    if (S_ISREG(inode->i_mode) && inode->i_private) {
        struct mfs_file_private* p = inode->i_private;
        kfree(p->data);
        kfree(p);
    } else if (S_ISDIR(inode->i_mode) && inode->i_private) {
        struct mfs_dir_private* p = inode->i_private;
        struct mfs_dir_entry* entry = p->head;
        // Free the entire linked list of directory entries.
        while (entry) {
            struct mfs_dir_entry* next = entry->next;
            kfree(entry);
            entry = next;
        }
        kfree(p);
    }
}

static const struct super_operations ramfs_ops = {
    .statfs = simple_statfs,
    .evict_inode = mfs_evict_inode,
};

static int ramfs_fill_super(struct super_block* sb, struct fs_context* fc)
{
    struct ramfs_fs_info* fsi = kzalloc(sizeof(*fsi), GFP_KERNEL);
    struct inode* inode;
    sb->s_fs_info = fsi;
    if (!fsi)
        return -ENOMEM;
    fsi->mount_opts.mode = RAMFS_DEFAULT_MODE;
    sb->s_maxbytes = MAX_LFS_FILESIZE;
    sb->s_blocksize = PAGE_SIZE;
    sb->s_blocksize_bits = PAGE_SHIFT;
    sb->s_magic = RAMFS_MAGIC;
    sb->s_op = &ramfs_ops;
    sb->s_time_gran = 1;
    inode = ramfs_get_inode(sb, NULL, S_IFDIR | fsi->mount_opts.mode, 0);
    sb->s_root = d_make_root(inode);
    if (!sb->s_root)
        return -ENOMEM;
    return 0;
}

static int ramfs_get_tree(struct fs_context* fc) { return get_tree_nodev(fc, ramfs_fill_super); }

void ramfs_kill_sb(struct super_block* sb)
{
    kfree(sb->s_fs_info);
    kill_litter_super(sb);
}

static struct file_system_type mfs_fs_type = {
    .name = "mfs",
    .init_fs_context = fs_context_for_get_tree,
    .get_tree = ramfs_get_tree,
    .kill_sb = ramfs_kill_sb,
    .fs_flags = FS_USERNS_MOUNT,
};

static int __init init_mfs_fs(void)
{
    pr_info("Initializing MFS (custom directory handling)\n");
    return register_filesystem(&mfs_fs_type);
}

static void __exit exit_mfs_fs(void) { unregister_filesystem(&mfs_fs_type); }

module_init(init_mfs_fs);
module_exit(exit_mfs_fs);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("AI Assistant");
