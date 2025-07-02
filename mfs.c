// -------------------------------------------------------------
// mfs: A simple in-memory filesystem with some metric-reporting
// -------------------------------------------------------------

// --------
// Includes
// --------
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pagemap.h>

// ------------------
// Module definitions
// ------------------

MODULE_DESCRIPTION("Simple no-dev filesystem");
MODULE_AUTHOR("Caio, Eduardo, Lucas");
MODULE_LICENSE("GPL");

// ---------
// Constants
// ---------

#define mfs_BLOCKSIZE 4096
#define mfs_BLOCKSIZE_BITS 12
#define mfs_MAGIC 0xbeefcafe
#define LOG_LEVEL KERN_ALERT

// ----------------
// Recorded metrics
// ----------------

static atomic64_t total_bytes_written;
static atomic64_t total_bytes_read;
static atomic64_t total_read_ops;
static atomic64_t total_write_ops;
static atomic64_t total_files_created;

// --------------------------------------
// Forward declarations for regular files
// --------------------------------------

static int mfs_mknod(struct mnt_idmap *idmap, struct inode *dir,
                     struct dentry *dentry, umode_t mode, dev_t dev);
static int mfs_create(struct mnt_idmap *idmap, struct inode *dir,
                      struct dentry *dentry, umode_t mode, bool excl);
static int mfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
                     struct dentry *dentry, umode_t mode);
struct inode *mfs_get_inode(struct super_block *sb, const struct inode *dir,
                            int mode);

// This needs to be declared here since it's not publicly exported by the
// kernel. In the tutorial this wasn't a problem since the filesystem an in-tree
// module, while we are an out-of-tree module.
extern int simple_write_end(struct file *file, struct address_space *mapping,
                            loff_t pos, unsigned len, unsigned copied,
                            struct page *page, void *fsdata);

// ------------------------------------
// Forward declarations for metric file
// ------------------------------------

static ssize_t mfs_metrics_read(struct file *file, char __user *buf, size_t len,
                                loff_t *offset);
static ssize_t mfs_metrics_write(struct file *file, const char __user *buf,
                                 size_t len, loff_t *offset);

// ----------
// Operations
// ----------

static const struct super_operations mfs_ops = {
    .statfs = simple_statfs,
    .drop_inode = generic_drop_inode,
};

static const struct inode_operations mfs_dir_inode_operations = {
    .create = mfs_create,
    .lookup = simple_lookup,
    .link = simple_link,
    .unlink = simple_unlink,
    .mkdir = mfs_mkdir,
    .rmdir = simple_rmdir,
    .mknod = mfs_mknod,
    .rename = simple_rename,
};

static const struct file_operations mfs_file_operations = {
    .read_iter = generic_file_read_iter,
    .write_iter = generic_file_write_iter,
    .mmap = generic_file_mmap,
    .llseek = generic_file_llseek,
};

static const struct inode_operations mfs_file_inode_operations = {
    .getattr = simple_getattr,
};

static const struct file_operations mfs_metrics_operations = {
    .read = mfs_metrics_read, .write = mfs_metrics_write,
    // TODO do we need to add
    // .llseek = genereic_file_llseek
};

// -------------------------
// Operation implementations
// -------------------------

struct inode *mfs_get_inode(struct super_block *sb, const struct inode *dir,
                            int mode) {
  struct inode *inode = new_inode(sb);

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

    inode->i_op = &mfs_dir_inode_operations;

    inc_nlink(inode);
  }

  if (S_ISREG(mode)) {
    inode->i_op = &mfs_file_inode_operations;
    inode->i_fop = &mfs_file_operations;
  }

  return inode;
}

static int mfs_mknod(struct mnt_idmap *idmap, struct inode *dir,
                     struct dentry *dentry, umode_t mode, dev_t dev) {
  printk(LOG_LEVEL "Creating node: %s\n", dentry->d_iname);

  struct inode *inode = mfs_get_inode(dir->i_sb, dir, mode);

  if (inode == NULL)
    return -ENOSPC;

  d_instantiate(dentry, inode);
  dget(dentry);
  dir->__i_mtime = dir->__i_ctime = current_time(inode);

  return 0;
}

static int mfs_create(struct mnt_idmap *idmap, struct inode *dir,
                      struct dentry *dentry, umode_t mode, bool excl) {
  printk(LOG_LEVEL "Creating file: %s\n", dentry->d_iname);

  return mfs_mknod(idmap, dir, dentry, mode | S_IFREG, 0);
}

static int mfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
                     struct dentry *dentry, umode_t mode) {
  printk(LOG_LEVEL "Creating dir: %s\n", dentry->d_iname);
  int ret;

  ret = mfs_mknod(idmap, dir, dentry, mode | S_IFDIR, 0);
  if (ret != 0)
    return ret;

  inc_nlink(dir);

  return 0;
}

// -------------------------
// Custom metrics operations
// -------------------------

// Custom read function which formats current metrics into a buffer
static ssize_t mfs_metrics_read(struct file *file, char __user *buf, size_t len,
                                loff_t *offset) {
  char metrics_buf[1024];

  int metrics_len = scnprintf(
      metrics_buf, sizeof(metrics_buf),
      "bytes written: %lld\n"
      "bytes read: %lld\n"
      "write operations: %lld\n"
      "read operations: %lld\n"
      "files created: %lld\n",
      atomic64_read(&total_bytes_written), atomic64_read(&total_bytes_read),
      atomic64_read(&total_write_ops), atomic64_read(&total_read_ops),
      atomic64_read(&total_files_created));

  // Copies the kernel space memory in metrics_buf correctly to the user space
  // memory in buf
  return simple_read_from_buffer(buf, len, offset, metrics_buf, metrics_len);
}

// No-op write function which returns "Permission denied" on every write
static ssize_t mfs_metrics_write(struct file *file, const char __user *buf,
                                 size_t len, loff_t *offset) {
  return -EACCES;
}

// -------------------
// Filesystem routines
// -------------------

static int mfs_fill_super(struct super_block *sb, void *data, int silent) {
  struct inode *root_inode;
  struct dentry *root_dentry;
  struct inode *metrics_inode;
  struct dentry *metrics_dentry;

  // Mfs properties
  sb->s_maxbytes = MAX_LFS_FILESIZE;
  sb->s_blocksize = mfs_BLOCKSIZE;
  sb->s_blocksize_bits = mfs_BLOCKSIZE_BITS;
  sb->s_magic = mfs_MAGIC;
  sb->s_op = &mfs_ops;

  /* mode = directory & access rights (755) */
  root_inode = mfs_get_inode(
      sb, NULL, S_IFDIR | S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);

  printk(LOG_LEVEL "root inode has %d link(s)\n", root_inode->i_nlink);

  if (!root_inode)
    return -ENOMEM;

  root_dentry = d_make_root(root_inode);
  if (!root_dentry)
    goto out_root_inode;
  sb->s_root = root_dentry;

  // Create "metrics" file

  // Allocate dentry
  metrics_dentry = d_alloc_name(root_dentry, "metrics");
  if (!metrics_dentry)
    goto out_root;

  // Allocate inode
  metrics_inode = mfs_get_inode(sb, root_inode, S_IFREG | 0444);
  if (!metrics_inode) {
    dput(metrics_dentry);
    goto out_root;
  }

  // Set custom operations
  metrics_inode->i_fop = &mfs_metrics_operations;
  // Link dentry and inode
  d_add(metrics_dentry, metrics_inode);

  return 0;

  // Deallocates root inode and dentry
out_root:
  dput(root_dentry);

  // Deallocates root inode
out_root_inode:
  iput(root_inode);
  return -ENOMEM;
}

static struct dentry *mfs_mount(struct file_system_type *fs_type, int flags,
                                const char *dev_name, void *data) {
  printk(LOG_LEVEL "Mounting mfs filesytem\n");
  return mount_nodev(fs_type, flags, data, mfs_fill_super);
}

// We only define this function in order to log the umount operation, since
// the actual work is being done by kill_litter_super
static void mfs_umount(struct super_block *sb) {
  printk(LOG_LEVEL "Unmount mfs filesystem\n");
  kill_litter_super(sb);
}

static struct file_system_type mfs_fs_type = {
    .owner = THIS_MODULE,
    .name = "mfs",
    .mount = mfs_mount,
    .kill_sb = mfs_umount,
};

static int __init mfs_init(void) {
  int err;

  printk(LOG_LEVEL "mfs: registering\n");

  err = register_filesystem(&mfs_fs_type);
  if (err) {
    printk(LOG_LEVEL "mfs: register_filesystem failed\n");
    return err;
  }

  return 0;
}

static void __exit mfs_exit(void) {
  printk(LOG_LEVEL "mfs: unregistering filesystem\n");

  unregister_filesystem(&mfs_fs_type);
}

// Register init and exit routines
module_init(mfs_init);
module_exit(mfs_exit);
