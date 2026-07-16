#include <miniOS/io.h>
#include <miniOS/drivers/block_layer.h>
#include <miniOS/drivers/ata.h>
#include <miniOS/mm/pmm.h>
#include <miniOS/mm/vmm.h>
#include <miniOS/mm/heap.h>
#include <miniOS/arch/x86_64/spinlock.h>
#include <string.h> // For memcpy

#define BLOCK_SIZE 512
#define CACHE_SIZE_BLOCKS 64 // Cache size of 64 * 512 = 32 KB

typedef struct {
    bool valid;
    bool referenced;
    uint64_t lba;
    uint8_t data[BLOCK_SIZE];
} cache_entry_t;

typedef struct {
    cache_entry_t *entries;
    uint32_t clock_hand;
} block_cache_t;

static block_cache_t g_block_cache;
static spinlock_t g_cache_lock;

// Must be called with g_cache_lock held
static cache_entry_t* block_cache_lookup_locked(uint64_t lba) {
    for (int i = 0; i < CACHE_SIZE_BLOCKS; i++) {
        if (g_block_cache.entries[i].valid && g_block_cache.entries[i].lba == lba) {
            return &g_block_cache.entries[i];
        }
    }
    return NULL;
}

// Must be called with g_cache_lock held
static cache_entry_t* block_cache_evict_locked(void) {
    while (true) {
        cache_entry_t *entry = &g_block_cache.entries[g_block_cache.clock_hand];

        // Advance the clock hand for the next eviction
        g_block_cache.clock_hand = (g_block_cache.clock_hand + 1) % CACHE_SIZE_BLOCKS;

        if (!entry->valid) {
            return entry; // Found an unused slot
        }

        if (entry->referenced) {
            entry->referenced = false; // Give it a second chance
        } else {
            // Found a victim
            return entry;
        }
    }
}

void block_layer_init(void) {
    printk("block_layer: Initializing.");

    uint32_t cache_size_bytes = CACHE_SIZE_BLOCKS * sizeof(cache_entry_t);
    g_block_cache.entries = (cache_entry_t*) kmalloc(cache_size_bytes);
    if (!g_block_cache.entries) {
        printk("block_layer: FAILED to allocate memory for cache!");
        return;
    }
    
    for (int i = 0; i < CACHE_SIZE_BLOCKS; i++) {
        g_block_cache.entries[i].valid = false;
    }
    g_block_cache.clock_hand = 0;
    // No spinlock_init is needed for a static global, it's zero-initialized.

    printk("Block cache: Initialized.");
}

int block_read(uint64_t lba, uint16_t count, void *buf) {
    // This simple cache only supports single block operations for now.
    if (count != 1) {
        // Fallback for multi-block reads
        return ata_read_sectors(lba, count, buf);
    }

    unsigned long flags;
    spinlock_irqsave(&g_cache_lock, &flags);

    cache_entry_t* entry = block_cache_lookup_locked(lba);
    if (entry) {
        // Cache hit
        memcpy(buf, entry->data, BLOCK_SIZE);
        entry->referenced = true;
        spinlock_irqrestore(&g_cache_lock, flags);
        return 0; // Success
    }

    // Cache miss
    spinlock_irqrestore(&g_cache_lock, flags);

    // Read from disk. Note: this buffer should be DMA-safe if not using PIO.
    // For this simple case we assume ata_read_sectors handles it.
    char temp_buf[BLOCK_SIZE];
    int result = ata_read_sectors(lba, 1, temp_buf);
    if (result != 0) {
        return result; // Read failed
    }

    // Copy to user buffer
    memcpy(buf, temp_buf, BLOCK_SIZE);

    // Add to cache
    spinlock_irqsave(&g_cache_lock, &flags);
    entry = block_cache_evict_locked();
    memcpy(entry->data, temp_buf, BLOCK_SIZE);
    entry->lba = lba;
    entry->valid = true;
    entry->referenced = true;
    spinlock_irqrestore(&g_cache_lock, flags);

    return 0; // Success
}

int block_write(uint64_t lba, uint16_t count, const void *buf) {
    // This simple cache only supports single block operations for now.
    if (count != 1) {
        // Fallback for multi-block writes
        return ata_write_sectors(lba, count, buf);
    }
    
    // Write-through: write to disk first.
    int result = ata_write_sectors(lba, 1, buf);
    if (result != 0) {
        return result;
    }

    // Now update the cache if the block exists there.
    unsigned long flags;
    spinlock_irqsave(&g_cache_lock, &flags);

    cache_entry_t* entry = block_cache_lookup_locked(lba);
    if (entry) {
        memcpy(entry->data, buf, BLOCK_SIZE);
        entry->referenced = true;
    }
    
    spinlock_irqrestore(&g_cache_lock, flags);

    return 0; // Success
}
