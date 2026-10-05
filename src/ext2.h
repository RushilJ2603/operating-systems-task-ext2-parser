// ext2 on-disk structures. Fixed width ints only, no methods, nothing that
// could make the compiler add padding, since raw disk bytes get copied
// straight into these. Little endian, same as x86, so no byte swapping.

#ifndef EXT2_H
#define EXT2_H

#include <cstddef>
#include <cstdint>

#define EXT2_MAGIC 0xEF53
#define EXT2_SUPER_OFFSET 1024
#define EXT2_ROOT_INO 2
#define EXT2_GOOD_OLD_INODE_SIZE 128

// i_block[] slots: first 12 point at data, last 3 point at blocks full of
// more block numbers.
#define EXT2_NDIR_BLOCKS 12
#define EXT2_IND_BLOCK   12
#define EXT2_DIND_BLOCK  13
#define EXT2_TIND_BLOCK  14

// top 4 bits of i_mode say what kind of file this is
#define EXT2_S_IFMT   0xF000
#define EXT2_S_IFSOCK 0xC000
#define EXT2_S_IFLNK  0xA000
#define EXT2_S_IFREG  0x8000
#define EXT2_S_IFBLK  0x6000
#define EXT2_S_IFDIR  0x4000
#define EXT2_S_IFCHR  0x2000
#define EXT2_S_IFIFO  0x1000

#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x0002

// Always at byte 1024, 1024 bytes long. Fixed byte offset because the block
// size is stored inside it.
struct ext2_superblock {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;   // 1 if block size is 1024, else 0
    uint32_t s_log_block_size;     // block size = 1024 << this
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    // revision 1+ only. Revision 0 left this area as zero padding.
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
};

struct ext2_group_desc {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint32_t bg_reserved[3];
};

// No filename in here. Names live in directories.
struct ext2_inode {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;    // 512-byte sectors, NOT blocks, and it counts
                          // indirect blocks as well as data
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;   // high 32 bits of size for regular files
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
};

// Header only. The name follows it on disk (name_len bytes, not null
// terminated) and rec_len gives the distance to the next entry, so entries are
// variable length.
struct ext2_dir_entry {
    uint32_t inode;      // 0 = unused slot
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
};

#define EXT2_FT_DIR 2

// A wrong struct size shifts every offset after it, and the only symptom is
// garbage numbers.
static_assert(sizeof(ext2_inode) == 128, "inode must be 128 bytes");
static_assert(sizeof(ext2_group_desc) == 32, "group desc must be 32 bytes");
static_assert(sizeof(ext2_dir_entry) == 8, "dir entry header must be 8 bytes");

#endif
