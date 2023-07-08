#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <mutex>

#include "help.h"

// implement based on:
// Hoard: A Scalable Memory Allocator for Multithreaded Applications
// with some modifications

// alignas keyword: https://en.cppreference.com/w/cpp/language/alignas
struct alignas(16) Block {
    size_t size;
    bool is_free;
    SuperBlock *sb;
    Block *prev;
    Block *next;

    // Block() = delete;
    static Block *allocate(size_t size) {
        Block *block = (Block *)mmap(NULL, PAD_UP(sizeof(Block) + size, 16),
                                     PROT_READ | PROT_WRITE,
                                     MAP_ANONYMOUS | MAP_SHARED, -1, 0);
        if (block == MAP_FAILED) {
            return nullptr;
        }
        block->size = size;
        block->is_free = true;
        block->sb = nullptr;
        block->prev = nullptr;
        block->next = nullptr;
        return block;
    }
    bool deallocate() { return !munmap(this, sizeof(Block) + size); }
    size_t total_size() { return sizeof(Block) + size; }
    void *data() { return (void *)((char *)this + sizeof(Block)); }
    void slice(size_t size, Block *&right) {
        size_t remain_size = this->size - size;
        right = nullptr;
        if (remain_size > sizeof(Block) + 16) {
            right = (Block *)((uintptr_t)(this->data()) + size);
            right->size = remain_size - sizeof(Block);
            right->is_free = true;
            right->sb = this->sb;
            right->prev = this;
            right->next = this->next;
            if (right->next) {
                char a = *((char *)right->next);
                (void)a;
            }
            if (this->next) {
                this->next->prev = right;
            }
            this->next = right;
            this->size = size;
        }
    }
};

struct alignas(16) SuperBlock {
    static constexpr size_t standard_size = 16 * 1024 * 1024;
    static constexpr int max_child_count = 128;  // prevent fragmentation
    Heap *heap;
    SuperBlock *prev;
    SuperBlock *next;

    Block *first_child;
    int size;
    int used_size;
    // using atomic affects performance of course, but a
    // better solution needs another 2 hours to debug.
    std::atomic<int> child_count;
    std::mutex slock;

    static SuperBlock *allocate(size_t size, Heap *parent, SuperBlock *prev,
                                SuperBlock *next) {
        SuperBlock *block = (SuperBlock *)mmap(
            NULL, PAD_UP(sizeof(SuperBlock) + size, 16), PROT_READ | PROT_WRITE,
            MAP_ANONYMOUS | MAP_SHARED, -1, 0);
        if (block == MAP_FAILED) {
            return nullptr;
        }
        block->size = size;
        block->heap = parent;
        block->prev = prev;
        block->next = next;
        block->first_child = (Block *)block->data();
        block->first_child->size = size - sizeof(Block);
        block->first_child->is_free = true;
        block->first_child->sb = block;
        block->first_child->prev = nullptr;
        block->first_child->next = nullptr;
        block->child_count = 1;
        return block;
    }

    SuperBlock() = delete;
    void lock() { slock.lock(); }
    void unlock() { slock.unlock(); }
    void *data() { return (void *)((char *)this + sizeof(SuperBlock)); }
    bool deallocate() { return !munmap(this, sizeof(SuperBlock) + size); }

    Block *malloc(size_t size) {
        size = PAD_UP(size, 16);
        // fail
        if ((int)(size + sizeof(Block)) > this->size - this->used_size ||
            this->child_count >= max_child_count) {
            return nullptr;
        }
        Block *b = this->first_child;
        Block *bb = b;
        while (b) {
            // find first free block
            if (b->is_free && b->size >= size) {
                Block *right;
                b->slice(size, right);
                b->is_free = false;
                this->used_size += b->total_size();
                if (right) {
                    this->child_count++;
                }
                return b;
            }
            b = b->next;
            if (bb && bb->next) {
                bb = bb->next->next;
                if (b == bb) {
                    break;
                }
            }
        }
        return nullptr;
    }

    void free(Block *block) {
        block->is_free = true;
        this->used_size -= block->total_size();
        // if next is free, merge with block
        if (block->next && block->next->is_free) {
            block->size += block->next->total_size();
            block->next = block->next->next;
            if (block->next) {
                block->next->prev = block;
            }
            this->child_count--;
        }
        // if prev is free, merge with prev
        if (block->prev && block->prev->is_free) {
            block->prev->next = block->next;
            block->prev->size += block->total_size();
            if (block->next) {
                block->next->prev = block->prev;
            }
            this->child_count--;
        }
    }

    float used_ratio() { return (float)used_size / size; }
};

class Heap {
   public:
    std::mutex slock;
    LinkList<SuperBlock> super_blocks;

    // static Heap global_heap;
    // provide for std::lock_guard
    void lock() { slock.lock(); }
    void unlock() { slock.unlock(); }

    // all these functions are NOT thread-safe, they should be called under lock
    Block *malloc(size_t size);

    void free(Block *block);
};

Block *Heap::malloc(size_t size) {
    if (size > SuperBlock::standard_size / 2) {
        Block *large_block = Block::allocate(size);
        return large_block;
    }
    // find a super block that can fit the size
    for (SuperBlock *sb : super_blocks) {
        Block *block = sb->malloc(size);
        if (block) {
            return block;
        }
    }
    // if (this == &global_heap) {
    //     return nullptr;
    // }

    // try brorrow a super
    // block from the global heap if (global_heap.super_blocks.head != nullptr)
    // {
    //     global_heap.lock();
    //     if (global_heap.super_blocks.head != nullptr) {
    //         // try malloc from the global heap
    //         Block *block = global_heap.malloc(size);
    //         if (block) {
    //             // get the ownership of the super block
    //             global_heap.super_blocks.remove(block->sb);
    //             block->sb->heap = this;
    //             global_heap.unlock();
    //             this->super_blocks.insert(block->sb);
    //             return block;
    //         }
    //     }
    //     global_heap.unlock();
    // }

    // allocate a new super block
    SuperBlock *sb =
        SuperBlock::allocate(SuperBlock::standard_size, this, nullptr, nullptr);
    if (sb == nullptr) {
        return nullptr;
    }
    this->super_blocks.insert(sb);
    Block *block = sb->malloc(size);
    return block;
}

void Heap::free(Block *block) {
    SuperBlock *sb = block->sb;
    sb->free(block);
    // if (sb->used_ratio() < 0.25 && this != &global_heap) {
    //     // if the super block is not used, transfer it to the global heap
    //     this->super_blocks.remove(sb);
    //     global_heap.lock();
    //     global_heap.super_blocks.insert(sb);
    //     sb->heap = &global_heap;
    //     global_heap.unlock();
    // }
}

#define MAX_CPU_NUM 16
// NOTE: mechainism of global heap needs more tuning, currently it
// introduces too many locks and thus degrades multi-thread performance, so
// we disabled it
// Heap Heap::global_heap;
static Heap heaps[MAX_CPU_NUM];

extern "C" {
void *_malloc(size_t size) {
    unsigned cpu, numa;
    getcpu(&cpu, &numa);
    cpu = cpu % MAX_CPU_NUM;
    Heap *heap = &heaps[cpu];
    heap->lock();
    Block *block = heap->malloc(size);
    heap->unlock();
    if (block == nullptr) {
        heap->unlock();
        return nullptr;
    }
    return block->data();
}

void _free(void *ptr) {
    if (ptr == nullptr) {
        return;
    }
    Block *block = reinterpret_cast<Block *>(((uintptr_t)ptr - sizeof(Block)));
    if (block->sb == nullptr) {
        // free a large block
        block->deallocate();
    } else {
    // free a small block
    retry:
        SuperBlock *sb = block->sb;
        sb->lock();
        if (block->sb != sb) {
            block->sb->unlock();
            goto retry;
        }
        Heap *heap = block->sb->heap;  // store to a variable since sb->heap may
                                       // change during free
        heap->lock();
        if (block->sb != sb || block->sb->heap != heap) {
            // the block has been moved to another heap, try again
            heap->unlock();
            sb->unlock();
            goto retry;
        }
        heap->free(block);
        heap->unlock();
        sb->unlock();
    }
    return;
}

void *malloc(size_t size) { return _malloc(size); }
void *calloc(size_t nmemb, size_t size) {
    size_t total_size = nmemb * size;
    void *ptr = malloc(total_size);
    if (ptr == nullptr) {
        return nullptr;
    }
    memset(ptr, 0, total_size);
    return ptr;
}
void *realloc(void *ptr, size_t size) {
    if (ptr == nullptr) {
        return malloc(size);
    }
    Block *block = reinterpret_cast<Block *>(((uintptr_t)ptr - sizeof(Block)));
    if (block->size >= size) {
        return ptr;
    }
    void *new_ptr = malloc(size);
    if (new_ptr == nullptr) {
        return nullptr;
    }
    memcpy(new_ptr, ptr, size);
    free(ptr);
    return new_ptr;
}
void free(void *ptr) { _free(ptr); }
}  // extern "C"