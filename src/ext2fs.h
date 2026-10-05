// ext2 filesystem reader/writer.
//
// The image is mmap'd, so a read is a memcpy out of a pointer. There is no
// file cursor, which is why any number of threads can read at once with no
// locks. Writing takes one mutex per inode, plus one for the block allocator
// because the bitmap and free counts are shared.

#ifndef EXT2FS_H
#define EXT2FS_H

#include "ext2.h"

#include <cstdint>
#include <iosfwd>
#include <mutex>
#include <string>
#include <vector>

class Ext2Fs {
public:
    // writable=false opens read only
    Ext2Fs(const std::string& path, bool writable = false);
    ~Ext2Fs();
    Ext2Fs(const Ext2Fs&) = delete;
    Ext2Fs& operator=(const Ext2Fs&) = delete;

    // task 1: read core structures
    void dump_superblock(std::ostream& out) const;
    void dump_groups(std::ostream& out) const;

    // task 2: traverse directories
    void print_tree(std::ostream& out, uint32_t ino = EXT2_ROOT_INO) const;

    struct DirEntry {
        uint32_t ino;
        std::string name;
    };
    std::vector<DirEntry> read_dir(const ext2_inode& dir) const;

    // "/dir1/innerdir3/comp-dsa.pdf" to an inode number
    uint32_t lookup(const std::string& path) const;

    // task 3: read file contents
    ext2_inode read_inode(uint32_t ino) const;
    void dump_inode(std::ostream& out, uint32_t ino) const;
    uint64_t file_size(const ext2_inode& in) const;

    // Which block on disk holds the file's n'th block. Handles the indirect
    // levels. Returns 0 for a hole or past the end.
    uint32_t block_for(const ext2_inode& in, uint64_t n) const;

    uint64_t extract(const ext2_inode& in, std::ostream& out) const;
    uint64_t read_data(const ext2_inode& in, uint64_t off, void* buf, uint64_t len) const;

    // task 4: update files. Both allocate blocks as needed and fix up i_size,
    // i_blocks, i_mtime and the free block counters.
    uint64_t append(uint32_t ino, const void* data, uint64_t len);
    uint64_t overwrite(uint32_t ino, const void* data, uint64_t len);

    const ext2_superblock& super() const { return sb_; }
    uint32_t block_size() const { return block_size_; }
    uint32_t ptrs_per_block() const { return ptrs_per_block_; }
    uint32_t group_count() const { return (uint32_t)groups_.size(); }

private:
    void read_at(uint64_t off, void* buf, uint64_t len) const;
    void write_at(uint64_t off, const void* buf, uint64_t len);
    uint32_t read_ptr(uint32_t blk, uint32_t idx) const;
    void tree_rec(std::ostream& out, const ext2_inode& dir, int depth) const;

    uint32_t alloc_block();
    void free_block(uint32_t blk);
    void set_block(ext2_inode& in, uint64_t n, uint32_t phys);
    void write_inode(uint32_t ino, const ext2_inode& in);
    void write_meta();        // caller must hold alloc_lock_
    void sync();
    uint64_t write_range(ext2_inode& in, uint64_t pos, const void* data, uint64_t len);

    int fd_ = -1;
    uint8_t* map_ = nullptr;
    uint64_t map_size_ = 0;
    bool writable_ = false;

    ext2_superblock sb_{};
    std::vector<ext2_group_desc> groups_;
    uint32_t block_size_ = 0;
    uint32_t inode_size_ = 0;
    uint32_t ptrs_per_block_ = 0;

    std::vector<std::mutex> inode_locks_;
    std::mutex alloc_lock_;
};

#endif
