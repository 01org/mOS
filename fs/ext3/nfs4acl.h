#ifndef __FS_EXT3_NFS4ACL_H
#define __FS_EXT3_NFS4ACL_H

#ifdef CONFIG_EXT3_FS_NFS4ACL

extern int ext3_nfs4acl_permission(struct inode *, int);
extern int ext3_nfs4acl_init(handle_t *, struct inode *, struct inode *);
extern int ext3_nfs4acl_chmod(struct inode *);

#else  /* CONFIG_FS_EXT3_NFS4ACL */

static inline int
ext3_nfs4acl_init(handle_t *handle, struct inode *inode, struct inode *dir)
{
	return 0;
}

static inline int
ext3_nfs4acl_chmod(struct inode *inode)
{
	return 0;
}

#endif  /* CONFIG_FS_EXT3_NFS4ACL */

#endif  /* __FS_EXT3_NFS4ACL_H */
