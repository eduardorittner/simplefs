/*
<<<<<<< HEAD
 * mfs.c - A simple in-memory filesystem using direct allocation for both
 * files (kmalloc) and directories (linked list).
=======
 * SO2 Lab - Filesystem drivers
 * Exercise #1 (no-dev filesystem)
>>>>>>> 80ac265 (startign point from tutorial)
 */

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pagemap.h>

MODULE_DESCRIPTION("Simple no-dev filesystem");
MODULE_AUTHOR("SO2");
MODULE_LICENSE("GPL");

#define MYFS_BLOCKSIZE 4096
#define MYFS_BLOCKSIZE_BITS 12
#define MYFS_MAGIC 0xbeefcafe
#define LOG_LEVEL KERN_ALERT

/* declarations of functions that are part of operation structures */

static int myfs_mknod(
    struct mnt_idmap* idmap, struct inode* dir, struct dentry* dentry, umode_t mode, dev_t dev);
static int myfs_create(
    struct mnt_idmap* idmap, struct inode* dir, struct dentry* dentry, umode_t mode, bool excl);
static int myfs_mkdir(
    struct mnt_idmap* idmap, struct inode* dir, struct dentry* dentry, umode_t mode);
struct inode* myfs_get_inode(struct super_block* sb, const struct inode* dir, int mode);

static const struct super_operations myfs_ops = {
    .statfs = simple_statfs,
    .drop_inode = generic_drop_inode,
};

static const struct inode_operations myfs_dir_inode_operations = {
    .create = myfs_create,
    .lookup = simple_lookup,
    .link = simple_link,
    .unlink = simple_unlink,
    .mkdir = myfs_mkdir,
    .rmdir = simple_rmdir,
    .mknod = myfs_mknod,
    .rename = simple_rename,
};

static const struct file_operations myfs_file_operations = {
    .read_iter = generic_file_read_iter,
    .write_iter = generic_file_write_iter,
    .mmap = generic_file_mmap,
    .llseek = generic_file_llseek,
};

static const struct inode_operations myfs_file_inode_operations = {
    .getattr = simple_getattr,
};

extern int simple_write_end(struct file* file, struct address_space* mapping, loff_t pos,
    unsigned len, unsigned copied, struct page* page, void* fsdata);

struct inode* myfs_get_inode(struct super_block* sb, const struct inode* dir, int mode)
{
    struct inode* inode = new_inode(sb);

    if (!inode)
        return NULL;

    inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
    inode->__i_atime = inode->__i_mtime = inode->__i_ctime = current_time(inode);
    inode->i_ino = 1;

    inode->i_ino = get_next_ino();

    inode->i_mapping->a_ops = &ram_aops;

    if (S_ISDIR(mode)) {
        inode->i_op = &simple_dir_inode_operations;
        inode->i_fop = &simple_dir_operations;

        inode->i_op = &myfs_dir_inode_operations;

        inc_nlink(inode);
    }

    if (S_ISREG(mode)) {
        inode->i_op = &myfs_file_inode_operations;
        inode->i_fop = &myfs_file_operations;
    }

    return inode;
}

static int myfs_mknod(
    struct mnt_idmap* idmap, struct inode* dir, struct dentry* dentry, umode_t mode, dev_t dev)
{
    printk(LOG_LEVEL "Creating node: %s\n", dentry->d_iname);

    struct inode* inode = myfs_get_inode(dir->i_sb, dir, mode);

    if (inode == NULL)
        return -ENOSPC;

    d_instantiate(dentry, inode);
    dget(dentry);
    dir->__i_mtime = dir->__i_ctime = current_time(inode);

    return 0;
}

static int myfs_create(
    struct mnt_idmap* idmap, struct inode* dir, struct dentry* dentry, umode_t mode, bool excl)
{
    printk(LOG_LEVEL "Creating file: %s\n", dentry->d_iname);

    return myfs_mknod(idmap, dir, dentry, mode | S_IFREG, 0);
}

static int myfs_mkdir(
    struct mnt_idmap* idmap, struct inode* dir, struct dentry* dentry, umode_t mode)
{
    printk(LOG_LEVEL "Creating dir: %s\n", dentry->d_iname);
    int ret;

    ret = myfs_mknod(idmap, dir, dentry, mode | S_IFDIR, 0);
    if (ret != 0)
        return ret;

    inc_nlink(dir);

    return 0;
}

static int myfs_fill_super(struct super_block* sb, void* data, int silent)
{
    struct inode* root_inode;
    struct dentry* root_dentry;

    sb->s_maxbytes = MAX_LFS_FILESIZE;
    sb->s_blocksize = MYFS_BLOCKSIZE;
    sb->s_blocksize_bits = MYFS_BLOCKSIZE_BITS;
    sb->s_magic = MYFS_MAGIC;
    sb->s_op = &myfs_ops;

    /* mode = directory & access rights (755) */
    root_inode
        = myfs_get_inode(sb, NULL, S_IFDIR | S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);

    printk(LOG_LEVEL "root inode has %d link(s)\n", root_inode->i_nlink);

    if (!root_inode)
        return -ENOMEM;

    root_dentry = d_make_root(root_inode);
    if (!root_dentry)
        goto out_no_root;
    sb->s_root = root_dentry;

    return 0;

out_no_root:
    iput(root_inode);
    return -ENOMEM;
}

static struct dentry* myfs_mount(
    struct file_system_type* fs_type, int flags, const char* dev_name, void* data)
{
    printk(LOG_LEVEL "Mounting myfs filesytem\n");
    return mount_nodev(fs_type, flags, data, myfs_fill_super);
}

/* We only define this function in order to log, since the actual work is being done by
 * kill_litter_super */
static void myfs_umount(struct super_block* sb)
{
    printk(LOG_LEVEL "Unmount myfs filesystem\n");
    kill_litter_super(sb);
}

static struct file_system_type myfs_fs_type = {
    .owner = THIS_MODULE,
    .name = "myfs",
    .mount = myfs_mount,
    .kill_sb = myfs_umount,
};

static int __init myfs_init(void)
{
    int err;

    printk(LOG_LEVEL "myfs: registering\n");

    err = register_filesystem(&myfs_fs_type);
    if (err) {
        printk(LOG_LEVEL "myfs: register_filesystem failed\n");
        return err;
    }

    return 0;
}

static void __exit myfs_exit(void)
{
    printk(LOG_LEVEL "myfs: unregistering filesystem\n");

    unregister_filesystem(&myfs_fs_type);
}

module_init(myfs_init);
module_exit(myfs_exit);
