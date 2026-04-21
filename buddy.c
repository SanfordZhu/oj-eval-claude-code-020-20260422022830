#include "buddy.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define NULL ((void *)0)
#define MAX_RANK 16
#define PAGE_SIZE 4096

// Structure for a free block
typedef struct FreeBlock {
    struct FreeBlock *next;
} FreeBlock;

// Global variables
static void *memory_base = NULL;
static int total_pages = 0;
static FreeBlock *free_lists[MAX_RANK + 1]; // Index 1..MAX_RANK
static char allocated_ranks[32768]; // Max 32768 pages, track rank of allocated blocks

// Helper functions
static int get_block_size(int rank) {
    return PAGE_SIZE * (1 << (rank - 1));
}

static int get_max_rank_for_pages(int pages) {
    int rank = 1;
    while ((1 << (rank - 1)) <= pages && rank <= MAX_RANK) {
        rank++;
    }
    return rank - 1;
}

static void *get_buddy(void *block, int rank) {
    uintptr_t addr = (uintptr_t)block;
    uintptr_t base = (uintptr_t)memory_base;
    uintptr_t offset = addr - base;
    uintptr_t block_size = get_block_size(rank);
    uintptr_t buddy_offset = offset ^ block_size;
    return (void *)(base + buddy_offset);
}

static int get_page_index(void *addr) {
    return ((uintptr_t)addr - (uintptr_t)memory_base) / PAGE_SIZE;
}

static void *get_page_address(int index) {
    return (void *)((uintptr_t)memory_base + index * PAGE_SIZE);
}

static int is_valid_address(void *addr) {
    if (!memory_base || total_pages <= 0) return 0;
    return addr >= memory_base &&
           (uintptr_t)addr < (uintptr_t)memory_base + total_pages * PAGE_SIZE;
}

static int is_aligned(void *addr, int rank) {
    uintptr_t a = (uintptr_t)addr;
    uintptr_t base = (uintptr_t)memory_base;
    uintptr_t offset = a - base;
    uintptr_t size = get_block_size(rank);
    return (offset & (size - 1)) == 0;
}

int init_page(void *p, int pgcount) {
    if (!p || pgcount <= 0) {
        return -EINVAL;
    }

    memory_base = p;
    total_pages = pgcount;

    // Initialize free lists
    for (int i = 1; i <= MAX_RANK; i++) {
        free_lists[i] = NULL;
    }

    // Calculate maximum rank that can hold all pages
    int max_rank = get_max_rank_for_pages(pgcount);

    // Add the entire memory as one free block of max_rank
    FreeBlock *block = (FreeBlock *)p;
    block->next = NULL;
    free_lists[max_rank] = block;

    // Initialize allocation tracking array
    for (int i = 0; i < total_pages; i++) {
        allocated_ranks[i] = 0;
    }

    return OK;
}

void *alloc_pages(int rank) {
    if (rank < 1 || rank > MAX_RANK) {
        return -EINVAL;
    }

    // Find a free block of appropriate size
    int current_rank = rank;
    while (current_rank <= MAX_RANK && free_lists[current_rank] == NULL) {
        current_rank++;
    }

    if (current_rank > MAX_RANK) {
        return ERR_PTR(-ENOSPC);
    }

    // Take the first block from the free list
    FreeBlock *block = free_lists[current_rank];
    free_lists[current_rank] = block->next;

    // Split the block down to the requested rank
    while (current_rank > rank) {
        current_rank--;

        // Create buddy block: block + size of current rank
        uintptr_t buddy_addr = (uintptr_t)block + get_block_size(current_rank);
        FreeBlock *buddy_block = (FreeBlock *)buddy_addr;

        // Add buddy to free list
        buddy_block->next = free_lists[current_rank];
        free_lists[current_rank] = buddy_block;
    }

    // Mark pages as allocated with this rank
    int block_size = get_block_size(rank);
    int page_index = get_page_index(block);
    int pages_in_block = block_size / PAGE_SIZE;

    for (int i = 0; i < pages_in_block; i++) {
        allocated_ranks[page_index + i] = rank;
    }

    return block;
}

void *return_pages(void *p) {
    if (!p || !is_valid_address(p)) {
        return ERR_PTR(-EINVAL);
    }

    // Get the rank from allocation tracking
    int page_index = get_page_index(p);
    int rank = allocated_ranks[page_index];

    if (rank == 0) {
        // Block is not allocated
        return ERR_PTR(-EINVAL);
    }

    // Check alignment
    if (!is_aligned(p, rank)) {
        return -EINVAL;
    }

    // Clear allocation marks
    int block_size = get_block_size(rank);
    int pages_in_block = block_size / PAGE_SIZE;

    for (int i = 0; i < pages_in_block; i++) {
        allocated_ranks[page_index + i] = 0;
    }

    // Merge with buddies
    void *current_block = p;
    int current_rank = rank;

    while (current_rank < MAX_RANK) {
        void *buddy = get_buddy(current_block, current_rank);

        // Check if buddy is in free list of this rank
        FreeBlock **prev = &free_lists[current_rank];
        FreeBlock *curr = free_lists[current_rank];
        int buddy_found = 0;

        while (curr) {
            if (curr == buddy) {
                // Remove buddy from free list
                *prev = curr->next;
                buddy_found = 1;
                break;
            }
            prev = &curr->next;
            curr = curr->next;
        }

        if (!buddy_found) {
            break;
        }

        // Merge with buddy
        if (current_block > buddy) {
            current_block = buddy;
        }
        current_rank++;
    }

    // Add the merged block to free list
    FreeBlock *block = (FreeBlock *)current_block;
    block->next = free_lists[current_rank];
    free_lists[current_rank] = block;

    return (void *)OK;
}

int query_ranks(void *p) {
    if (!p || !is_valid_address(p)) {
        return -EINVAL;
    }

    int page_index = get_page_index(p);
    int rank = allocated_ranks[page_index];

    if (rank > 0) {
        // Page is allocated, return its rank
        return rank;
    }

    // Page is free, find the largest free block containing it
    // Check free lists from largest to smallest rank
    for (int r = MAX_RANK; r >= 1; r--) {
        FreeBlock *curr = free_lists[r];
        uintptr_t block_size = get_block_size(r);

        while (curr) {
            uintptr_t block_start = (uintptr_t)curr;
            uintptr_t block_end = block_start + block_size;

            if ((uintptr_t)p >= block_start && (uintptr_t)p < block_end) {
                // Found in free list of rank r
                return r;
            }

            curr = curr->next;
        }
    }

    // Should not happen
    return -EINVAL;
}

int query_page_counts(int rank) {
    if (rank < 1 || rank > MAX_RANK) {
        return -EINVAL;
    }

    int count = 0;
    FreeBlock *curr = free_lists[rank];

    while (curr) {
        count++;
        curr = curr->next;
    }

    return count;
}
