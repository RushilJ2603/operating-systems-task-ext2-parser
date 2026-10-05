// Command line front end. Files can be given as a path or an inode number.

#include "ext2fs.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int usage(const char* prog) {
    std::cerr
        << "usage: " << prog << " <image> <command> [args]\n\n"
        << "reading:\n"
        << "  super                     superblock\n"
        << "  groups                    block group descriptors\n"
        << "  tree                      whole directory tree\n"
        << "  inode <path|num>          one inode and where its blocks are\n"
        << "  cat <path|num>            file contents to stdout\n"
        << "  extract <path|num> <out>  file contents to a file\n\n"
        << "writing (opens the image read-write, back it up first):\n"
        << "  append <path|num> <text>\n"
        << "  overwrite <path|num> <text>\n\n"
        << "concurrency:\n"
        << "  threads <n>               n threads reading at once, no locks\n"
        << "  race <n> <path|num>...    n threads appending, one lock per inode\n";
    return 2;
}

// a leading digit means it's an inode number, otherwise a path
uint32_t resolve(const Ext2Fs& fs, const std::string& s) {
    if (!s.empty() && s[0] >= '0' && s[0] <= '9') return (uint32_t)std::strtoul(s.c_str(), nullptr, 10);
    return fs.lookup(s);
}

// just something to compare between threads
uint64_t checksum(const uint8_t* p, uint64_t n, uint64_t sum = 0) {
    for (uint64_t i = 0; i < n; i++) sum = sum * 131 + p[i];
    return sum;
}

// checksum every file in the tree
uint64_t checksum_all(const Ext2Fs& fs, uint32_t ino, uint64_t& bytes) {
    uint64_t sum = 0;
    std::vector<uint8_t> buf(fs.block_size());
    ext2_inode dir = fs.read_inode(ino);
    for (const auto& e : fs.read_dir(dir)) {
        if (e.name == "." || e.name == "..") continue;
        ext2_inode in = fs.read_inode(e.ino);
        if ((in.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
            sum += checksum_all(fs, e.ino, bytes);
        } else {
            uint64_t size = fs.file_size(in), off = 0;
            while (off < size) {
                uint64_t got = fs.read_data(in, off, buf.data(), buf.size());
                if (got == 0) break;
                sum = checksum(buf.data(), got, sum);
                bytes += got;
                off += got;
            }
        }
    }
    return sum;
}

// Bonus 1: concurrent reads, no locks. The image is mmap'd, so a read is a
// memcpy with no shared file position to protect.
int cmd_threads(const std::string& image, int n) {
    Ext2Fs fs(image);

    uint64_t ref_bytes = 0;
    uint64_t ref = checksum_all(fs, EXT2_ROOT_INO, ref_bytes);
    std::cout << "single thread: checksum " << ref << " over " << ref_bytes << " bytes\n";

    std::vector<uint64_t> sums(n), byte_counts(n);
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> ts;
    for (int i = 0; i < n; i++) {
        ts.emplace_back([&fs, &sums, &byte_counts, i] {
            uint64_t b = 0;
            sums[i] = checksum_all(fs, EXT2_ROOT_INO, b);
            byte_counts[i] = b;
        });
    }
    for (auto& t : ts) t.join();
    auto t1 = std::chrono::steady_clock::now();

    bool ok = true;
    uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        if (sums[i] != ref || byte_counts[i] != ref_bytes) {
            std::cout << "thread " << i << " disagreed: " << sums[i] << "\n";
            ok = false;
        }
        total += byte_counts[i];
    }

    double secs = std::chrono::duration<double>(t1 - t0).count();
    std::cout << n << " threads: " << (ok ? "all got the same checksum" : "MISMATCH") << "\n";
    std::cout << "read " << total << " bytes in " << secs << "s = "
              << (total / secs / (1024.0 * 1024.0)) << " MB/s\n";
    return ok ? 0 : 1;
}

// Bonus 2: concurrent appends, one lock per inode rather than one for the
// whole disk. Only the block allocator is still shared.
int cmd_race(const std::string& image, int nthreads, const std::vector<std::string>& targets) {
    Ext2Fs fs(image, true);

    std::vector<uint32_t> inos;
    for (const auto& t : targets) inos.push_back(resolve(fs, t));

    std::vector<uint64_t> before;
    for (uint32_t ino : inos) before.push_back(fs.file_size(fs.read_inode(ino)));

    const int per_thread = 20;
    std::atomic<int> failures{0};

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> ts;
    for (int i = 0; i < nthreads; i++) {
        ts.emplace_back([&fs, &inos, &failures, i, per_thread] {
            uint32_t ino = inos[i % inos.size()];
            for (int k = 0; k < per_thread; k++) {
                std::string line = "thread " + std::to_string(i) + " line " + std::to_string(k) + "\n";
                try {
                    fs.append(ino, line.data(), line.size());
                } catch (const std::exception& e) {
                    failures++;
                }
            }
        });
    }
    for (auto& t : ts) t.join();
    auto t1 = std::chrono::steady_clock::now();

    std::cout << nthreads << " threads x " << per_thread << " appends, "
              << std::chrono::duration<double>(t1 - t0).count() << "s, "
              << failures.load() << " failures\n\n";

    // Sizes must add up exactly, or writes were lost.
    bool ok = true;
    for (size_t j = 0; j < inos.size(); j++) {
        uint64_t now = fs.file_size(fs.read_inode(inos[j]));
        int threads_here = 0;
        uint64_t expected_added = 0;
        for (int i = 0; i < nthreads; i++) {
            if (inos[i % inos.size()] == inos[j]) {
                threads_here++;
                for (int k = 0; k < per_thread; k++)
                    expected_added += ("thread " + std::to_string(i) + " line "
                                       + std::to_string(k) + "\n").size();
            }
        }
        uint64_t expected = before[j] + expected_added;
        std::cout << "inode " << inos[j] << ": " << before[j] << " -> " << now
                  << " bytes (expected " << expected << ", " << threads_here << " threads) "
                  << (now == expected ? "ok" : "WRONG") << "\n";
        if (now != expected) ok = false;
    }
    std::cout << "\nnow run: e2fsck -fn " << image << "\n";
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage(argv[0]);
    std::string image = argv[1], cmd = argv[2];

    try {
        if (cmd == "threads") {
            if (argc < 4) return usage(argv[0]);
            return cmd_threads(image, std::atoi(argv[3]));
        }
        if (cmd == "race") {
            if (argc < 5) return usage(argv[0]);
            std::vector<std::string> targets(argv + 4, argv + argc);
            return cmd_race(image, std::atoi(argv[3]), targets);
        }

        bool writing = (cmd == "append" || cmd == "overwrite");
        Ext2Fs fs(image, writing);

        if (cmd == "super")  { fs.dump_superblock(std::cout); return 0; }
        if (cmd == "groups") { fs.dump_groups(std::cout);     return 0; }
        if (cmd == "tree")   { fs.print_tree(std::cout);      return 0; }

        if (cmd == "inode") {
            if (argc < 4) return usage(argv[0]);
            fs.dump_inode(std::cout, resolve(fs, argv[3]));
            return 0;
        }
        if (cmd == "cat") {
            if (argc < 4) return usage(argv[0]);
            ext2_inode in = fs.read_inode(resolve(fs, argv[3]));
            fs.extract(in, std::cout);
            std::cout.flush();
            return 0;
        }
        if (cmd == "extract") {
            if (argc < 5) return usage(argv[0]);
            ext2_inode in = fs.read_inode(resolve(fs, argv[3]));
            // binary, or newlines get rewritten and corrupt the file
            std::ofstream out(argv[4], std::ios::binary);
            if (!out) { std::cerr << "can't create " << argv[4] << "\n"; return 1; }
            uint64_t n = fs.extract(in, out);
            out.close();
            std::cerr << "wrote " << n << " bytes to " << argv[4] << "\n";
            return 0;
        }
        if (cmd == "append") {
            if (argc < 5) return usage(argv[0]);
            uint32_t ino = resolve(fs, argv[3]);
            std::string text = argv[4];
            uint64_t size = fs.append(ino, text.data(), text.size());
            std::cerr << "inode " << ino << " is now " << size << " bytes\n";
            return 0;
        }
        if (cmd == "overwrite") {
            if (argc < 5) return usage(argv[0]);
            uint32_t ino = resolve(fs, argv[3]);
            std::string text = argv[4];
            uint64_t size = fs.overwrite(ino, text.data(), text.size());
            std::cerr << "inode " << ino << " is now " << size << " bytes\n";
            return 0;
        }

        std::cerr << "don't know the command '" << cmd << "'\n";
        return usage(argv[0]);

    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
