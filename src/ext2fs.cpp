#include "ext2fs.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <stdexcept>

namespace {

// Strings rather than std::setw, because setw and hex are sticky: switch hex
// on and every number after it prints in hex until something switches it back.
std::string pad(const std::string& s, size_t w) {
    return s.size() >= w ? s : s + std::string(w - s.size(), ' ');
}

std::string num(uint64_t v) { return std::to_string(v); }

std::string hex4(uint16_t v) {
    std::ostringstream s;
    s << "0x" << std::uppercase << std::hex << std::setw(4) << std::setfill('0') << v;
    return s.str();
}

std::string timestr(uint32_t t) {
    if (t == 0) return "never";
    std::time_t tt = (std::time_t)t;
    std::tm tmv{};
    gmtime_r(&tt, &tmv);
    char buf[64];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S UTC", &tmv);
    return buf;
}

std::string field(const char* name) { return "  " + pad(name, 20) + ": "; }

char type_char(uint16_t mode) {
    switch (mode & EXT2_S_IFMT) {
        case EXT2_S_IFREG:  return '-';
        case EXT2_S_IFDIR:  return 'd';
        case EXT2_S_IFLNK:  return 'l';
        case EXT2_S_IFBLK:  return 'b';
        case EXT2_S_IFCHR:  return 'c';
        case EXT2_S_IFIFO:  return 'p';
        case EXT2_S_IFSOCK: return 's';
        default:            return '?';
    }
}

std::string perms(uint16_t mode) {
    static const char* bits[8] = {"---","--x","-w-","-wx","r--","r-x","rw-","rwx"};
    return std::string(bits[(mode >> 6) & 7]) + bits[(mode >> 3) & 7] + bits[mode & 7];
}

std::vector<std::string> split_path(const std::string& p) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : p) {
        if (c == '/') {
            if (!cur.empty()) parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

}  // namespace

Ext2Fs::Ext2Fs(const std::string& path, bool writable) : writable_(writable) {
    fd_ = ::open(path.c_str(), writable ? O_RDWR : O_RDONLY);
    if (fd_ < 0) throw std::runtime_error("cannot open " + path + ": " + std::strerror(errno));

    struct stat st;
    if (::fstat(fd_, &st) != 0) {
        ::close(fd_);
        throw std::runtime_error("fstat failed");
    }
    map_size_ = (uint64_t)st.st_size;

    int prot = writable ? (PROT_READ | PROT_WRITE) : PROT_READ;
    void* m = ::mmap(nullptr, map_size_, prot, MAP_SHARED, fd_, 0);
    if (m == MAP_FAILED) {
        ::close(fd_);
        throw std::runtime_error(std::string("mmap failed: ") + std::strerror(errno));
    }
    map_ = (uint8_t*)m;

    read_at(EXT2_SUPER_OFFSET, &sb_, sizeof sb_);
    if (sb_.s_magic != EXT2_MAGIC) {
        ::munmap(map_, map_size_);
        ::close(fd_);
        throw std::runtime_error("not an ext2 image, magic was " + hex4(sb_.s_magic));
    }

    block_size_ = 1024u << sb_.s_log_block_size;
    ptrs_per_block_ = block_size_ / 4;

    // Revision 0 always used 128 byte inodes and had no field for it.
    inode_size_ = (sb_.s_rev_level == 0) ? EXT2_GOOD_OLD_INODE_SIZE : sb_.s_inode_size;
    if (inode_size_ < sizeof(ext2_inode)) throw std::runtime_error("bad inode size");

    uint64_t data_blocks = sb_.s_blocks_count - sb_.s_first_data_block;
    uint32_t n = (uint32_t)((data_blocks + sb_.s_blocks_per_group - 1) / sb_.s_blocks_per_group);
    groups_.resize(n);

    // Table sits on the block after whichever block holds the superblock:
    // block 2 with 1K blocks, block 1 with bigger ones.
    read_at((uint64_t)(sb_.s_first_data_block + 1) * block_size_,
            groups_.data(), (uint64_t)n * sizeof(ext2_group_desc));

    std::vector<std::mutex> locks(sb_.s_inodes_count + 1);
    inode_locks_.swap(locks);
}

Ext2Fs::~Ext2Fs() {
    if (map_) ::munmap(map_, map_size_);
    if (fd_ >= 0) ::close(fd_);
}

// Reads are just memcpy out of the mapping. No cursor, no locking needed.
void Ext2Fs::read_at(uint64_t off, void* buf, uint64_t len) const {
    if (off + len > map_size_)
        throw std::runtime_error("read past end of image at offset " + num(off));
    std::memcpy(buf, map_ + off, len);
}

void Ext2Fs::write_at(uint64_t off, const void* buf, uint64_t len) {
    if (!writable_) throw std::runtime_error("image was opened read only");
    if (off + len > map_size_)
        throw std::runtime_error("write past end of image at offset " + num(off));
    std::memcpy(map_ + off, buf, len);
}

void Ext2Fs::sync() {
    if (writable_ && ::msync(map_, map_size_, MS_SYNC) != 0)
        throw std::runtime_error("msync failed");
}

// task 1: superblock and group descriptors

void Ext2Fs::dump_superblock(std::ostream& out) const {
    std::string vol(sb_.s_volume_name, sizeof sb_.s_volume_name);
    vol.erase(std::find(vol.begin(), vol.end(), '\0'), vol.end());

    out << "superblock, byte " << EXT2_SUPER_OFFSET << "\n\n";
    out << field("magic") << hex4(sb_.s_magic)
        << (sb_.s_magic == EXT2_MAGIC ? "  ok, this is ext2" : "  BAD") << "\n";
    out << field("volume name") << (vol.empty() ? "(none)" : vol) << "\n";
    out << field("revision") << sb_.s_rev_level << "." << sb_.s_minor_rev_level << "\n";
    out << field("state") << (sb_.s_state == 1 ? "clean" : "has errors") << "\n";
    out << "\n";
    out << field("block size") << block_size_ << " (1024 << " << sb_.s_log_block_size << ")\n";
    out << field("total blocks") << sb_.s_blocks_count << "\n";
    out << field("free blocks") << sb_.s_free_blocks_count << "\n";
    out << field("first data block") << sb_.s_first_data_block << "\n";
    out << field("blocks per group") << sb_.s_blocks_per_group << "\n";
    out << "\n";
    out << field("total inodes") << sb_.s_inodes_count << "\n";
    out << field("free inodes") << sb_.s_free_inodes_count << "\n";
    out << field("inodes per group") << sb_.s_inodes_per_group << "\n";
    out << field("inode size") << inode_size_ << " (struct is "
        << sizeof(ext2_inode) << ", stride by the bigger one)\n";
    out << "\n";
    out << field("last mounted") << timestr(sb_.s_mtime) << "\n";
    out << field("last written") << timestr(sb_.s_wtime) << "\n";
    out << "\n";

    out << "  worked out from the above:\n";
    out << field("block groups") << group_count() << "\n";
    out << field("pointers per block") << ptrs_per_block_ << " (" << block_size_ << " / 4)\n";
    out << field("bits per bitmap blk") << (block_size_ * 8) << " which matches blocks per group\n";
    out << field("file_type in dirents")
        << ((sb_.s_feature_incompat & EXT2_FEATURE_INCOMPAT_FILETYPE) ? "yes" : "no") << "\n";

    uint64_t P = ptrs_per_block_, bs = block_size_;
    out << field("max size, direct") << (EXT2_NDIR_BLOCKS * bs) << " bytes\n";
    out << field("    + singly") << ((EXT2_NDIR_BLOCKS + P) * bs) << " bytes\n";
    out << field("    + doubly") << ((EXT2_NDIR_BLOCKS + P + P * P) * bs) << " bytes\n";
    out << field("    + triply") << ((EXT2_NDIR_BLOCKS + P + P * P + P * P * P) * bs) << " bytes\n";
}

void Ext2Fs::dump_groups(std::ostream& out) const {
    out << "block group descriptors, " << group_count()
        << " groups, table starts at block " << (sb_.s_first_data_block + 1) << "\n\n";
    out << "  " << pad("group", 7) << pad("blk bitmap", 12) << pad("ino bitmap", 12)
        << pad("ino table", 11) << pad("free blks", 11) << pad("free inos", 11) << "dirs\n";
    for (uint32_t g = 0; g < groups_.size(); g++) {
        const ext2_group_desc& d = groups_[g];
        out << "  " << pad(num(g), 7) << pad(num(d.bg_block_bitmap), 12)
            << pad(num(d.bg_inode_bitmap), 12) << pad(num(d.bg_inode_table), 11)
            << pad(num(d.bg_free_blocks_count), 11)
            << pad(num(d.bg_free_inodes_count), 11) << d.bg_used_dirs_count << "\n";
    }
    out << "\n";
    for (uint32_t g = 0; g < groups_.size(); g++) {
        uint32_t lo = g * sb_.s_inodes_per_group + 1;
        uint32_t hi = std::min(lo + sb_.s_inodes_per_group - 1, sb_.s_inodes_count);
        out << "  group " << g << " has inodes " << lo << " to " << hi << "\n";
    }
}

// inodes

ext2_inode Ext2Fs::read_inode(uint32_t ino) const {
    if (ino == 0 || ino > sb_.s_inodes_count)
        throw std::runtime_error("inode " + num(ino) + " out of range");

    // Inode numbers are 1-based, the table is 0-based, so subtract 1 once.
    uint32_t group = (ino - 1) / sb_.s_inodes_per_group;
    uint32_t index = (ino - 1) % sb_.s_inodes_per_group;
    if (group >= groups_.size()) throw std::runtime_error("inode in a group that doesn't exist");

    uint64_t off = (uint64_t)groups_[group].bg_inode_table * block_size_
                 + (uint64_t)index * inode_size_;
    ext2_inode in{};
    read_at(off, &in, sizeof in);
    return in;
}

void Ext2Fs::write_inode(uint32_t ino, const ext2_inode& in) {
    uint32_t group = (ino - 1) / sb_.s_inodes_per_group;
    uint32_t index = (ino - 1) % sb_.s_inodes_per_group;
    uint64_t off = (uint64_t)groups_[group].bg_inode_table * block_size_
                 + (uint64_t)index * inode_size_;
    write_at(off, &in, sizeof in);
}

uint64_t Ext2Fs::file_size(const ext2_inode& in) const {
    uint64_t sz = in.i_size;
    if (sb_.s_rev_level >= 1 && (in.i_mode & EXT2_S_IFMT) == EXT2_S_IFREG)
        sz |= (uint64_t)in.i_dir_acl << 32;
    return sz;
}

void Ext2Fs::dump_inode(std::ostream& out, uint32_t ino) const {
    ext2_inode in = read_inode(ino);
    uint32_t group = (ino - 1) / sb_.s_inodes_per_group;
    uint32_t index = (ino - 1) % sb_.s_inodes_per_group;
    uint64_t size = file_size(in);

    out << "inode " << ino << "\n\n";
    out << field("found in") << "group " << group << " index " << index
        << " (table block " << groups_[group].bg_inode_table << ")\n";
    out << field("type and perms") << type_char(in.i_mode) << " " << perms(in.i_mode) << "\n";
    out << field("size") << size << " bytes\n";
    out << field("links") << in.i_links_count << "\n";
    out << field("modified") << timestr(in.i_mtime) << "\n";

    uint64_t data_blocks = (size + block_size_ - 1) / block_size_;
    uint32_t spb = block_size_ / 512;
    out << field("i_blocks") << in.i_blocks << " sectors = " << (in.i_blocks / spb)
        << " blocks\n";
    out << field("") << "the data only needs " << data_blocks
        << ", the rest are indirect blocks\n";

    out << "\n  i_block[]:\n";
    for (int i = 0; i < 15; i++) {
        const char* what = i < EXT2_NDIR_BLOCKS ? "direct"
                         : i == EXT2_IND_BLOCK  ? "singly indirect"
                         : i == EXT2_DIND_BLOCK ? "doubly indirect" : "triply indirect";
        out << "    [" << pad(num(i), 2) << "] " << pad(what, 16) << in.i_block[i]
            << (in.i_block[i] == 0 ? "   unused" : "") << "\n";
    }

    if (data_blocks > 0 && (in.i_mode & EXT2_S_IFMT) != EXT2_S_IFLNK) {
        out << "\n  where the blocks actually are:\n";
        auto show = [&](uint64_t n) {
            uint32_t phys = block_for(in, n);
            const char* how = n < EXT2_NDIR_BLOCKS ? "direct"
                            : n < EXT2_NDIR_BLOCKS + ptrs_per_block_ ? "singly" : "doubly";
            out << "    block " << pad(num(n), 8) << " -> " << pad(num(phys), 8)
                << " (" << how << ")" << (phys == 0 ? "  hole" : "") << "\n";
        };
        for (uint64_t n = 0; n < std::min<uint64_t>(data_blocks, 3); n++) show(n);
        if (data_blocks > 6) out << "    ...\n";
        for (uint64_t n = std::max<uint64_t>(3, data_blocks - 3); n < data_blocks; n++) show(n);
    }
}

// task 2: directories

std::vector<Ext2Fs::DirEntry> Ext2Fs::read_dir(const ext2_inode& dir) const {
    if ((dir.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
        throw std::runtime_error("not a directory");

    std::vector<DirEntry> out;
    std::vector<uint8_t> buf(block_size_);

    // Directories can span blocks. lost+found here is 12 of them.
    uint64_t nblocks = (dir.i_size + block_size_ - 1) / block_size_;

    for (uint64_t n = 0; n < nblocks; n++) {
        uint32_t phys = block_for(dir, n);
        if (phys == 0) continue;
        read_at((uint64_t)phys * block_size_, buf.data(), block_size_);

        uint32_t p = 0;
        while (p + sizeof(ext2_dir_entry) <= block_size_) {
            ext2_dir_entry e{};
            std::memcpy(&e, buf.data() + p, sizeof e);

            // Check rec_len before trusting it. A 0 here means a junk block,
            // and advancing by 0 loops forever.
            if (e.rec_len < sizeof(ext2_dir_entry) || p + e.rec_len > block_size_
                || e.rec_len % 4 != 0)
                break;

            // inode 0 means the entry was deleted
            if (e.inode != 0 && e.name_len > 0
                && p + sizeof(ext2_dir_entry) + e.name_len <= block_size_) {
                out.push_back({e.inode,
                    std::string((const char*)buf.data() + p + sizeof(ext2_dir_entry), e.name_len)});
            }
            p += e.rec_len;
        }
    }
    return out;
}

void Ext2Fs::print_tree(std::ostream& out, uint32_t ino) const {
    ext2_inode root = read_inode(ino);
    out << "d " << pad("/", 24) << " inode=" << pad(num(ino), 6)
        << " size=" << file_size(root) << "\n";
    tree_rec(out, root, 1);
}

void Ext2Fs::tree_rec(std::ostream& out, const ext2_inode& dir, int depth) const {
    if (depth > 64) { out << "  too deep, giving up\n"; return; }

    for (const DirEntry& e : read_dir(dir)) {
        if (e.name == "." || e.name == "..") continue;
        std::string indent(depth * 2, ' ');

        ext2_inode in{};
        try {
            in = read_inode(e.ino);
        } catch (const std::exception& ex) {
            out << indent << "? " << e.name << " (" << ex.what() << ")\n";
            continue;
        }

        // i_mode, not the dir entry's file_type byte, which is optional.
        bool isdir = (in.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
        out << indent << type_char(in.i_mode) << " " << pad(e.name + (isdir ? "/" : ""), 24)
            << " inode=" << pad(num(e.ino), 6)
            << " size=" << pad(num(file_size(in)), 10)
            << " i_blocks=" << in.i_blocks << "\n";

        if (isdir) tree_rec(out, in, depth + 1);
    }
}

uint32_t Ext2Fs::lookup(const std::string& path) const {
    // Start at root, follow one name at a time. The kernel calls this namei.
    uint32_t ino = EXT2_ROOT_INO;
    for (const std::string& part : split_path(path)) {
        ext2_inode in = read_inode(ino);
        if ((in.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
            throw std::runtime_error("'" + part + "': not a directory");

        bool found = false;
        for (const DirEntry& e : read_dir(in)) {
            if (e.name == part) { ino = e.ino; found = true; break; }
        }
        if (!found) throw std::runtime_error("no such file or directory: " + path);
    }
    return ino;
}

// task 3: finding and reading file data
//
// Only 15 slots in i_block[], so the last three hold block numbers instead of
// data, one, two and three levels deep. With 1K blocks a block fits 256
// pointers: 12 direct, then 256, then 256*256, then 256*256*256.

uint32_t Ext2Fs::read_ptr(uint32_t blk, uint32_t idx) const {
    // Block 0 is the boot sector, so ext2 reuses it to mean "nothing here".
    if (blk == 0 || blk >= sb_.s_blocks_count) return 0;
    if (idx >= ptrs_per_block_) return 0;
    uint32_t v = 0;
    read_at((uint64_t)blk * block_size_ + (uint64_t)idx * 4, &v, 4);
    return v;
}

uint32_t Ext2Fs::block_for(const ext2_inode& in, uint64_t n) const {
    uint64_t P = ptrs_per_block_;

    if (n < EXT2_NDIR_BLOCKS) return in.i_block[n];
    n -= EXT2_NDIR_BLOCKS;

    if (n < P) return read_ptr(in.i_block[EXT2_IND_BLOCK], (uint32_t)n);
    n -= P;

    if (n < P * P) {
        // n/P picks which second level block, n%P picks the slot in it
        uint32_t mid = read_ptr(in.i_block[EXT2_DIND_BLOCK], (uint32_t)(n / P));
        return read_ptr(mid, (uint32_t)(n % P));
    }
    n -= P * P;

    if (n < P * P * P) {
        uint32_t a = read_ptr(in.i_block[EXT2_TIND_BLOCK], (uint32_t)(n / (P * P)));
        uint32_t b = read_ptr(a, (uint32_t)((n / P) % P));
        return read_ptr(b, (uint32_t)(n % P));
    }
    return 0;  // bigger than ext2 can address
}

uint64_t Ext2Fs::read_data(const ext2_inode& in, uint64_t off, void* buf, uint64_t len) const {
    uint64_t size = file_size(in);
    if (off >= size) return 0;
    if (off + len > size) len = size - off;

    uint8_t* dst = (uint8_t*)buf;
    uint64_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t phys = block_for(in, pos / block_size_);
        uint32_t boff = pos % block_size_;
        uint64_t chunk = std::min<uint64_t>(len - done, block_size_ - boff);
        if (phys == 0) std::memset(dst + done, 0, chunk);   // hole reads as zeros
        else read_at((uint64_t)phys * block_size_ + boff, dst + done, chunk);
        done += chunk;
    }
    return done;
}

uint64_t Ext2Fs::extract(const ext2_inode& in, std::ostream& out) const {
    // Short symlinks store the target text in i_block, so those bytes are a
    // string and must not go through block_for.
    if ((in.i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK && in.i_blocks == 0) {
        uint64_t len = std::min<uint64_t>(in.i_size, sizeof in.i_block);
        out.write((const char*)in.i_block, (std::streamsize)len);
        return len;
    }

    uint64_t size = file_size(in);
    std::vector<uint8_t> buf(block_size_);
    uint64_t done = 0;
    // One block at a time, so a 2.7 MB file costs 1 KB of buffer.
    while (done < size) {
        uint64_t chunk = read_data(in, done, buf.data(), block_size_);
        if (chunk == 0) break;
        out.write((const char*)buf.data(), (std::streamsize)chunk);
        if (!out) throw std::runtime_error("write failed");
        done += chunk;
    }
    return done;
}

// task 4: writing. Growing a file handles direct and singly indirect blocks
// only; past that it throws. See the README.

void Ext2Fs::write_meta() {
    // caller holds alloc_lock_
    write_at(EXT2_SUPER_OFFSET, &sb_, sizeof sb_);
    write_at((uint64_t)(sb_.s_first_data_block + 1) * block_size_,
             groups_.data(), (uint64_t)groups_.size() * sizeof(ext2_group_desc));
}

uint32_t Ext2Fs::alloc_block() {
    // The bitmap and free counters are shared, so this is the one place
    // threads queue up even when writing to different files.
    std::lock_guard<std::mutex> lk(alloc_lock_);

    std::vector<uint8_t> bm(block_size_);
    for (uint32_t g = 0; g < groups_.size(); g++) {
        if (groups_[g].bg_free_blocks_count == 0) continue;

        uint32_t first = sb_.s_first_data_block + g * sb_.s_blocks_per_group;
        uint32_t count = sb_.s_blocks_count - first;
        if (count > sb_.s_blocks_per_group) count = sb_.s_blocks_per_group;

        read_at((uint64_t)groups_[g].bg_block_bitmap * block_size_, bm.data(), block_size_);
        for (uint32_t i = 0; i < count; i++) {
            if (bm[i / 8] & (1 << (i % 8))) continue;   // already taken
            bm[i / 8] |= (uint8_t)(1 << (i % 8));
            write_at((uint64_t)groups_[g].bg_block_bitmap * block_size_, bm.data(), block_size_);
            groups_[g].bg_free_blocks_count--;
            sb_.s_free_blocks_count--;
            write_meta();
            return first + i;
        }
    }
    throw std::runtime_error("no free blocks left");
}

void Ext2Fs::free_block(uint32_t blk) {
    std::lock_guard<std::mutex> lk(alloc_lock_);
    if (blk < sb_.s_first_data_block || blk >= sb_.s_blocks_count)
        throw std::runtime_error("tried to free block " + num(blk) + " which isn't valid");

    uint32_t g = (blk - sb_.s_first_data_block) / sb_.s_blocks_per_group;
    uint32_t i = (blk - sb_.s_first_data_block) % sb_.s_blocks_per_group;

    std::vector<uint8_t> bm(block_size_);
    read_at((uint64_t)groups_[g].bg_block_bitmap * block_size_, bm.data(), block_size_);
    bm[i / 8] &= (uint8_t)~(1 << (i % 8));
    write_at((uint64_t)groups_[g].bg_block_bitmap * block_size_, bm.data(), block_size_);

    groups_[g].bg_free_blocks_count++;
    sb_.s_free_blocks_count++;
    write_meta();
}

void Ext2Fs::set_block(ext2_inode& in, uint64_t n, uint32_t phys) {
    if (n < EXT2_NDIR_BLOCKS) { in.i_block[n] = phys; return; }
    n -= EXT2_NDIR_BLOCKS;

    if (n < ptrs_per_block_) {
        if (in.i_block[EXT2_IND_BLOCK] == 0) {
            uint32_t ib = alloc_block();
            std::vector<uint8_t> zero(block_size_, 0);
            write_at((uint64_t)ib * block_size_, zero.data(), block_size_);
            in.i_block[EXT2_IND_BLOCK] = ib;
            in.i_blocks += block_size_ / 512;   // indirect blocks count too
        }
        write_at((uint64_t)in.i_block[EXT2_IND_BLOCK] * block_size_ + n * 4, &phys, 4);
        return;
    }
    throw std::runtime_error("file would need doubly indirect blocks to grow, not supported");
}

// Writes len bytes at byte pos, allocating blocks where there are none yet.
uint64_t Ext2Fs::write_range(ext2_inode& in, uint64_t pos, const void* data, uint64_t len) {
    if (len == 0) return 0;

    // Check the whole write fits before allocating anything. Allocating first
    // and then throwing leaves blocks marked used with nothing pointing at
    // them, which e2fsck reports as block bitmap differences.
    uint64_t last = (pos + len - 1) / block_size_;
    if (last >= EXT2_NDIR_BLOCKS + ptrs_per_block_)
        throw std::runtime_error("write would need doubly indirect blocks, not supported");

    const uint8_t* src = (const uint8_t*)data;
    uint64_t done = 0;
    std::vector<uint32_t> mine;   // allocated here, freed again on failure

    try {
        while (done < len) {
            uint64_t at = pos + done;
            uint64_t n = at / block_size_;
            uint32_t boff = at % block_size_;

            uint32_t phys = block_for(in, n);
            if (phys == 0) {
                phys = alloc_block();
                mine.push_back(phys);
                // Zero it, or old contents show through in the unwritten part.
                std::vector<uint8_t> zero(block_size_, 0);
                write_at((uint64_t)phys * block_size_, zero.data(), block_size_);
                set_block(in, n, phys);
                in.i_blocks += block_size_ / 512;
            }

            uint64_t chunk = std::min<uint64_t>(len - done, block_size_ - boff);
            write_at((uint64_t)phys * block_size_ + boff, src + done, chunk);
            done += chunk;
        }
    } catch (...) {
        for (uint32_t b : mine) free_block(b);
        throw;
    }
    return done;
}

uint64_t Ext2Fs::append(uint32_t ino, const void* data, uint64_t len) {
    // One lock per inode, so different files don't block each other.
    std::lock_guard<std::mutex> lk(inode_locks_[ino]);

    ext2_inode in = read_inode(ino);
    if ((in.i_mode & EXT2_S_IFMT) != EXT2_S_IFREG)
        throw std::runtime_error("can only append to regular files");

    uint64_t size = file_size(in);
    write_range(in, size, data, len);

    in.i_size = (uint32_t)(size + len);
    in.i_mtime = (uint32_t)std::time(nullptr);
    write_inode(ino, in);
    sync();
    return size + len;
}

uint64_t Ext2Fs::overwrite(uint32_t ino, const void* data, uint64_t len) {
    std::lock_guard<std::mutex> lk(inode_locks_[ino]);

    ext2_inode in = read_inode(ino);
    if ((in.i_mode & EXT2_S_IFMT) != EXT2_S_IFREG)
        throw std::runtime_error("can only overwrite regular files");

    uint64_t old_size = file_size(in);
    uint64_t old_blocks = (old_size + block_size_ - 1) / block_size_;
    uint64_t new_blocks = (len + block_size_ - 1) / block_size_;

    if (old_blocks > EXT2_NDIR_BLOCKS + ptrs_per_block_)
        throw std::runtime_error("file is big enough to use doubly indirect blocks, "
                                 "shrinking those isn't supported");

    write_range(in, 0, data, len);

    for (uint64_t n = new_blocks; n < old_blocks; n++) {
        uint32_t phys = block_for(in, n);
        if (phys == 0) continue;
        free_block(phys);
        set_block(in, n, 0);
        in.i_blocks -= block_size_ / 512;
    }
    if (new_blocks <= EXT2_NDIR_BLOCKS && in.i_block[EXT2_IND_BLOCK] != 0) {
        free_block(in.i_block[EXT2_IND_BLOCK]);
        in.i_block[EXT2_IND_BLOCK] = 0;
        in.i_blocks -= block_size_ / 512;
    }

    in.i_size = (uint32_t)len;
    in.i_mtime = (uint32_t)std::time(nullptr);
    write_inode(ino, in);
    sync();
    return len;
}
