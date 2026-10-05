#include <cstdio>
#include <cstdint>
#include <iostream>
#include <sys/mmap.h>
#include <sys/stat.h>
#include <fcntl.h>
struct ext2_superblock {
    uint32_t s_inodes_count;      // Total inodes
    uint32_t s_blocks_count;      // Total blocks
    uint32_t s_r_blocks_count;    // Reserved blocks
    uint32_t s_free_blocks_count; // Free blocks
    uint32_t s_free_inodes_count; // Free inodes
    uint32_t s_first_data_block;  // First Data Block
    uint32_t s_log_block_size;    // Block size
    uint32_t s_log_frag_size;     // Fragment size
    uint32_t s_blocks_per_group;  // Blocks per group
    uint32_t s_frags_per_group;   // Fragments per group
    uint32_t s_inodes_per_group;  // Inodes per group
    uint32_t s_mtime;             // Mount time
    uint32_t s_wtime;             // Write time
    uint16_t s_mnt_count;         // Mount count
    uint16_t s_max_mnt_count;     // Max mount count
    uint16_t s_magic;             // Magic signature (MUST BE 0xEF53)
    uint16_t s_state;             // File system state
    uint16_t s_errors;            // Error behavior
    uint16_t s_minor_rev_level;   // Minor revision level
    uint32_t s_lastcheck;         // Time of last check
    uint32_t s_checkinterval;     // Max time between checks
    uint32_t s_creator_os;        // Creator OS
    uint32_t s_rev_level;         // Revision level
    uint16_t s_def_resuid;        // Default uid for reserved blocks
    uint16_t s_def_resgid;        // Default gid for reserved blocks
    uint32_t s_first_ino;         // First non-reserved inode
    uint16_t s_inode_size;        // Size of inode structure
};

struct ext2_block_group_descriptor {
    uint32_t bg_block_bitmap;      // Block number of the block bitmap
    uint32_t bg_inode_bitmap;      // Block number of the inode bitmap
    uint32_t bg_inode_table;       // Block number of the inode table
    uint16_t bg_free_blocks_count; // Free blocks count
    uint16_t bg_free_inodes_count; // Free inodes count
    uint16_t bg_used_dirs_count;   // Directories count
    uint16_t bg_pad;
    uint32_t bg_reserved[3];
};

struct ext2_inode {
    uint16_t i_mode;        // File mode
    uint16_t i_uid;         // Low 16 bits of Owner Uid
    uint32_t i_size;        // Size in bytes
    uint32_t i_atime;       // Access time
    uint32_t i_ctime;       // Creation time
    uint32_t i_mtime;       // Modification time
    uint32_t i_dtime;       // Deletion Time
    uint16_t i_gid;         // Low 16 bits of Group Id
    uint16_t i_links_count; // Links count
    uint32_t i_blocks;      // Blocks count
    uint32_t i_flags;       // File flags
    uint32_t i_osd1;        // OS dependent 1
    uint32_t i_block[15];   // Pointers to data blocks!
    uint32_t i_generation;  // File version (for NFS)
    uint32_t i_file_acl;    // File ACL
    uint32_t i_dir_acl;     // Directory ACL
    uint32_t i_faddr;       // Fragment address
    uint32_t i_osd2[3];     // OS dependent 2
};

struct ext2_dir_entry {
    uint32_t inode;       // Inode number of the file
    uint16_t rec_len;     // Length of this entire directory record
    uint8_t  name_len;    // Length of the string name
    uint8_t  file_type;   // File type (1 for regular file, 2 for directory)
    char     name[255];   // The actual string name
};


void traverse_directory(FILE* file, uint32_t inode_num, ext2_superblock &super, ext2_block_group_descriptor &descriptor, int depth = 0){

    uint32_t block_size = 1024<<super.s_log_block_size;
    // if(block_size==1024) fseek(file, block_size*2, SEEK_SET);
    // else fseek(file, block_size, SEEK_SET);

    // uint32_t root_inode_offset = descriptor.bg_inode_table*block_size + 1*(super.s_inode_size);
    // ext2_inode root_inode;
    // fseek(file, root_inode_offset, SEEK_SET);
    // fread(&root_inode, sizeof(ext2_inode), 1, file);

    // fseek(file, root_inode.i_block[0]*block_size, SEEK_SET);

    ext2_inode cur_inode;

    fseek(file, descriptor.bg_inode_table*block_size +  (inode_num-1)*super.s_inode_size, SEEK_SET);
    fread(&cur_inode, sizeof(ext2_inode), 1, file);
    fseek(file, cur_inode.i_block[0]*block_size, SEEK_SET);

    uint32_t bytes_read = 0;
    
    
    while(bytes_read < block_size) {
        for(int i = 0; i < depth; i++) {
        std::cout << "\t";
        }
        ext2_dir_entry entry;
        fread(&entry, sizeof(ext2_dir_entry), 1, file);
        std::string name = std::string(entry.name, entry.name_len);
        long current_pos = ftell(file);
        std::cout << std::dec << name;
        if (entry.file_type == 2) {
            std::cout << "/\n"; // Add a trailing slash so we know it's a folder
            
            if (name != "." && name != "..") {
                // Dive deeper into this subdirectory!
                uint32_t bgd_table_block = (block_size == 1024) ? 2 : 1;
                uint32_t group_index = (entry.inode-1)/super.s_inodes_per_group;
                uint32_t local_inode_index = (entry.inode)%super.s_inodes_per_group;
                fseek(file, bgd_table_block * block_size + (group_index * sizeof(ext2_block_group_descriptor)), SEEK_SET);

                // 2. Read the specific descriptor for our target group!
                ext2_block_group_descriptor target_descriptor;
                fread(&target_descriptor, sizeof(ext2_block_group_descriptor), 1, file);

                // 3. Now calculate the Inode offset using the TARGET descriptor and the LOCAL index!
                //fseek(file, target_descriptor.bg_inode_table * block_size + (local_inode_index * super.s_inode_size), SEEK_SET);
                traverse_directory(file, local_inode_index, super, target_descriptor, depth + 1);
            }
        } else {
            std::cout << '\n';
        }
        
        bytes_read += entry.rec_len;
        fseek(file, current_pos + entry.rec_len - sizeof(ext2_dir_entry), SEEK_SET);
    }

    


}

int main(int argc, char* argv[]){

    // Fixed typo: the image file is actually "disk-backpup.img"
    FILE* file = fopen("Artifacts/disk-backpup.img", "r+b");
    if (file == NULL){
        std::cout << "no file found\n"; // Added std::
        return 1;
    }

    fseek(file, 1024, SEEK_SET);

    // In C++, you can drop the 'struct' keyword here. It works perfectly!
    ext2_superblock super;
    fread(&super, sizeof(ext2_superblock), 1, file);

    // Added std:: and printed the signature in hexadecimal format!
    std::cout << "Magic Signature: " << std::hex << super.s_magic << "\n";

    uint32_t block_size = 1024<<super.s_log_block_size;
    if(block_size==1024) fseek(file, block_size*2, SEEK_SET);
    else fseek(file, block_size, SEEK_SET);

    ext2_block_group_descriptor descriptor;
    fread(&descriptor, sizeof(ext2_block_group_descriptor), 1, file);

    //std::cout<<descriptor.bg_inode_table;

    uint32_t root_inode_offset = descriptor.bg_inode_table*block_size + 1*(super.s_inode_size);
    fseek(file, root_inode_offset, SEEK_SET);
    ext2_inode root_inode;

    fread(&root_inode, sizeof(ext2_inode), 1, file);

    fseek(file, root_inode.i_block[0]*block_size, SEEK_SET);

    uint32_t bytes_read = 0;
    //traverse_directory(file, 2, super, descriptor);
    // while(bytes_read<block_size){

    //     ext2_dir_entry entry;
    //     fread(&entry, sizeof(ext2_dir_entry), 1, file);
    //     std::string name = std::string(entry.name, entry.name_len);
    //     //std::cout<<std::dec<<name<<" "<<entry.inode<<'\n';
    //     bytes_read+=entry.rec_len;
    //     fseek(file, entry.rec_len - sizeof(ext2_dir_entry), SEEK_CUR);

    // }

    fseek(file, descriptor.bg_inode_table*block_size + 11*super.s_inode_size, SEEK_SET);
    ext2_inode read_this;
    fread(&read_this, sizeof(ext2_inode), 1, file);

    fseek(file, read_this.i_block[0]*block_size, SEEK_SET);



    // char buffer[read_this.i_size+1];
    // fread(buffer, 1, read_this.i_size, file );
    // for(int i = 0; i<=read_this.i_size; i++){
    //     std::cout<<buffer[i];
    // }

    

    //std::cout<<std::dec<<root_inode.i_block[0];
    
    ext2_inode updated;
    fseek(file , descriptor.bg_inode_table*block_size + 11*super.s_inode_size, SEEK_SET);
    fread(&updated, sizeof(ext2_inode), 1, file);

    fseek(file, updated.i_block[0]*block_size + updated.i_size, SEEK_SET);
    std::string append = "\nthis was appended by C++";
    fwrite(append.c_str(), sizeof(char), append.size(),  file);

    updated.i_size+=append.size();

    fseek(file , descriptor.bg_inode_table*block_size + 11*super.s_inode_size, SEEK_SET);
    fwrite(&updated, sizeof(ext2_inode), 1, file);
    fseek(file, read_this.i_block[0]*block_size, SEEK_SET);

    char buffer[updated.i_size+1];
    buffer[updated.i_size] = '\0'; // Properly null terminate it!
    fread(buffer, 1, updated.i_size, file );
    std::cout << buffer << "\n";
    
    fclose(file);

    return 0;
}