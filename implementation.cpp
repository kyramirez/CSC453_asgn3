#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include "cpe453fs.h"
#include <fuse.h>

#define BLOCK_SIZE 4096

#define TYPE_SUPERBLOCK  1
#define TYPE_INODE 2
#define TYPE_DIR_EXTENTS  3
#define TYPE_FILE_EXTENTS  4
#define TYPE_FREE 5

#define SB_ROOT_INODE_OFF 4088
#define SB_FREE_LIST_OFF 4092

#define INODE_TYPE_OFF 0
#define INODE_MODE_OFF 4
#define INODE_NLINK_OFF 6
#define INODE_UID_OFF 8
#define INODE_GID_OFF 12
#define INODE_RDEV_OFF 16
#define INODE_ATIME_S_OFF 24
#define INODE_ATIME_N_OFF 28
#define INODE_MTIME_S_OFF 32
#define INODE_MTIME_N_OFF 36
#define INODE_CTIME_S_OFF 40
#define INODE_CTIME_N_OFF 44
#define INODE_SIZE_OFF 48
#define INODE_BLOCKS_OFF 56
#define INODE_CONTENTS_OFF 64
#define INODE_NEXT_EXT_OFF 4092
#define INODE_CONTENTS_LEN (INODE_NEXT_EXT_OFF - INODE_CONTENTS_OFF)

#define FEXT_TYPE_OFF 0
#define FEXT_INODE_OFF 4
#define FEXT_CONTENTS_OFF 8
#define FEXT_NEXT_EXT_OFF 4092
#define FEXT_CONTENTS_LEN (FEXT_NEXT_EXT_OFF - FEXT_CONTENTS_OFF)

#define DEXT_TYPE_OFF 0
#define DEXT_CONTENTS_OFF 4
#define DEXT_NEXT_EXT_OFF 4092
#define DEXT_CONTENTS_LEN (DEXT_NEXT_EXT_OFF - DEXT_CONTENTS_OFF)

#define DIRENT_LEN_OFF 0
#define DIRENT_INODE_OFF 2
#define DIRENT_NAME_OFF 6
#define DIRENT_MIN_LEN 7

static inline uint16_t read16(const unsigned char *buf, int off) {
    
    return (uint16_t)buf[off] | ((uint16_t)buf[off+1] << 8);
}

static inline uint32_t read32(const unsigned char *buf, int off) {
    return (uint32_t)buf[off]
         | ((uint32_t)buf[off+1] <<  8)
         | ((uint32_t)buf[off+2] << 16)

         | ((uint32_t)buf[off+3] << 24);
}

static inline uint64_t read64(const unsigned char *buf, int off) {
    return (uint64_t)read32(buf, off) | ((uint64_t)read32(buf, off+4) << 32);
}

static inline void write16(unsigned char *buf, int off, uint16_t v) {
    buf[off] = (unsigned char)(v & 0xff);
    buf[off+1] = (unsigned char)((v >> 8) & 0xff);
}

static inline void write32(unsigned char *buf, int off, uint32_t v) {
   
    buf[off] = (unsigned char)(v & 0xff);
    buf[off+1] = (unsigned char)((v >>  8) & 0xff);
    buf[off+2] = (unsigned char)((v >> 16) & 0xff);
    buf[off+3] = (unsigned char)((v >> 24) & 0xff);
}


static inline void write64(unsigned char *buf, int off, uint64_t v) {
    write32(buf, off, (uint32_t)(v & 0xffffffff));
    write32(buf, off+4, (uint32_t)((v >> 32) & 0xffffffff));
}

/* state struct */
struct fs_state {
    int fd;
};

/* get current time */
static void get_current_time(struct timespec *ts) {
    clock_gettime(CLOCK_REALTIME, ts);
}

/* get num blocks - total blocs */

static uint32_t get_num_blocks(int fd) {
    off_t size = lseek(fd, 0, SEEK_END);
    return (uint32_t)(size / BLOCK_SIZE);

}

/* allocate block */
static uint32_t allocate_block(struct fs_state *st) {
    
    unsigned char sb[BLOCK_SIZE];
    unsigned char blk[BLOCK_SIZE];

    readblock(st->fd, sb, 0);

    uint32_t free_head = read32(sb, SB_FREE_LIST_OFF);

    if (free_head != 0) {
        readblock(st->fd, blk, free_head);
        uint32_t next = read32(blk, 4);

        write32(sb, SB_FREE_LIST_OFF, next);
        writeblock(st->fd, sb, 0);

        memset(blk, 0, BLOCK_SIZE);
        writeblock(st->fd, blk, free_head);

        return free_head;
    } else {
        uint32_t new_block = get_num_blocks(st->fd);

        memset(blk, 0, BLOCK_SIZE);
        writeblock(st->fd, blk, new_block);
        return new_block;
    }

}

/* free block */
static void free_block(struct fs_state *st, uint32_t block_num) {
    unsigned char sb[BLOCK_SIZE];

    unsigned char blk[BLOCK_SIZE];
    readblock(st->fd, sb, 0);
    uint32_t old_head = read32(sb, SB_FREE_LIST_OFF);

    memset(blk, 0, BLOCK_SIZE);
    write32(blk, 0, TYPE_FREE);

    write32(blk, 4, old_head);
    writeblock(st->fd, blk, block_num);

    write32(sb, SB_FREE_LIST_OFF, block_num);
    writeblock(st->fd, sb, 0);
}


/* scan directory for named entry */
static uint32_t find_in_dir(struct fs_state *st, uint32_t dir_block, const char *name) {

    unsigned char blk[BLOCK_SIZE];
    uint32_t curr = dir_block;
    int is_inode = 1;

    while (curr != 0) {
        readblock(st->fd, blk, curr);

        int content_off = is_inode ? INODE_CONTENTS_OFF : DEXT_CONTENTS_OFF;
        int next_off = is_inode ? INODE_NEXT_EXT_OFF : DEXT_NEXT_EXT_OFF;

        int content_len = next_off - content_off;
        int pos = content_off;
        int end = content_off + content_len;

        while (pos + DIRENT_MIN_LEN <= end) {
            uint16_t elen = read16(blk, pos);
            if (elen == 0) {
                break;
            }

            if (pos + elen > end) {
                break;

            }

            int nlen = elen - DIRENT_NAME_OFF;
            char ename[nlen + 1];

            memcpy(ename, blk + pos + DIRENT_NAME_OFF, nlen);
            ename[nlen] = '\0';

            if (strcmp(ename, name) == 0) {
                return read32(blk, pos + DIRENT_INODE_OFF);
            }

            pos += elen;
        }

        curr = read32(blk, next_off);

        is_inode = 0;
    }

    return 0;
}

static int remove_dir_entry(struct fs_state *st, uint32_t dir_block, const char *name) {
    unsigned char blk[BLOCK_SIZE];
    uint32_t curr = dir_block;
    int is_inode = 1;

    while (curr != 0) {
        readblock(st->fd, blk, curr);

        int content_off = is_inode ? INODE_CONTENTS_OFF : DEXT_CONTENTS_OFF;
        int next_off = is_inode ? INODE_NEXT_EXT_OFF : DEXT_NEXT_EXT_OFF;
        int content_len = next_off - content_off;

        int pos = content_off;
        int end = content_off + content_len;


        while (pos + DIRENT_MIN_LEN <= end) {

            uint16_t entry_len = read16(blk, pos);
            if (entry_len == 0) {
                break;

            }

            if (pos + entry_len > end) {
                break;
            }

            int nlen = entry_len - DIRENT_NAME_OFF;

            char ename[nlen + 1];
            memcpy(ename, blk + pos + DIRENT_NAME_OFF, nlen);
            ename[nlen] = '\0';

            if (strcmp(ename, name) == 0) {

                /* shift remaining entries left */
                int remaining = end - (pos + entry_len);

                if (remaining > 0) {
                    memmove(blk + pos, blk + pos + entry_len, remaining);
                }

                memset(blk + pos + remaining, 0, entry_len);
                writeblock(st->fd, blk, curr);

                /* update dir inode size and timestamp */
                unsigned char iblk[BLOCK_SIZE];
                readblock(st->fd, iblk, dir_block);
                uint64_t dir_size = read64(iblk, INODE_SIZE_OFF);
                if (dir_size >= (uint64_t)entry_len) {
                    dir_size -= entry_len;
                } else {
                    dir_size = 0;
                }

                write64(iblk, INODE_SIZE_OFF, dir_size);


                struct timespec ts;

                get_current_time(&ts);
                write32(iblk, INODE_MTIME_S_OFF, (uint32_t)ts.tv_sec);
                write32(iblk, INODE_MTIME_N_OFF, (uint32_t)ts.tv_nsec);
                write32(iblk, INODE_CTIME_S_OFF, (uint32_t)ts.tv_sec);
                write32(iblk, INODE_CTIME_N_OFF, (uint32_t)ts.tv_nsec);
                writeblock(st->fd, iblk, dir_block);

                return 0;
            }

            pos += entry_len;
        }

        curr = read32(blk, next_off);
        is_inode = 0;
    }

    return -ENOENT;

}

static int add_dir_entry(struct fs_state *st, uint32_t dir_block, const char *name, uint32_t inode_block) {

    unsigned char blk[BLOCK_SIZE];
    int name_len = (int)strlen(name);
    int entry_len = DIRENT_NAME_OFF + name_len;

    uint32_t curr = dir_block;
    uint32_t prev_block = dir_block;
    int is_inode = 1;

    while (curr != 0) {
        readblock(st->fd, blk, curr);
        int content_off = is_inode ? INODE_CONTENTS_OFF : DEXT_CONTENTS_OFF;
        int next_off = is_inode ? INODE_NEXT_EXT_OFF : DEXT_NEXT_EXT_OFF;
        int content_len = next_off - content_off;

        int pos = content_off;
        int end = content_off + content_len;

        /* find end of existing enttries */
        while (pos + DIRENT_MIN_LEN <= end) {
            uint16_t elen = read16(blk, pos + DIRENT_LEN_OFF);

            if (elen == 0) {
                break;
            }

            if (pos + elen > end) {
                break;
            }

            pos += elen;
        }

        if (pos + entry_len <= end) {

            write16(blk, pos + DIRENT_LEN_OFF, (uint16_t)entry_len);
            write32(blk, pos + DIRENT_INODE_OFF, inode_block);
            memcpy(blk + pos + DIRENT_NAME_OFF, name, name_len);

            /* mark next entry as end-of-directory if there is room */
            if (pos + entry_len + 2 <= end) {
                write16(blk, pos + entry_len, 0);
            }

            writeblock(st->fd, blk, curr);

            /* update dir inode size and timpestamp */
            unsigned char iblk[BLOCK_SIZE];
            readblock(st->fd, iblk, dir_block);
            uint64_t dir_size = read64(iblk, INODE_SIZE_OFF);
            dir_size += entry_len;
            write64(iblk, INODE_SIZE_OFF, dir_size);
            
            struct timespec ts;
            get_current_time(&ts);
            write32(iblk, INODE_MTIME_S_OFF, (uint32_t)ts.tv_sec);
            write32(iblk, INODE_MTIME_N_OFF, (uint32_t)ts.tv_nsec);
            write32(iblk, INODE_CTIME_S_OFF, (uint32_t)ts.tv_sec);
            write32(iblk, INODE_CTIME_N_OFF, (uint32_t)ts.tv_nsec);
            writeblock(st->fd, iblk, dir_block);
            return 0;
        }

        prev_block = curr;
        curr = read32(blk, next_off);
        is_inode = 0;
    }

    /* need new extents block */
    uint32_t new_blk_num = allocate_block(st);
    unsigned char new_blk[BLOCK_SIZE];
    memset(new_blk, 0, BLOCK_SIZE);
    write32(new_blk, DEXT_TYPE_OFF, TYPE_DIR_EXTENTS);
    write16(new_blk, DEXT_CONTENTS_OFF + DIRENT_LEN_OFF, (uint16_t)entry_len);
    write32(new_blk, DEXT_CONTENTS_OFF + DIRENT_INODE_OFF, inode_block);
    memcpy(new_blk + DEXT_CONTENTS_OFF + DIRENT_NAME_OFF, name, name_len);

    if (DEXT_CONTENTS_OFF + entry_len + 2 <= DEXT_NEXT_EXT_OFF) {
        write16(new_blk, DEXT_CONTENTS_OFF + entry_len, 0);
    }

    writeblock(st->fd, new_blk, new_blk_num);

    /* link prev to new extents */
    unsigned char prev_blk[BLOCK_SIZE];
    readblock(st->fd, prev_blk, prev_block);

    /* if prev block is in inode or extents */
    uint32_t prev_type = read32(prev_blk, 0);
    int prev_next_off = (prev_type == TYPE_INODE) ? INODE_NEXT_EXT_OFF : DEXT_NEXT_EXT_OFF;

    write32(prev_blk, prev_next_off, new_blk_num);
    writeblock(st->fd, prev_blk, prev_block);

    /* update dir inode */
    unsigned char iblk[BLOCK_SIZE];
    readblock(st->fd, iblk, dir_block);
    uint64_t dir_size = read64(iblk, INODE_SIZE_OFF);
    dir_size += entry_len;
    write64(iblk, INODE_SIZE_OFF, dir_size);

    uint32_t num_blks = read32(iblk, INODE_BLOCKS_OFF);
    write32(iblk, INODE_BLOCKS_OFF, num_blks + 1);

    struct timespec ts2;
    get_current_time(&ts2);
    write32(iblk, INODE_MTIME_S_OFF, (uint32_t)ts2.tv_sec);
    write32(iblk, INODE_MTIME_N_OFF, (uint32_t)ts2.tv_nsec);
    write32(iblk, INODE_CTIME_S_OFF, (uint32_t)ts2.tv_sec);
    write32(iblk, INODE_CTIME_N_OFF, (uint32_t)ts2.tv_nsec);
    writeblock(st->fd, iblk, dir_block);

    return 0;

}

/* read only functions */

static void fs_set_file_descriptor(void *arg, int fd) {
    struct fs_state *st = (struct fs_state *)arg;

    st->fd = fd;
}



static uint32_t fs_root_node(void *arg) {
    
    struct fs_state *st = (struct fs_state *)arg;
    unsigned char buf[BLOCK_SIZE];
    readblock(st->fd, buf, 0);

    return read32(buf, SB_ROOT_INODE_OFF);

}

static int fs_getattr(void *arg, uint32_t block_num, struct stat *stbuf) {
    struct fs_state *st = (struct fs_state *)arg;
    unsigned char buf[BLOCK_SIZE];

    readblock(st->fd, buf, block_num);

    uint32_t type = read32(buf, INODE_TYPE_OFF);
    if (type != TYPE_INODE)
        return -ENOENT;

    memset(stbuf, 0, sizeof(struct stat));

    stbuf->st_mode = (mode_t)read16(buf, INODE_MODE_OFF);
    stbuf->st_nlink = (nlink_t)read16(buf, INODE_NLINK_OFF);
    stbuf->st_uid = (uid_t)read32(buf, INODE_UID_OFF);
    stbuf->st_gid = (gid_t)read32(buf, INODE_GID_OFF);
    stbuf->st_rdev = (dev_t)read32(buf, INODE_RDEV_OFF);
    stbuf->st_size = (off_t)read64(buf, INODE_SIZE_OFF);
    stbuf->st_blocks = (blkcnt_t)read32(buf, INODE_BLOCKS_OFF) * 8;
    
    stbuf->st_blksize = BLOCK_SIZE;
    stbuf->st_atime = (time_t)read32(buf, INODE_ATIME_S_OFF);
    stbuf->st_mtime = (time_t)read32(buf, INODE_MTIME_S_OFF);
    stbuf->st_ctime = (time_t)read32(buf, INODE_CTIME_S_OFF);
    stbuf->st_ino = block_num;

    return 0;
}

static void parse_dir_entries(const unsigned char *buf, int content_off, int content_len, void *cbarg, CPE453_readdir_callback_t cb) {
    
    int pos = content_off;
    int end = content_off + content_len;

    while (pos + DIRENT_MIN_LEN <= end) {
        uint16_t entry_len = read16(buf, pos + DIRENT_LEN_OFF);

        if (entry_len == 0) {
            break;
        }

        if (pos + entry_len > end) {
            break;
        }

        uint32_t inode_block = read32(buf, pos + DIRENT_INODE_OFF);
        int name_len = entry_len - DIRENT_NAME_OFF;

        if (name_len > 0 && inode_block != 0) {
            
            char *name = (char *)malloc(name_len + 1);
            memcpy(name, buf + pos + DIRENT_NAME_OFF, name_len);
            name[name_len] = '\0';
            
            cb(cbarg, name, inode_block);
            free(name);
        }


        pos += entry_len;
    }
}

static int fs_readdir(void *arg, uint32_t block_num, void *buf, CPE453_readdir_callback_t cb) {
    
    struct fs_state *st = (struct fs_state *)arg;
    unsigned char blk[BLOCK_SIZE];

    readblock(st->fd, blk, block_num);

    uint32_t type = read32(blk, INODE_TYPE_OFF);
    
    if (type != TYPE_INODE) {
        return -ENOTDIR;
    }

    uint16_t mode = read16(blk, INODE_MODE_OFF);
    if (!S_ISDIR(mode)) {
        return -ENOTDIR;
    }

    parse_dir_entries(blk, INODE_CONTENTS_OFF, INODE_CONTENTS_LEN, buf, cb);

    uint32_t next = read32(blk, INODE_NEXT_EXT_OFF);
    while (next != 0) {
        readblock(st->fd, blk, next);
        uint32_t ext_type = read32(blk, DEXT_TYPE_OFF);
        if (ext_type != TYPE_DIR_EXTENTS) {
            break;
        }

        parse_dir_entries(blk, DEXT_CONTENTS_OFF, DEXT_CONTENTS_LEN, buf, cb);
        next = read32(blk, DEXT_NEXT_EXT_OFF);
    }

    return 0;
}

static int fs_open(void *arg, uint32_t block_num) {
    
    struct fs_state *st = (struct fs_state *)arg;
    unsigned char buf[BLOCK_SIZE];

    readblock(st->fd, buf, block_num);

    uint32_t type = read32(buf, INODE_TYPE_OFF);
    if (type != TYPE_INODE) {
        return -ENOENT;
    }

    uint16_t mode = read16(buf, INODE_MODE_OFF);
    if (!S_ISREG(mode)) {
        return -ENOENT;

    }

    return 0;
}

static int fs_read(void *arg, uint32_t block_num, char *buff, size_t size, off_t offset) {
    
    struct fs_state *st = (struct fs_state *)arg;
    unsigned char blk[BLOCK_SIZE];

    readblock(st->fd, blk, block_num);

    uint32_t type = read32(blk, INODE_TYPE_OFF);
    if (type != TYPE_INODE) {
        return -ENOENT;
    }

    uint64_t file_size = read64(blk, INODE_SIZE_OFF);

    if ((uint64_t)offset >= file_size) {
        return 0;

    }

    if ((uint64_t)(offset + (off_t)size) > file_size) {
        size = (size_t)(file_size - (uint64_t)offset);

    }

    size_t bytes_read = 0;
    off_t  file_offset = 0;

    /* inode content region */
    {
        off_t chunk_start = 0;
        off_t chunk_end = INODE_CONTENTS_LEN;

        if (offset < chunk_end && bytes_read < size) {
            off_t src_off = (offset > 0) ? offset : 0;
            size_t avail = (size_t)(chunk_end - chunk_start - src_off);
            size_t need = size - bytes_read;
            size_t copy_len = (avail < need) ? avail : need;
            memcpy(buff + bytes_read, blk + INODE_CONTENTS_OFF + src_off, copy_len);
            bytes_read += copy_len;
        }

        file_offset = chunk_end;
    }

    /* file extents chain */
    uint32_t next = read32(blk, INODE_NEXT_EXT_OFF);
    while (next != 0 && bytes_read < size) {
        readblock(st->fd, blk, next);
        uint32_t ext_type = read32(blk, FEXT_TYPE_OFF);

        if (ext_type != TYPE_FILE_EXTENTS) {
            break;
        }

        off_t chunk_start = file_offset;
        off_t chunk_end   = file_offset + FEXT_CONTENTS_LEN;

        if (offset < chunk_end) {
            off_t src_off = (offset > chunk_start) ? (offset - chunk_start) : 0;
            size_t avail = (size_t)(chunk_end - chunk_start - src_off);
            size_t need = size - bytes_read;
            size_t copy_len = (avail < need) ? avail : need;
            memcpy(buff + bytes_read, blk + FEXT_CONTENTS_OFF + src_off, copy_len);
            bytes_read += copy_len;
        }


        file_offset = chunk_end;
        next = read32(blk, FEXT_NEXT_EXT_OFF);
    }

    return (int)bytes_read;
}

static int fs_readlink(void *arg, uint32_t block_num, char *buff, size_t buff_size) {
    struct fs_state *st = (struct fs_state *)arg;
    unsigned char blk[BLOCK_SIZE];

    readblock(st->fd, blk, block_num);

    uint32_t type = read32(blk, INODE_TYPE_OFF);
    if (type != TYPE_INODE) {
        return -ENOENT;
    }

    uint16_t mode = read16(blk, INODE_MODE_OFF);

    if (!S_ISLNK(mode)) {
        return -EINVAL;
    }

    uint64_t path_len = read64(blk, INODE_SIZE_OFF);
    size_t copy_len = (path_len < buff_size - 1) ? (size_t)path_len : buff_size - 1;

    memcpy(buff, blk + INODE_CONTENTS_OFF, copy_len);
    buff[copy_len] = '\0';

    return 0;
}

/* read write functions */


static int fs_chmod(void *arg, uint32_t block_num, mode_t new_mode) {
    struct fs_state *st = (struct fs_state *)arg;
    unsigned char buf[BLOCK_SIZE];
    readblock(st->fd, buf, block_num);
    if (read32(buf, INODE_TYPE_OFF) != TYPE_INODE) {
        return -ENOENT;
    }
 
    write16(buf, INODE_MODE_OFF, (uint16_t)new_mode);

    struct timespec ts; 
    get_current_time(&ts);
    write32(buf, INODE_CTIME_S_OFF, (uint32_t)ts.tv_sec);
    write32(buf, INODE_CTIME_N_OFF, (uint32_t)ts.tv_nsec);
    writeblock(st->fd, buf, block_num);
    return 0;
}


static int fs_chown(void *arg, uint32_t block_num, uid_t new_uid, gid_t new_gid) {
    struct fs_state *st = (struct fs_state *)arg;
    unsigned char buf[BLOCK_SIZE];
    readblock(st->fd, buf, block_num);
    if (read32(buf, INODE_TYPE_OFF) != TYPE_INODE) {
        return -ENOENT;
    }
 
    if ((int)new_uid != -1) {
        write32(buf, INODE_UID_OFF, (uint32_t)new_uid);
    }

    if ((int)new_gid != -1) {
        write32(buf, INODE_GID_OFF, (uint32_t)new_gid);
    }
 
    struct timespec ts; 
    get_current_time(&ts);
    write32(buf, INODE_CTIME_S_OFF, (uint32_t)ts.tv_sec);
    write32(buf, INODE_CTIME_N_OFF, (uint32_t)ts.tv_nsec);
    writeblock(st->fd, buf, block_num);
    return 0;
}

static int fs_utimens(void *arg, uint32_t block_num, const struct timespec tv[2]) {
    struct fs_state *st = (struct fs_state *)arg;
    unsigned char buf[BLOCK_SIZE];
    readblock(st->fd, buf, block_num);
    if (read32(buf, INODE_TYPE_OFF) != TYPE_INODE) {
        return -ENOENT;
    }

    write32(buf, INODE_ATIME_S_OFF, (uint32_t)tv[0].tv_sec);
    write32(buf, INODE_ATIME_N_OFF, (uint32_t)tv[0].tv_nsec);
    write32(buf, INODE_MTIME_S_OFF, (uint32_t)tv[1].tv_sec);
    write32(buf, INODE_MTIME_N_OFF, (uint32_t)tv[1].tv_nsec);
    writeblock(st->fd, buf, block_num);
    return 0;
}

static int fs_rmdir(void *arg, uint32_t block_num, const char *name) {
    struct fs_state *st = (struct fs_state *)arg;
    unsigned char buf[BLOCK_SIZE];
 
    uint32_t target = find_in_dir(st, block_num, name);
    if (target == 0) {
        return -ENOENT;
    }
 
    readblock(st->fd, buf, target);

    if (read32(buf, INODE_TYPE_OFF) != TYPE_INODE) {
        return -ENOENT;
    }

    if (!S_ISDIR(read16(buf, INODE_MODE_OFF))) {
        return -ENOTDIR;
    }

    if (read64(buf, INODE_SIZE_OFF) != 0) {
        return -ENOTEMPTY;
    } 
 
    /* free all directory extent blocks */
    uint32_t next_ext = read32(buf, INODE_NEXT_EXT_OFF);
    while (next_ext != 0) {
        unsigned char ext_blk[BLOCK_SIZE];
        readblock(st->fd, ext_blk, next_ext);
        uint32_t next_next = read32(ext_blk, DEXT_NEXT_EXT_OFF);
        free_block(st, next_ext);
        next_ext = next_next;
    }
    free_block(st, target);
 
    /* decrement parent nlink */
    unsigned char parent_buf[BLOCK_SIZE];
    readblock(st->fd, parent_buf, block_num);
    uint16_t pnlink = read16(parent_buf, INODE_NLINK_OFF);
    if (pnlink > 0) {
        pnlink--;
    }
    write16(parent_buf, INODE_NLINK_OFF, pnlink);
    struct timespec ts; 
    get_current_time(&ts);

    write32(parent_buf, INODE_CTIME_S_OFF, (uint32_t)ts.tv_sec);
    write32(parent_buf, INODE_CTIME_N_OFF, (uint32_t)ts.tv_nsec);
    writeblock(st->fd, parent_buf, block_num);
 
    return remove_dir_entry(st, block_num, name);
}


static int fs_unlink(void *arg, uint32_t block_num, const char *name) {
    struct fs_state *st = (struct fs_state *)arg;
    unsigned char buf[BLOCK_SIZE];
 
    uint32_t target = find_in_dir(st, block_num, name);
    if (target == 0) {
        return -ENOENT;
    }
 
    readblock(st->fd, buf, target);
    if (read32(buf, INODE_TYPE_OFF) != TYPE_INODE) {
        return -ENOENT;
    }
 
    uint16_t nlink = read16(buf, INODE_NLINK_OFF);
    if (nlink > 0) {
        nlink--;
    }

    write16(buf, INODE_NLINK_OFF, nlink);
 
    struct timespec ts; 
    get_current_time(&ts);

    write32(buf, INODE_CTIME_S_OFF, (uint32_t)ts.tv_sec);
    write32(buf, INODE_CTIME_N_OFF, (uint32_t)ts.tv_nsec);
    writeblock(st->fd, buf, target);
 
    if (nlink == 0) {
        /* free all file extent blocks */
        uint32_t next_ext = read32(buf, INODE_NEXT_EXT_OFF);
        while (next_ext != 0) {
            unsigned char ext_blk[BLOCK_SIZE];
            readblock(st->fd, ext_blk, next_ext);
            uint32_t next_next = read32(ext_blk, FEXT_NEXT_EXT_OFF);
            free_block(st, next_ext);
            next_ext = next_next;
        }

        free_block(st, target);
    }
 
    return remove_dir_entry(st, block_num, name);
}


/* helper: initialise a new inode block */
static void init_inode(unsigned char *buf, mode_t mode, dev_t rdev, uid_t uid, gid_t gid) {

    memset(buf, 0, BLOCK_SIZE);
    write32(buf, INODE_TYPE_OFF, TYPE_INODE);
    write16(buf, INODE_MODE_OFF, (uint16_t)mode);
    write32(buf, INODE_UID_OFF, (uint32_t)uid);
    write32(buf, INODE_GID_OFF, (uint32_t)gid);
    write32(buf, INODE_RDEV_OFF, (uint32_t)rdev);
    write32(buf, INODE_BLOCKS_OFF, 1);
 
    struct timespec ts;
    get_current_time(&ts);
    write32(buf, INODE_ATIME_S_OFF, (uint32_t)ts.tv_sec);
    write32(buf, INODE_ATIME_N_OFF, (uint32_t)ts.tv_nsec);
    write32(buf, INODE_MTIME_S_OFF, (uint32_t)ts.tv_sec);
    write32(buf, INODE_MTIME_N_OFF, (uint32_t)ts.tv_nsec);
    write32(buf, INODE_CTIME_S_OFF, (uint32_t)ts.tv_sec);
    write32(buf, INODE_CTIME_N_OFF, (uint32_t)ts.tv_nsec);
}


static int fs_mknod(void *arg, uint32_t parent_block, const char *name, mode_t new_mode, dev_t new_dev) {
    struct fs_state *st = (struct fs_state *)arg;
    unsigned char buf[BLOCK_SIZE];

    if (find_in_dir(st, parent_block, name) != 0) {
        return -EEXIST;
    }

    struct fuse_context *ctx = fuse_get_context();
    uid_t uid = ctx ? ctx->uid : 0;
    gid_t gid = ctx ? ctx->gid : 0;

    uint32_t new_block = allocate_block(st);
    init_inode(buf, new_mode, new_dev, uid, gid);
    write16(buf, INODE_NLINK_OFF, 1);
    writeblock(st->fd, buf, new_block);

    return add_dir_entry(st, parent_block, name, new_block);
}


static int fs_symlink(void *arg, uint32_t parent_block, const char *name, const char *link_dest) {
    struct fs_state *st = (struct fs_state *)arg;
    unsigned char buf[BLOCK_SIZE];

    if (find_in_dir(st, parent_block, name) != 0) {
        return -EEXIST;
    }

    struct fuse_context *ctx = fuse_get_context();
    uid_t uid = ctx ? ctx->uid : 0;
    gid_t gid = ctx ? ctx->gid : 0;

    int dest_len = (int)strlen(link_dest);

    if (dest_len > INODE_CONTENTS_LEN) {
        return -ENAMETOOLONG;
    }

    uint32_t new_block = allocate_block(st);
    init_inode(buf, S_IFLNK | 0777, 0, uid, gid);
    write16(buf, INODE_NLINK_OFF, 1);
    write64(buf, INODE_SIZE_OFF, (uint64_t)dest_len);
    memcpy(buf + INODE_CONTENTS_OFF, link_dest, dest_len);
    writeblock(st->fd, buf, new_block);

    return add_dir_entry(st, parent_block, name, new_block);
}

static int fs_mkdir(void *arg, uint32_t parent_block, const char *name, mode_t new_mode) {
    struct fs_state *st = (struct fs_state *)arg;
    unsigned char buf[BLOCK_SIZE];
    unsigned char parent_buf[BLOCK_SIZE];

    if (find_in_dir(st, parent_block, name) != 0) {
        return -EEXIST;
    }

    struct fuse_context *ctx = fuse_get_context();
    uid_t uid = ctx ? ctx->uid : 0;
    gid_t gid = ctx ? ctx->gid : 0;

    uint32_t new_block = allocate_block(st);
    init_inode(buf, S_IFDIR | (new_mode & 07777), 0, uid, gid);
    write16(buf, INODE_NLINK_OFF, 2);
    write64(buf, INODE_SIZE_OFF, 0);
    writeblock(st->fd, buf, new_block);

    readblock(st->fd, parent_buf, parent_block);
    uint16_t pnlink = read16(parent_buf, INODE_NLINK_OFF);
    write16(parent_buf, INODE_NLINK_OFF, pnlink + 1);

    struct timespec ts;
    get_current_time(&ts);
    write32(parent_buf, INODE_CTIME_S_OFF, (uint32_t)ts.tv_sec);
    write32(parent_buf, INODE_CTIME_N_OFF, (uint32_t)ts.tv_nsec);
    write32(parent_buf, INODE_MTIME_S_OFF, (uint32_t)ts.tv_sec);
    write32(parent_buf, INODE_MTIME_N_OFF, (uint32_t)ts.tv_nsec);
    writeblock(st->fd, parent_buf, parent_block);

    return add_dir_entry(st, parent_block, name, new_block);
}


static int fs_link(void *arg, uint32_t parent_block, const char *name, uint32_t dest_block) {
    struct fs_state *st = (struct fs_state *)arg;
    unsigned char buf[BLOCK_SIZE];

    if (find_in_dir(st, parent_block, name) != 0) {
        return -EEXIST;
    }

    readblock(st->fd, buf, dest_block);
    if (read32(buf, INODE_TYPE_OFF) != TYPE_INODE) {
        return -ENOENT;
    }

    if (S_ISDIR(read16(buf, INODE_MODE_OFF))) {
        return -EPERM;
    }

    uint16_t nlink = read16(buf, INODE_NLINK_OFF);
    write16(buf, INODE_NLINK_OFF, nlink + 1);

    struct timespec ts;
    get_current_time(&ts);
    write32(buf, INODE_CTIME_S_OFF, (uint32_t)ts.tv_sec);
    write32(buf, INODE_CTIME_N_OFF, (uint32_t)ts.tv_nsec);
    writeblock(st->fd, buf, dest_block);

    return add_dir_entry(st, parent_block, name, dest_block);
}


static int fs_rename(void *arg, uint32_t old_parent, const char *old_name, uint32_t new_parent, const char *new_name) {
    struct fs_state *st = (struct fs_state *)arg;

    uint32_t target = find_in_dir(st, old_parent, old_name);
    if (target == 0) {
        return -ENOENT;
    }

    uint32_t existing = find_in_dir(st, new_parent, new_name);
    if (existing != 0) {
        return -EEXIST;
    }

    int res = add_dir_entry(st, new_parent, new_name, target);
    if (res < 0) {
        return res;
    }

    return remove_dir_entry(st, old_parent, old_name);
}
 

/* fs_truncate */

static int fs_truncate(void *arg, uint32_t block_num, off_t new_size) {
    struct fs_state *st = (struct fs_state *)arg;
        unsigned char buf[BLOCK_SIZE];
    
        readblock(st->fd, buf, block_num);

        if (read32(buf, INODE_TYPE_OFF) != TYPE_INODE) {
            return -ENOENT;
        }
    
        uint64_t old_size = read64(buf, INODE_SIZE_OFF);
        uint64_t ns = (uint64_t)new_size;
        uint64_t inode_cap = (uint64_t)INODE_CONTENTS_LEN;
    
        if (ns == old_size) {
            return 0;
        }
    
        if (ns < old_size) {
            /* shrink */
            if (ns <= inode_cap) {
                /* zero unused inode content */
                memset(buf + INODE_CONTENTS_OFF + ns, 0, (size_t)(inode_cap - ns));
                /* free all extents */
                uint32_t next = read32(buf, INODE_NEXT_EXT_OFF);
                write32(buf, INODE_NEXT_EXT_OFF, 0);
                while (next != 0) {
                    unsigned char ext[BLOCK_SIZE];
                    readblock(st->fd, ext, next);
                    uint32_t nn = read32(ext, FEXT_NEXT_EXT_OFF);
                    free_block(st, next);
                    next = nn;
                }
            } else {
                /* walk extents, freeing those past new_size */
                uint64_t byte_pos = inode_cap;
                uint32_t prev = block_num;
                int prev_inode = 1;
                uint32_t next = read32(buf, INODE_NEXT_EXT_OFF);
    
                while (next != 0) {
                    unsigned char ext[BLOCK_SIZE];
                    readblock(st->fd, ext, next);
                    uint64_t ext_end = byte_pos + FEXT_CONTENTS_LEN;
    
                    if (ns <= byte_pos) {
                        /* free and all following extents */
                        unsigned char pb[BLOCK_SIZE];
                        readblock(st->fd, pb, prev);
                        int pnoff = prev_inode ? INODE_NEXT_EXT_OFF : FEXT_NEXT_EXT_OFF;
                        write32(pb, pnoff, 0);
                        writeblock(st->fd, pb, prev);
    
                        while (next != 0) {
                            unsigned char e[BLOCK_SIZE];
                            readblock(st->fd, e, next);
                            uint32_t nn = read32(e, FEXT_NEXT_EXT_OFF);
                            free_block(st, next);
                            next = nn;
                        }
                        break;
                    } else if (ns < ext_end) {
                        /* zero unused tail of this extent, free the rest */
                        uint64_t keep = ns - byte_pos;
                        memset(ext + FEXT_CONTENTS_OFF + keep, 0,
                            (size_t)(FEXT_CONTENTS_LEN - keep));
                        uint32_t nn = read32(ext, FEXT_NEXT_EXT_OFF);
                        write32(ext, FEXT_NEXT_EXT_OFF, 0);
                        writeblock(st->fd, ext, next);
                        while (nn != 0) {
                            unsigned char e[BLOCK_SIZE];
                            readblock(st->fd, e, nn);
                            uint32_t nnn = read32(e, FEXT_NEXT_EXT_OFF);
                            free_block(st, nn);
                            nn = nnn;
                        }
                        break;
                    }
    
                    prev = next;
                    prev_inode = 0;
                    byte_pos = ext_end;
                    next = read32(ext, FEXT_NEXT_EXT_OFF);
                }
            }
        } else {
            /* grow: allocate and zero-fill new blocks as needed */
            /* find the last existing extent */
            uint64_t byte_pos = inode_cap;
            uint32_t prev = block_num;
            int prev_inode = 1;
            uint32_t next = read32(buf, INODE_NEXT_EXT_OFF);
    
            while (next != 0) {
                unsigned char ext[BLOCK_SIZE];
                readblock(st->fd, ext, next);
                prev = next;
                prev_inode = 0;
                byte_pos += FEXT_CONTENTS_LEN;
                next = read32(ext, FEXT_NEXT_EXT_OFF);
            }
    
            /* allocate extents until we cover new_size */
            while (byte_pos < ns) {

                uint32_t new_ext = allocate_block(st);
                unsigned char new_ext_blk[BLOCK_SIZE];
                memset(new_ext_blk, 0, BLOCK_SIZE);
                write32(new_ext_blk, FEXT_TYPE_OFF,  TYPE_FILE_EXTENTS);
                write32(new_ext_blk, FEXT_INODE_OFF, block_num);
                writeblock(st->fd, new_ext_blk, new_ext);
    
                /* link from prev */
                unsigned char pb[BLOCK_SIZE];
                readblock(st->fd, pb, prev);
                int pnoff = prev_inode ? INODE_NEXT_EXT_OFF : FEXT_NEXT_EXT_OFF;
                write32(pb, pnoff, new_ext);
                writeblock(st->fd, pb, prev);
    
                prev = new_ext;
                prev_inode = 0;
                byte_pos += FEXT_CONTENTS_LEN;
    
                /* re read buf since we may have overwritten it via writeblock */
                readblock(st->fd, buf, block_num);
            }
        }
    
        /* reread inode in case it was rewritten during extent allocation */
        readblock(st->fd, buf, block_num);
    
        /* update size and block count */
        write64(buf, INODE_SIZE_OFF, ns);
        uint32_t num_blocks = 1;

        if (ns > inode_cap) {
            uint64_t extra = ns - inode_cap;
            num_blocks += (uint32_t)((extra + FEXT_CONTENTS_LEN - 1) / FEXT_CONTENTS_LEN);
        }
        write32(buf, INODE_BLOCKS_OFF, num_blocks);
    
        struct timespec ts; get_current_time(&ts);
        write32(buf, INODE_MTIME_S_OFF, (uint32_t)ts.tv_sec);
        write32(buf, INODE_MTIME_N_OFF, (uint32_t)ts.tv_nsec);
        write32(buf, INODE_CTIME_S_OFF, (uint32_t)ts.tv_sec);
        write32(buf, INODE_CTIME_N_OFF, (uint32_t)ts.tv_nsec);
        writeblock(st->fd, buf, block_num);
    
        return 0;
}

/* fs_write */
static int fs_write(void *arg, uint32_t block_num, const char *buff, size_t wr_len, off_t wr_offset) {
    struct fs_state *st = (struct fs_state *)arg;
    unsigned char blk[BLOCK_SIZE];
 
    readblock(st->fd, blk, block_num);
    if (read32(blk, INODE_TYPE_OFF) != TYPE_INODE) {
        return -ENOENT;
    }
 
    uint64_t file_size = read64(blk, INODE_SIZE_OFF);
    uint64_t new_end = (uint64_t)wr_offset + wr_len;
 
    /* grow file if needed */
    if (new_end > file_size) {
        /* truncate will allocate blocks and update size */
        int res = fs_truncate(arg, block_num, (off_t)new_end);
        if (res < 0) {
            return res;
        }
        /* re read inode after truncate */
        readblock(st->fd, blk, block_num);
    }
 
    size_t bytes_written = 0;
    off_t  file_offset = 0;
 
    /* write into inode content region */
    {
        off_t chunk_start = 0;
        off_t chunk_end = (off_t)INODE_CONTENTS_LEN;
 
        if ((off_t)wr_offset < chunk_end)
        {
            off_t dst_off = (off_t)wr_offset - chunk_start;
            size_t avail = (size_t)(chunk_end - dst_off);
            size_t need = wr_len - bytes_written;
            size_t copy_len = (avail < need) ? avail : need;
 
            memcpy(blk + INODE_CONTENTS_OFF + dst_off, buff + bytes_written, copy_len);
            bytes_written += copy_len;
        }

        file_offset = chunk_end;
    }
 
    writeblock(st->fd, blk, block_num);
 
    if (bytes_written >= wr_len) {
        return (int)wr_len;
    }
 
    /* walk extents chain */
    uint32_t next = read32(blk, INODE_NEXT_EXT_OFF);
 
    while (next != 0 && bytes_written < wr_len) {
        unsigned char ext[BLOCK_SIZE];
        readblock(st->fd, ext, next);
 
        off_t chunk_start = file_offset;
        off_t chunk_end = file_offset + (off_t)FEXT_CONTENTS_LEN;
 
        if ((off_t)wr_offset + (off_t)wr_len > chunk_start && (off_t)wr_offset < chunk_end) {
            off_t dst_off = ((off_t)wr_offset > chunk_start) ? ((off_t)wr_offset - chunk_start) : 0;

            if (dst_off < 0) {
                dst_off = 0;
            }

            size_t avail = (size_t)(chunk_end - (chunk_start + dst_off));
            size_t need = wr_len - bytes_written;
            size_t copy_len = (avail < need) ? avail : need;
 
            memcpy(ext + FEXT_CONTENTS_OFF + dst_off, buff + bytes_written, copy_len);
            bytes_written += copy_len;
            writeblock(st->fd, ext, next);
        }
 
        file_offset = chunk_end;
        next = read32(ext, FEXT_NEXT_EXT_OFF);
    }
 
    /* update mtime on inode */
    readblock(st->fd, blk, block_num);

    struct timespec ts; 
    get_current_time(&ts);
    write32(blk, INODE_MTIME_S_OFF, (uint32_t)ts.tv_sec);
    write32(blk, INODE_MTIME_N_OFF, (uint32_t)ts.tv_nsec);
    writeblock(st->fd, blk, block_num);
 
    return (int)wr_len;
}


#ifdef __cplusplus
extern "C" {
#endif

struct cpe453fs_ops *CPE453_get_operations(void) {
    static struct cpe453fs_ops ops;
    static struct fs_state state;

    memset(&ops, 0, sizeof(ops));
    memset(&state, 0, sizeof(state));

    ops.arg = &state;

    ops.set_file_descriptor = fs_set_file_descriptor;
    ops.root_node = fs_root_node;
    ops.getattr = fs_getattr;
    ops.readdir  = fs_readdir;
    ops.open = fs_open;
    ops.read = fs_read;
    ops.readlink  = fs_readlink;

    ops.chmod = fs_chmod;
    ops.chown = fs_chown;
    ops.utimens = fs_utimens;
    ops.rmdir = fs_rmdir;
    ops.unlink = fs_unlink;
    ops.mknod = fs_mknod;
    ops.symlink = fs_symlink;
    ops.mkdir = fs_mkdir;
    ops.link = fs_link;
    ops.rename = fs_rename;
    ops.truncate = fs_truncate;
    ops.write = fs_write;

    return &ops;
}


#ifdef __cplusplus
}
#endif