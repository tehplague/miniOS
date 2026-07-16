#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define PART_OFFSET (135168 * 512)

typedef struct
{
    uint32_t sectors_per_fat;
    uint16_t flags;
    uint16_t version;
    uint32_t root_directory_cluster;
    uint16_t fsinfo_sector;
    uint16_t backup_boot_sector;
    uint8_t reserved[12];
    uint8_t drive_number;
    uint8_t reserved2;
    uint8_t signature;
    uint32_t volume_id;
    char volume_label[11];
    char system_id[8];
    uint8_t boot_code[420];
    uint16_t boot_signature;
} __attribute__((packed)) fat32_ebr_t;

typedef struct
{
    uint8_t jump[3];
    uint8_t oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t fat_count;
    uint16_t root_entry_count;
    uint16_t total_sector_count;
    uint8_t media_descriptor_type;
    uint16_t sectors_per_fat;
    uint16_t sectors_per_track;
    uint16_t head_count;
    uint32_t hidden_sector_count;
    uint32_t large_sector_count;

    fat32_ebr_t ebr;
}  __attribute__((packed)) fat32_bpb_t;

typedef struct
{
    uint32_t lead_signature;
    uint8_t reserved[480];
    uint32_t signature;
    uint32_t last_free_cluster_count;
    uint32_t last_allocated_cluster;
    uint8_t reserved2[12];
    uint32_t trail_signature;
} __attribute__((packed)) fat32_fsinfo_t;

typedef struct
{
    char name[8];
    char extension[3];
    uint8_t attributes;
    uint8_t lowercase;
    uint8_t ctime_ms;
    uint16_t ctime;
    uint16_t cdate;
    uint16_t adate;
    uint16_t cluster_high;
    uint16_t mtime;
    uint16_t mdate;
    uint16_t cluster_low;
    uint32_t filesize;
} __attribute__((packed)) fat32_dir_entry_t;

typedef struct
{
    uint8_t order;
    uint16_t name1[5];
    uint8_t attribute;
    uint8_t type;
    uint8_t checksum;
    uint16_t name2[6];
    uint16_t zero;
    uint16_t name3[2];
} __attribute__((packed)) fat32_long_entry_t;

int fd = -1;
void *data = NULL;

uint32_t read_fat_entry(void *part_start, fat32_bpb_t *bpb, uint32_t index)
{
    uint32_t *fat = part_start + (bpb->reserved_sector_count * bpb->bytes_per_sector);
    uint32_t entry = fat[index];
    return entry & 0x0FFFFFFF;
}

static void lfn_to_ascii(const uint16_t *buf, int count, char *out, size_t out_size)
{
    size_t n = 0;
    for (int j = 0; j < count * 13 && n + 1 < out_size; j++) {
        uint16_t c = buf[j];
        if (c == 0x0000 || c == 0xFFFF)
            break;
        out[n++] = c < 0x80 ? (char)c : '?';
    }
    out[n] = '\0';
}

static uint8_t lfn_checksum(const char *short_name)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + (uint8_t)short_name[i];
    return sum;
}

void read_directory(void *part_start, uint32_t cluster, fat32_bpb_t *bpb)
{
    fat32_ebr_t *ebr = &bpb->ebr;
    uint32_t sectors_per_fat = bpb->sectors_per_fat == 0
        ? ebr->sectors_per_fat : bpb->sectors_per_fat;
    uint32_t root_dir_sectors = ((uint32_t)bpb->root_entry_count * 32
        + bpb->bytes_per_sector - 1) / bpb->bytes_per_sector;
    uint32_t first_data_sector = bpb->reserved_sector_count
        + (bpb->fat_count * sectors_per_fat) + root_dir_sectors;
    uint32_t cluster_size = (uint32_t)bpb->sectors_per_cluster * bpb->bytes_per_sector;
    uint32_t entries_per_cluster = cluster_size / sizeof(fat32_dir_entry_t);

    /* LFN accumulator: up to 20 LFN entries × 13 UCS-2 chars */
    uint16_t lfn_buf[20 * 13 + 1];
    int      lfn_count = 0;
    uint8_t  lfn_csum  = 0;

    for (uint32_t cur = cluster; cur >= 2 && cur < 0x0FFFFFF8;
         cur = read_fat_entry(part_start, bpb, cur)) {
        uint32_t off = ((cur - 2) * bpb->sectors_per_cluster + first_data_sector)
                       * bpb->bytes_per_sector;
        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)((uint8_t *)part_start + off);

        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            fat32_dir_entry_t *e = &entries[i];

            if ((uint8_t)e->name[0] == 0x00)
                return; /* end of directory */
            if ((uint8_t)e->name[0] == 0xE5) {
                lfn_count = 0;
                continue; /* deleted */
            }

            if (e->attributes == 0x0F) {
                /* LFN entry — accumulate name fragments in order */
                fat32_long_entry_t *lfn = (fat32_long_entry_t *)e;
                int seq = (lfn->order & 0x3F) - 1; /* 0-based slot index */
                if (lfn->order & 0x40) {
                    /* last LFN entry (highest seq, first on disk) — reset */
                    lfn_count = seq + 1;
                    lfn_csum  = lfn->checksum;
                    memset(lfn_buf, 0xFF, sizeof(lfn_buf));
                    lfn_buf[lfn_count * 13] = 0;
                }
                uint16_t *dst = lfn_buf + seq * 13;
                memcpy(dst,      lfn->name1, 5 * sizeof(uint16_t));
                memcpy(dst + 5,  lfn->name2, 6 * sizeof(uint16_t));
                memcpy(dst + 11, lfn->name3, 2 * sizeof(uint16_t));
                continue;
            }

            if (e->attributes & 0x08) { /* volume label */
                lfn_count = 0;
                continue;
            }

            int is_dir = (e->attributes & 0x10) != 0;

            if (lfn_count > 0) {
                char raw[11];
                memcpy(raw,     e->name,      8);
                memcpy(raw + 8, e->extension, 3);
                if (lfn_checksum(raw) == lfn_csum) {
                    char name[20 * 13 + 1];
                    lfn_to_ascii(lfn_buf, lfn_count, name, sizeof(name));
                    printf("  %s %s\n", is_dir ? "[DIR]" : "     ", name);
                    lfn_count = 0;
                    continue;
                }
                lfn_count = 0; /* checksum mismatch — fall through to 8.3 */
            }

            /* 8.3 short name: 8 chars name + optional dot + 3 chars ext, space-padded */
            char name[13];
            int  n = 0;
            for (int j = 0; j < 8 && e->name[j] != ' '; j++)
                name[n++] = e->name[j];
            if (e->extension[0] != ' ') {
                name[n++] = '.';
                for (int j = 0; j < 3 && e->extension[j] != ' '; j++)
                    name[n++] = e->extension[j];
            }
            name[n] = '\0';
            printf("  %s %s\n", is_dir ? "[DIR]" : "     ", name);
        }
    }
}

int main(void) {
    printf("Opening disk image...\n");
    fd = open("../dist/x86_64/disk.img", O_RDONLY);
    if (fd < 0)
    {
        perror("open");
        exit(1);
    }

    struct stat file_stat;
    if (fstat(fd, &file_stat) < 0)
    {
        close(fd);
        perror("fstat");
        exit(1);
    }

    printf("Mapping disk image...\n");
    data = mmap(NULL, file_stat.st_size - PART_OFFSET, PROT_READ, MAP_PRIVATE, fd, PART_OFFSET);
    if (data == MAP_FAILED)
    {
        close(fd);
        perror("mmap");
        exit(1);
    }

    printf("disk image mapped at %p\n", data);

    fat32_bpb_t *bpb = data;
    fat32_ebr_t *ebr = &bpb->ebr;

    uint32_t total_sectors = bpb->total_sector_count == 0 ? bpb->large_sector_count : bpb->total_sector_count;
    uint32_t sectors_per_fat = bpb->sectors_per_fat == 0 ? ebr->sectors_per_fat : bpb->sectors_per_fat;
    uint16_t root_dir_sectors = (bpb->root_entry_count * 32 + bpb->bytes_per_sector - 1) / bpb->bytes_per_sector;
    uint32_t data_sectors = total_sectors - (bpb->reserved_sector_count + (bpb->fat_count * sectors_per_fat) + root_dir_sectors);
    printf("FAT32 BPB:\n");
    printf("  Media descriptor type: 0x%02x\n", bpb->media_descriptor_type);
    printf("  Total sectors: %u\n", total_sectors);
    printf("  Sectors per FAT: %u\n", sectors_per_fat);
    printf("  Sectors per cluster: %u\n", bpb->sectors_per_cluster);
    printf("  Head count: %u\n", bpb->head_count);
    printf("  Number of FATs: %u\n", bpb->fat_count);
    printf("  Hidden sector count: %u\n", bpb->hidden_sector_count);
    printf("  Root entry count: %u\n", bpb->root_entry_count);
    printf("  Root directory sector count (0 for FAT32): %u\n", root_dir_sectors);
    printf("  Root directory cluster count: %u\n", root_dir_sectors / bpb->sectors_per_cluster);
    printf("  Reserved sector count: %u\n", bpb->reserved_sector_count);
    printf("  First data sector: %u\n", bpb->reserved_sector_count + (bpb->fat_count * sectors_per_fat) + root_dir_sectors);
    printf("  Data sector count: %u\n", data_sectors);
    printf("  Total cluster count: %u\n", data_sectors / bpb->sectors_per_cluster);

    char volume_label[11] = {0};
    memcpy(volume_label, ebr->volume_label, sizeof(ebr->volume_label));
    volume_label[sizeof(ebr->volume_label) - 1] = '\0';
    char system_id[8] = {0};
    memcpy(system_id, ebr->system_id, sizeof(ebr->system_id));
    system_id[sizeof(ebr->system_id) - 1] = '\0';

    printf("FAT32 EBR:\n");
    printf("  Signature: 0x%04x\n", ebr->signature);
    printf("  Volume ID: 0x%08x\n", ebr->volume_id);
    printf("  Volume label: %s\n", volume_label);
    printf("  System ID: %s\n", system_id);

    fat32_fsinfo_t *fsinfo = (fat32_fsinfo_t *)(data + ebr->fsinfo_sector * 512);
    printf("FAT32 FSINFO:\n");
    printf("  Lead signature: 0x%08x\n", fsinfo->lead_signature);
    printf("  Signature: 0x%08x\n", fsinfo->signature);
    printf("  Last free cluster count: %u\n", fsinfo->last_free_cluster_count);
    printf("  Last allocated cluster: %u\n", fsinfo->last_allocated_cluster);
    printf("  Trail signature: 0x%08x\n", fsinfo->trail_signature);


    printf("Reading root directory:\n");
    read_directory(data, ebr->root_directory_cluster, bpb);

    munmap(data, 0);
    close(fd);
    return 0;
}
