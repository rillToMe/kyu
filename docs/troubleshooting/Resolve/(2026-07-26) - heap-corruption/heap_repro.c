// Standalone reproduction: EXACT kernel heap allocator (heap.c logic) driven
// by stb_image decoding the real kyuzen.png. Backed by a flat arena instead
// of the kernel VMM. Goal: reproduce header corruption on the host.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#define HEAP_MAGIC 0xDEADC0DE

typedef struct heap_block {
    uint32_t magic;
    size_t size;
    uint8_t is_free;
    struct heap_block* next;
} heap_block_t;

// ---- arena-backed page allocator, mimics vmm_alloc_page(current_heap_end,7) ----
#define ARENA_PAGES 8192            // 32 MB arena
static uint8_t arena[ARENA_PAGES * 4096] __attribute__((aligned(4096)));
static uint64_t arena_next = 0;     // next unused page index

// current_heap_end is a real pointer into the arena so block pointer identity
// works exactly like the kernel (blocks live at current_heap_end and grow up).
static uint64_t current_heap_end = 0;   // set in main() to (uint64_t)&arena[0]

static int vmm_alloc_page(uint64_t vaddr, int flags) {
    (void)vaddr; (void)flags;
    if (arena_next >= ARENA_PAGES) return 0;
    arena_next++;
    return 1;
}

heap_block_t* heap_head = NULL;

static heap_block_t* expand_heap(size_t required_size) {
    size_t total_size    = required_size + sizeof(heap_block_t);
    uint64_t pages_needed = total_size / 4096;
    if (total_size % 4096 != 0) pages_needed++;

    uint64_t start_expansion_addr = current_heap_end;

    for (uint64_t i = 0; i < pages_needed; i++) {
        if (!vmm_alloc_page(current_heap_end, 7)) return NULL;
        current_heap_end += 4096;
    }

    heap_block_t* new_block = (heap_block_t*)start_expansion_addr;
    new_block->magic   = HEAP_MAGIC;
    new_block->size    = (size_t)(pages_needed * 4096) - sizeof(heap_block_t);
    new_block->is_free = 1;
    new_block->next    = NULL;

    if (heap_head == NULL) {
        heap_head = new_block;
    } else {
        heap_block_t* curr = heap_head;
        while (curr->next != NULL) curr = curr->next;
        curr->next = new_block;
    }
    return new_block;
}

static void dump_and_abort(heap_block_t* bad, const char* where);

void* kmalloc(size_t size) {
    if (size == 0) return NULL;
    size = (size + 15) & ~(size_t)15;

    heap_block_t* current = heap_head;
    while (current != NULL) {
        if (current->magic != HEAP_MAGIC) dump_and_abort(current, "kmalloc-walk");
        if (current->is_free && current->size >= size) {
            if (current->size > size + sizeof(heap_block_t) + 1) {
                heap_block_t* new_block = (heap_block_t*)((uint8_t*)current + sizeof(heap_block_t) + size);
                new_block->magic   = HEAP_MAGIC;
                new_block->is_free = 1;
                new_block->size    = current->size - size - sizeof(heap_block_t);
                new_block->next    = current->next;
                current->next = new_block;
                current->size = size;
            }
            current->is_free = 0;
            return (void*)((uint8_t*)current + sizeof(heap_block_t));
        }
        current = current->next;
    }
    heap_block_t* fresh_block = expand_heap(size);
    if (fresh_block == NULL) return NULL;
    return kmalloc(size);
}

void kfree(void* ptr) {
    if (ptr == NULL) return;
    heap_block_t* block = (heap_block_t*)((uint8_t*)ptr - sizeof(heap_block_t));
    if (block->magic != HEAP_MAGIC) dump_and_abort(block, "kfree-hdr");
    block->is_free = 1;

    heap_block_t* current = heap_head;
    while (current != NULL) {
        if (current->magic != HEAP_MAGIC) dump_and_abort(current, "kfree-coalesce");
        heap_block_t* expected_next = (heap_block_t*)((uint8_t*)current + sizeof(heap_block_t) + current->size);
        if (current->is_free && current->next != NULL && current->next->is_free &&
            current->next == expected_next) {
            current->size += current->next->size + sizeof(heap_block_t);
            current->next  = current->next->next;
        } else {
            current = current->next;
        }
    }
}

void* krealloc(void* ptr, size_t old_size, size_t new_size) {
    if (new_size == 0) { kfree(ptr); return NULL; }
    if (ptr == NULL)   return kmalloc(new_size);
    void* new_ptr = kmalloc(new_size);
    if (new_ptr == NULL) return NULL;
    size_t copied = (old_size < new_size) ? old_size : new_size;
    memcpy(new_ptr, ptr, copied);
    kfree(ptr);
    return new_ptr;
}

static void dump_and_abort(heap_block_t* bad, const char* where) {
    fprintf(stderr, "\n*** HEAP CORRUPTION (%s) block=%p\n", where, (void*)bad);
    fprintf(stderr, "    magic=0x%08X (want 0x%08X) size=0x%zX free=%u next=%p\n",
            bad->magic, HEAP_MAGIC, bad->size, bad->is_free, (void*)bad->next);
    fflush(stderr);
    abort();
}

// stb hooks -> kernel allocator
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_SIMD
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_ASSERT(x)
#define STBI_MALLOC(sz)                       kmalloc(sz)
#define STBI_FREE(p)                          kfree(p)
#define STBI_REALLOC_SIZED(p, old_sz, new_sz) krealloc(p, old_sz, new_sz)
#include "include/stb_image.h"

int main(int argc, char** argv) {
    current_heap_end = (uint64_t)&arena[0];

    const char* fn = argc > 1 ? argv[1] : "kyuzen.png";
    FILE* f = fopen(fn, "rb");
    if (!f) { fprintf(stderr, "open failed: %s\n", fn); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    fprintf(stderr, "file=%s size=%ld (0x%lX)\n", fn, sz, sz);

    // Mirror viewer.c render_image exactly: alloc raw, read, decode(req_comp=4), free raw, free pixels
    uint8_t* raw = (uint8_t*)kmalloc((size_t)sz);
    fread(raw, 1, (size_t)sz, f); fclose(f);

    int iw, ih, ch;
    uint8_t* px = stbi_load_from_memory(raw, (int)sz, &iw, &ih, &ch, 4);
    fprintf(stderr, "decoded: %dx%d src_ch=%d px=%p\n", iw, ih, ch, (void*)px);
    kfree(raw);
    if (px) kfree(px);

    fprintf(stderr, "NO CORRUPTION — kernel allocator survived stb on this image\n");
    return 0;
}
