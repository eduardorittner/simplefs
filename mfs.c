/*
 * ramfs.c - A combined version of the Linux ramfs implementation.
 *
 * This file is a combination of the following files from the Linux kernel
 * source:
 * - fs/ramfs/inode.c
 * - fs/ramfs/file-mmu.c
 * - fs/ramfs/file-nommu.c
 *
 * The original copyright notices and comments have been preserved.
 *
 * This combination is intended for educational purposes to simplify
 * building and modifying a basic in-memory filesystem.
 */

/*
 * Combined header includes from inode.c, file-mmu.c, and file-nommu.c
 */
#include <linux/backing-dev.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/highmem.h>
#include <linux/init.h>
#include <linux/magic.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/pagevec.h>
#include <linux/parser.h>
#include <linux/ramfs.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/time.h>
#include <linux/uaccess.h>

/*
 * From fs/ramfs/file-mmu.c
 */

const struct file_operations ramfs_file_operations = {
    .read_iter = generic_file_read_iter,
    .write_iter = generic_file_write_iter,
    .fsync = noop_fsync,
    .llseek = generic_file_llseek,
};

const struct inode_operations ramfs_file_inode_operations = {
    .setattr = simple_setattr,
    .getattr = simple_getattr,
};

/*
 * =============================================================================
 * START - Core inode and filesystem logic
 *
 * This code is from fs/ramfs/inode.c
 * =============================================================================
 */

struct ramfs_mount_opts {
  umode_t mode;
};

struct ramfs_fs_info {
  struct ramfs_mount_opts mount_opts;
};

#define RAMFS_DEFAULT_MODE 0755

static const struct super_operations ramfs_ops;
static const struct inode_operations ramfs_dir_inode_operations;

/*
 * NOTE: The original ramfs code uses `ram_aops` which is defined in mm/shmem.c.
 * You will need to provide an implementation for these address_space_operations
 * or link against the existing one for this code to compile and link.
 * For example:
 * extern const struct address_space_operations ram_aops;
 */
extern const struct address_space_operations ram_aops;

struct inode *ramfs_get_inode(struct super_block *sb, const struct inode *dir,
                              umode_t mode, dev_t dev) {
  struct inode *inode = new_inode(sb);

  if (inode) {
    inode->i_ino = get_next_ino();
    inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
    inode->i_mapping->a_ops = &ram_aops;
    mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
    mapping_set_unevictable(inode->i_mapping);
    simple_inode_init_ts(inode);
    switch (mode & S_IFMT) {
    // If is a file
    case S_IFREG:
      inode->i_op = &ramfs_file_inode_operations;
      inode->i_fop = &ramfs_file_operations;
      break;
    // Is a directory
    case S_IFDIR:
      inode->i_op = &ramfs_dir_inode_operations;
      inode->i_fop = &simple_dir_operations;
      inc_nlink(inode);
      break;
    // These can be symlinks or other file types like devices, etc
    default:
      pr_info("File type not supported");
      // Free the unused inode
      iput(inode);
      init_special_inode(inode, mode, dev);
      break;
    }
  }
  return inode;
}

static int ramfs_mknod(struct mnt_idmap *idmap, struct inode *dir,
                       struct dentry *dentry, umode_t mode, dev_t dev) {
  struct inode *inode = ramfs_get_inode(dir->i_sb, dir, mode, dev);
  int error = -ENOSPC;

  if (inode) {
    d_instantiate(dentry, inode);
    dget(dentry);
    error = 0;
    inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
  }
  return error;
}

static int ramfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
                       struct dentry *dentry, umode_t mode) {
  pr_info("ramfs: creating directory '%pd'\n", dentry);

  int retval = ramfs_mknod(&nop_mnt_idmap, dir, dentry, mode | S_IFDIR, 0);
  if (!retval)
    inc_nlink(dir);
  return retval;
}

static int ramfs_create(struct mnt_idmap *idmap, struct inode *dir,
                        struct dentry *dentry, umode_t mode, bool excl) {
  pr_info("ramfs: creating file '%pd'\n", dentry);
  return ramfs_mknod(&nop_mnt_idmap, dir, dentry, mode | S_IFREG, 0);
}

static int ramfs_unlink(struct inode *dir, struct dentry *dentry) {
  pr_info("ramfs: unlinking file '%pd'\n", dentry);
  return simple_unlink(dir, dentry);
}

static int ramfs_rmdir(struct inode *dir, struct dentry *dentry) {
  pr_info("ramfs: removing directory '%pd'\n", dentry);
  return simple_rmdir(dir, dentry);
}

static const struct inode_operations ramfs_dir_inode_operations = {
    .create = ramfs_create,
    .lookup = simple_lookup,
    .unlink = ramfs_unlink,
    .mkdir = ramfs_mkdir,
    .rmdir = ramfs_rmdir,
    .mknod = ramfs_mknod,
    .rename = simple_rename,
};

static int ramfs_show_options(struct seq_file *m, struct dentry *root) {
  struct ramfs_fs_info *fsi = root->d_sb->s_fs_info;

  if (fsi->mount_opts.mode != RAMFS_DEFAULT_MODE)
    seq_printf(m, ",mode=%o", fsi->mount_opts.mode);
  return 0;
}

static const struct super_operations ramfs_ops = {
    .statfs = simple_statfs,
    .drop_inode = generic_delete_inode,
    .show_options = ramfs_show_options,
};

enum ramfs_param {
  Opt_mode,
};

const struct fs_parameter_spec ramfs_fs_parameters[] = {
    fsparam_u32oct("mode", Opt_mode), {}};

static int ramfs_parse_param(struct fs_context *fc,
                             struct fs_parameter *param) {
  struct fs_parse_result result;
  struct ramfs_fs_info *fsi = fc->s_fs_info;
  int opt;

  opt = fs_parse(fc, ramfs_fs_parameters, param, &result);
  if (opt == -ENOPARAM) {
    opt = vfs_parse_fs_param_source(fc, param);
    if (opt != -ENOPARAM)
      return opt;
    return 0;
  }
  if (opt < 0)
    return opt;

  switch (opt) {
  case Opt_mode:
    fsi->mount_opts.mode = result.uint_32 & S_IALLUGO;
    break;
  }

  return 0;
}

static int ramfs_fill_super(struct super_block *sb, struct fs_context *fc) {
  struct ramfs_fs_info *fsi = sb->s_fs_info;
  struct inode *inode;

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

static int ramfs_get_tree(struct fs_context *fc) {
  return get_tree_nodev(fc, ramfs_fill_super);
}

static void ramfs_free_fc(struct fs_context *fc) { kfree(fc->s_fs_info); }

static const struct fs_context_operations ramfs_context_ops = {
    .free = ramfs_free_fc,
    .parse_param = ramfs_parse_param,
    .get_tree = ramfs_get_tree,
};

int ramfs_init_fs_context(struct fs_context *fc) {
  pr_info("Initializing metric-fs");
  struct ramfs_fs_info *fsi;

  fsi = kzalloc(sizeof(*fsi), GFP_KERNEL);
  if (!fsi)
    return -ENOMEM;

  fsi->mount_opts.mode = RAMFS_DEFAULT_MODE;
  fc->s_fs_info = fsi;
  fc->ops = &ramfs_context_ops;
  return 0;
}

void ramfs_kill_sb(struct super_block *sb) {
  kfree(sb->s_fs_info);
  kill_litter_super(sb);
}

static struct file_system_type ramfs_fs_type = {
    .name = "mfs",
    .init_fs_context = ramfs_init_fs_context,
    .parameters = ramfs_fs_parameters,
    .kill_sb = ramfs_kill_sb,
    .fs_flags = FS_USERNS_MOUNT,
};

MODULE_LICENSE("GPL");

static int __init init_ramfs_fs(void) {
  return register_filesystem(&ramfs_fs_type);
}
fs_initcall(init_ramfs_fs);
