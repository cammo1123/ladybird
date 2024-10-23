/*
 * Copyright (c) 2021-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>
#include <AK/Random.h>
#include <AK/Vector.h>
#include <LibJS/Heap/BlockAllocator.h>
#include <LibJS/Heap/HeapBlock.h>

#ifdef AK_OS_WINDOWS
#    include <windows.h>
#else
#    include <sys/mman.h>
#endif

#ifdef HAS_ADDRESS_SANITIZER
#    include <sanitizer/asan_interface.h>
#    include <sanitizer/lsan_interface.h>
#endif

#if defined(AK_OS_GNU_HURD) || (!defined(MADV_FREE) && !defined(MADV_DONTNEED))
#    define USE_FALLBACK_BLOCK_DEALLOCATION
#endif

namespace JS {

BlockAllocator::~BlockAllocator()
{
    for (auto* block : m_blocks) {
        ASAN_UNPOISON_MEMORY_REGION(block, HeapBlock::block_size);
#if !defined(AK_OS_WINDOWS)
        if (munmap(block, HeapBlock::block_size) < 0) {
            perror("munmap");
            VERIFY_NOT_REACHED();
        }
#else
        if (!VirtualFree(block, 0, MEM_RELEASE)) {
            perror("VirtualFree");
            VERIFY_NOT_REACHED();
        }
#endif
    }
}

void* BlockAllocator::allocate_block([[maybe_unused]] char const* name)
{
    if (!m_blocks.is_empty()) {
        // To reduce predictability, take a random block from the cache.
        size_t random_index = get_random_uniform(m_blocks.size());
        auto* block = m_blocks.unstable_take(random_index);
        ASAN_UNPOISON_MEMORY_REGION(block, HeapBlock::block_size);
        LSAN_REGISTER_ROOT_REGION(block, HeapBlock::block_size);
        return block;
    }

#ifdef AK_OS_WINDOWS
    auto* block = (HeapBlock*)VirtualAlloc(nullptr, HeapBlock::block_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    VERIFY(block);
#else
    auto* block = (HeapBlock*)mmap(nullptr, HeapBlock::block_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    VERIFY(block != MAP_FAILED);
#endif
    LSAN_REGISTER_ROOT_REGION(block, HeapBlock::block_size);
    return block;
}

void BlockAllocator::deallocate_block(void* block)
{
    VERIFY(block);

#if defined(AK_OS_WINDOWS)
    if (!VirtualFree(block, 0, MEM_RELEASE)) {
        perror("VirtualFree");
        VERIFY_NOT_REACHED();
    }
    block = VirtualAlloc(block, HeapBlock::block_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    VERIFY(block);
#elif defined(USE_FALLBACK_BLOCK_DEALLOCATION)
    if (munmap(block, HeapBlock::block_size) < 0) {
        perror("munmap");
        VERIFY_NOT_REACHED();
    }
    if (mmap(block, HeapBlock::block_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0) != block) {
        perror("mmap");
        VERIFY_NOT_REACHED();
    }
#elif defined(MADV_FREE)
    if (madvise(block, HeapBlock::block_size, MADV_FREE) < 0) {
        perror("madvise(MADV_FREE)");
        VERIFY_NOT_REACHED();
    }
#elif defined(MADV_DONTNEED)
    if (madvise(block, HeapBlock::block_size, MADV_DONTNEED) < 0) {
        perror("madvise(MADV_DONTNEED)");
        VERIFY_NOT_REACHED();
    }
#endif

    ASAN_POISON_MEMORY_REGION(block, HeapBlock::block_size);
    LSAN_UNREGISTER_ROOT_REGION(block, HeapBlock::block_size);
    m_blocks.append(block);
}

}
