/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "helper-tcg.h"
#include "qemu/bitmap.h"
#ifndef CONFIG_USER_ONLY
#include "accel/tcg/cpu-mmu-index.h"
#include "accel/tcg/cpu-loop.h"
#include "accel/tcg/probe.h"
#include "exec/target_page.h"
#include "exec/tlb-flags.h"
#include "exec/translation-block.h"
#include "hw/core/boards.h"
#include "system/reset.h"
#include "system/memory.h"
#include "system/ramblock.h"
#endif

void HELPER(dead_check_reg)(CPUX86State *env, uint32_t reg, uint32_t mask)
{
    if (env->dead_regs[reg] & mask) {
        raise_exception_err_ra(env, EXCP0D_GPF, 0, GETPC());
    }
}

void HELPER(dead_clear_reg)(CPUX86State *env, uint32_t reg, uint32_t mask)
{
    env->dead_regs[reg] &= ~mask;
}

void HELPER(dead_mark_reg)(CPUX86State *env, uint32_t reg, uint32_t mask)
{
    env->dead_regs[reg] |= mask;
}

void HELPER(dead_guard)(CPUX86State *env)
{
    for (unsigned i = 0; i < CPU_NB_EREGS; i++) {
        if (env->dead_regs[i]) {
            raise_exception_ra(env, EXCP06_ILLOP, GETPC());
        }
    }
}

#ifndef CONFIG_USER_ONLY
#define TYPE_X86_DEAD_MEMORY "x86-dead-memory"
OBJECT_DECLARE_SIMPLE_TYPE(X86DeadMemoryState, X86_DEAD_MEMORY)

struct X86DeadMemoryState {
    Object parent_obj;
    QemuMutex lock;
    GHashTable *pages;
    Notifier ram_write_notifier;
};

static void dead_memory_clear(X86DeadMemoryState *state, ram_addr_t addr,
                               hwaddr size);

static void dead_memory_ram_write(Notifier *notifier, void *opaque)
{
    X86DeadMemoryState *state = container_of(notifier, X86DeadMemoryState,
                                            ram_write_notifier);
    const MemoryRAMWrite *write = opaque;

    qemu_mutex_lock(&state->lock);
    dead_memory_clear(state, write->addr, write->size);
    qemu_mutex_unlock(&state->lock);
}

static void dead_memory_reset(void *opaque)
{
    X86DeadMemoryState *state = opaque;

    qemu_mutex_lock(&state->lock);
    g_hash_table_remove_all(state->pages);
    qemu_mutex_unlock(&state->lock);
}

static void dead_memory_init(Object *obj)
{
    X86DeadMemoryState *state = X86_DEAD_MEMORY(obj);

    qemu_mutex_init(&state->lock);
    state->pages = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                        g_free, g_free);
    state->ram_write_notifier.notify = dead_memory_ram_write;
    memory_ram_write_notifier_add(&state->ram_write_notifier);
    qemu_register_reset(dead_memory_reset, state);
}

static void dead_memory_finalize(Object *obj)
{
    X86DeadMemoryState *state = X86_DEAD_MEMORY(obj);

    qemu_unregister_reset(dead_memory_reset, state);
    memory_ram_write_notifier_remove(&state->ram_write_notifier);
    g_hash_table_destroy(state->pages);
    qemu_mutex_destroy(&state->lock);
}

static const TypeInfo dead_memory_type_info = {
    .name = TYPE_X86_DEAD_MEMORY,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(X86DeadMemoryState),
    .instance_init = dead_memory_init,
    .instance_finalize = dead_memory_finalize,
};

static void dead_memory_register_types(void)
{
    type_register_static(&dead_memory_type_info);
}

type_init(dead_memory_register_types)

void x86_dead_memory_init(X86CPU *cpu)
{
    Object *machine = OBJECT(qdev_get_machine());
    Object *state = object_resolve_path_component(machine, "dead-memory");

    if (!state) {
        state = object_new(TYPE_X86_DEAD_MEMORY);
        object_property_add_child(machine, "dead-memory", state);
        object_unref(state);
    }
    cpu->dead_memory_state = X86_DEAD_MEMORY(state);
}

bool x86_dead_memory_enabled(CPUState *cs, int mmu_idx)
{
    return X86_CPU(cs)->dead_memory && mmu_idx < MMU_PHYS_IDX;
}

static unsigned long *dead_memory_page(X86DeadMemoryState *state,
                                       ram_addr_t addr, bool create)
{
    uint64_t base = addr & ~((ram_addr_t)TARGET_PAGE_SIZE - 1);
    unsigned long *bits = g_hash_table_lookup(state->pages, &base);

    if (!bits && create) {
        uint64_t *key = g_new(uint64_t, 1);

        *key = base;
        bits = bitmap_new(TARGET_PAGE_SIZE);
        g_hash_table_insert(state->pages, key, bits);
    }
    return bits;
}

void x86_dead_mem_read(CPUState *cs, vaddr addr, ram_addr_t ram_addr,
                       unsigned size, uintptr_t ra)
{
    X86DeadMemoryState *state = X86_CPU(cs)->dead_memory_state;
    unsigned long *bits;
    unsigned offset = ram_addr & (TARGET_PAGE_SIZE - 1);
    bool dead;

    qemu_mutex_lock(&state->lock);
    bits = dead_memory_page(state, ram_addr, false);
    dead = bits && find_next_bit(bits, offset + size, offset) < offset + size;
    qemu_mutex_unlock(&state->lock);
    if (dead) {
        raise_exception_err_ra(cpu_env(cs), EXCP0D_GPF, 0, ra);
    }
}

static void dead_memory_clear(X86DeadMemoryState *state, ram_addr_t addr,
                               hwaddr size)
{
    while (size) {
        unsigned offset = addr & (TARGET_PAGE_SIZE - 1);
        unsigned chunk = MIN(size, TARGET_PAGE_SIZE - offset);
        unsigned long *bits = dead_memory_page(state, addr, false);
        uint64_t base = addr & ~((ram_addr_t)TARGET_PAGE_SIZE - 1);

        if (bits) {
            bitmap_clear(bits, offset, chunk);
            if (bitmap_empty(bits, TARGET_PAGE_SIZE)) {
                g_hash_table_remove(state->pages, &base);
            }
        }
        addr += chunk;
        size -= chunk;
    }
}

void x86_dead_mem_write(CPUState *cs, ram_addr_t ram_addr, unsigned size)
{
    X86DeadMemoryState *state = X86_CPU(cs)->dead_memory_state;

    qemu_mutex_lock(&state->lock);
    dead_memory_clear(state, ram_addr, size);
    qemu_mutex_unlock(&state->lock);
}

static void dead_memory_range(CPUX86State *env, target_ulong addr,
                               target_ulong len, bool mark, uintptr_t ra)
{
    CPUState *cs = env_cpu(env);
    X86DeadMemoryState *state = X86_CPU(cs)->dead_memory_state;
    int mmu_idx = cpu_mmu_index(cs, false);

    while (len) {
        unsigned size = MIN(len, TARGET_PAGE_SIZE -
                                 (addr & ~TARGET_PAGE_MASK));
        CPUTLBEntryFull *full;
        void *host;
        int flags = probe_access_full(env, addr, size, MMU_DATA_STORE,
                                       mmu_idx, false, &host, &full, ra);

        if ((flags & (TLB_MMIO | TLB_DISCARD_WRITE)) ||
            !memory_region_is_ram(full->section->mr) ||
            full->section->readonly) {
            raise_exception_err_ra(env, EXCP0D_GPF, 0, ra);
        }
        if (mark) {
            ram_addr_t ram_addr = full->xlat_offset + addr;
            unsigned long *bits;

            qemu_mutex_lock(&state->lock);
            bits = dead_memory_page(state, ram_addr, true);
            bitmap_set(bits, ram_addr & (TARGET_PAGE_SIZE - 1), size);
            qemu_mutex_unlock(&state->lock);
        }
        addr += size;
        len -= size;
    }
}

typedef struct DeadMemoryAccess {
    ram_addr_t ram_addr;
    uint8_t *host;
    size_t ordinal;
    unsigned size;
} DeadMemoryAccess;

struct DeadMemoryTransfer {
    size_t count;
    DeadMemoryAccess access[];
};

typedef struct DeadMemoryCursor {
    const DeadMemoryAccess *access;
    unsigned offset;
} DeadMemoryCursor;

void x86_dead_transfer_cleanup(X86CPU *cpu)
{
    g_clear_pointer(&cpu->dead_transfer, g_free);
}

static G_NORETURN void dead_memory_transfer_fault(X86CPU *cpu, uintptr_t ra)
{
    x86_dead_transfer_cleanup(cpu);
    raise_exception_err_ra(&cpu->env, EXCP0D_GPF, 0, ra);
}

static uint64_t dead_memory_ram_size(void)
{
    RAMBlock *block;
    uint64_t size = 0;

    RAMBLOCK_FOREACH(block) {
        uint64_t length = qemu_ram_get_used_length(block);

        if (length > UINT64_MAX - size) {
            return UINT64_MAX;
        }
        size += length;
    }
    return size;
}

static size_t dead_memory_access_count(X86CPU *cpu, target_ulong addr,
                                       target_ulong size, uintptr_t ra)
{
    target_ulong count;

    if (size - 1 > (target_ulong)-1 - addr) {
        dead_memory_transfer_fault(cpu, ra);
    }
    count = ((addr & ~TARGET_PAGE_MASK) + size - 1) / TARGET_PAGE_SIZE + 1;
    if ((target_ulong)(size_t)count != count) {
        dead_memory_transfer_fault(cpu, ra);
    }
    return count;
}

static void dead_memory_map(CPUX86State *env, target_ulong addr,
                             target_ulong size, MMUAccessType type,
                             DeadMemoryAccess *access, size_t ordinal,
                             uintptr_t ra)
{
    int mmu_idx = cpu_mmu_index(env_cpu(env), false);

    while (size) {
        unsigned chunk = MIN(size, TARGET_PAGE_SIZE -
                                   (addr & ~TARGET_PAGE_MASK));
        CPUTLBEntryFull *full;
        MemoryRegion *mr;
        void *host;
        int flags = probe_access_full(env, addr, chunk, type, mmu_idx,
                                       false, &host, &full, ra);

        mr = full->section->mr;
        if ((flags & TLB_MMIO) || !memory_region_is_ram(mr) ||
            (type == MMU_DATA_STORE &&
             ((flags & TLB_DISCARD_WRITE) || full->section->readonly))) {
            dead_memory_transfer_fault(X86_CPU(env_cpu(env)), ra);
        }
        access->ram_addr = full->xlat_offset + addr;
        access->host = memory_region_get_ram_ptr(mr);
        access->host += access->ram_addr - memory_region_get_ram_addr(mr);
        access->size = chunk;
        access->ordinal = ordinal++;
        access++;
        addr += chunk;
        size -= chunk;
    }
}

static int dead_memory_compare_backing(const void *a, const void *b)
{
    const DeadMemoryAccess *left = a;
    const DeadMemoryAccess *right = b;

    if (left->ram_addr < right->ram_addr) {
        return -1;
    }
    return left->ram_addr > right->ram_addr;
}

static int dead_memory_compare_ordinal(const void *a, const void *b)
{
    const DeadMemoryAccess *left = a;
    const DeadMemoryAccess *right = b;

    if (left->ordinal < right->ordinal) {
        return -1;
    }
    return left->ordinal > right->ordinal;
}

static void dead_memory_advance(DeadMemoryCursor *cursor, unsigned size)
{
    cursor->offset += size;
    if (cursor->offset == cursor->access->size) {
        cursor->access++;
        cursor->offset = 0;
    }
}

static void dead_memory_copy_bytes(DeadMemoryCursor *dst,
                                    DeadMemoryCursor *src, target_ulong size)
{
    while (size) {
        unsigned chunk = MIN(dst->access->size - dst->offset,
                             src->access->size - src->offset);

        chunk = MIN(size, chunk);
        memcpy(dst->access->host + dst->offset,
               src->access->host + src->offset, chunk);
        dead_memory_advance(dst, chunk);
        dead_memory_advance(src, chunk);
        size -= chunk;
    }
}

static void dead_memory_load(DeadMemoryCursor *src, uint8_t *buf, unsigned size)
{
    while (size) {
        unsigned chunk = MIN(size, src->access->size - src->offset);

        memcpy(buf, src->access->host + src->offset, chunk);
        dead_memory_advance(src, chunk);
        buf += chunk;
        size -= chunk;
    }
}

static void dead_memory_store(DeadMemoryCursor *dst, const uint8_t *buf,
                               unsigned size)
{
    while (size) {
        unsigned chunk = MIN(size, dst->access->size - dst->offset);

        memcpy(dst->access->host + dst->offset, buf, chunk);
        dead_memory_advance(dst, chunk);
        buf += chunk;
        size -= chunk;
    }
}

static void dead_memory_pack(X86DeadMemoryState *state, DeadMemoryCursor *src,
                              uint8_t *tags, unsigned size)
{
    unsigned bit = 0;

    memset(tags, 0, size / 8 + (size % 8 != 0));
    while (size) {
        ram_addr_t addr = src->access->ram_addr + src->offset;
        unsigned start = addr & (TARGET_PAGE_SIZE - 1);
        unsigned chunk = MIN(size, src->access->size - src->offset);
        unsigned long *bits = dead_memory_page(state, addr, false);

        if (bits) {
            for (unsigned i = 0; i < chunk; i++) {
                if (test_bit(start + i, bits)) {
                    tags[(bit + i) / 8] |= 1u << ((bit + i) % 8);
                }
            }
        }
        dead_memory_advance(src, chunk);
        bit += chunk;
        size -= chunk;
    }
}

static void dead_memory_unpack(X86DeadMemoryState *state, DeadMemoryCursor *dst,
                                const uint8_t *tags, unsigned size)
{
    unsigned bit = 0;

    while (size) {
        ram_addr_t addr = dst->access->ram_addr + dst->offset;
        unsigned start = addr & (TARGET_PAGE_SIZE - 1);
        unsigned chunk = MIN(size, dst->access->size - dst->offset);
        unsigned long *bits = dead_memory_page(state, addr, false);
        uint64_t base = addr & ~((ram_addr_t)TARGET_PAGE_SIZE - 1);

        if (bits) {
            bitmap_clear(bits, start, chunk);
        }
        for (unsigned i = 0; i < chunk; i++) {
            if (tags[(bit + i) / 8] & (1u << ((bit + i) % 8))) {
                if (!bits) {
                    bits = dead_memory_page(state, addr, true);
                }
                set_bit(start + i, bits);
            }
        }
        if (bits && bitmap_empty(bits, TARGET_PAGE_SIZE)) {
            g_hash_table_remove(state->pages, &base);
        }
        dead_memory_advance(dst, chunk);
        bit += chunk;
        size -= chunk;
    }
}

static void dead_memory_transfer(CPUX86State *env, uint32_t operation,
                                  uintptr_t ra)
{
    CPUState *cs = env_cpu(env);
    X86CPU *cpu = X86_CPU(cs);
    X86DeadMemoryState *state = cpu->dead_memory_state;
    target_ulong dst = env->regs[R_EDI];
    target_ulong src = env->regs[R_ESI];
    target_ulong size = env->regs[R_ECX];
    target_ulong src_size, dst_size, tag_size;
    DeadMemoryAccess *access, *source, *destination;
    DeadMemoryCursor input, output;
    size_t src_count, dst_count, count;
    uint64_t ram_size;
    uint8_t tags[256];

    x86_dead_transfer_cleanup(cpu);
    if (!size) {
        return;
    }
    if (tcg_cflags_has(cs, CF_PARALLEL) && !cpu_in_exclusive_context(cs)) {
        cpu_loop_exit_atomic(cs, ra);
    }

    tag_size = size / 8 + (size % 8 != 0);
    src_size = dst_size = size;
    if (operation == DEAD64_SAVE_MEMORY) {
        if (tag_size > (target_ulong)-1 - size) {
            dead_memory_transfer_fault(cpu, ra);
        }
        dst_size += tag_size;
    } else if (operation == DEAD64_RESTORE_MEMORY) {
        if (tag_size > (target_ulong)-1 - size) {
            dead_memory_transfer_fault(cpu, ra);
        }
        src_size += tag_size;
    } else {
        g_assert(operation == DEAD64_COPY_MEMORY);
    }

    ram_size = dead_memory_ram_size();
    if (src_size > ram_size || dst_size > ram_size - src_size) {
        dead_memory_transfer_fault(cpu, ra);
    }
    src_count = dead_memory_access_count(cpu, src, src_size, ra);
    dst_count = dead_memory_access_count(cpu, dst, dst_size, ra);
    if (src_count > SIZE_MAX - dst_count) {
        dead_memory_transfer_fault(cpu, ra);
    }
    count = src_count + dst_count;
    if (count > (SIZE_MAX - sizeof(*cpu->dead_transfer)) / sizeof(*access)) {
        dead_memory_transfer_fault(cpu, ra);
    }
    cpu->dead_transfer = g_try_malloc(sizeof(*cpu->dead_transfer) +
                                    count * sizeof(*access));
    if (!cpu->dead_transfer) {
        dead_memory_transfer_fault(cpu, ra);
    }
    cpu->dead_transfer->count = count;
    access = cpu->dead_transfer->access;
    source = access;
    destination = access + src_count;

    /* CPU ownership keeps descriptors alive if an MMU probe longjmps. */
    dead_memory_map(env, src, src_size, MMU_DATA_LOAD, source, 0, ra);
    dead_memory_map(env, dst, dst_size, MMU_DATA_STORE, destination,
                    src_count, ra);
    qsort(access, cpu->dead_transfer->count, sizeof(*access),
          dead_memory_compare_backing);
    for (size_t i = 1; i < cpu->dead_transfer->count; i++) {
        if (access[i].ram_addr - access[i - 1].ram_addr < access[i - 1].size) {
            dead_memory_transfer_fault(cpu, ra);
        }
    }
    qsort(access, cpu->dead_transfer->count, sizeof(*access),
          dead_memory_compare_ordinal);

    qemu_mutex_lock(&state->lock);
    if (operation == DEAD64_RESTORE_MEMORY) {
        for (size_t i = 0; i < src_count; i++) {
            unsigned long *bits = dead_memory_page(state, source[i].ram_addr,
                                                   false);
            unsigned start = source[i].ram_addr & (TARGET_PAGE_SIZE - 1);

            if (bits && find_next_bit(bits, start + source[i].size, start) <
                        start + source[i].size) {
                qemu_mutex_unlock(&state->lock);
                dead_memory_transfer_fault(cpu, ra);
            }
        }
    }
    input = (DeadMemoryCursor) { .access = source };
    output = (DeadMemoryCursor) { .access = destination };
    dead_memory_copy_bytes(&output, &input, size);
    if (operation != DEAD64_RESTORE_MEMORY) {
        input = (DeadMemoryCursor) { .access = source };
    }
    if (operation != DEAD64_SAVE_MEMORY) {
        output = (DeadMemoryCursor) { .access = destination };
    }
    while (size) {
        unsigned chunk = MIN(size, sizeof(tags) * 8);
        unsigned bytes = chunk / 8 + (chunk % 8 != 0);

        if (operation == DEAD64_RESTORE_MEMORY) {
            dead_memory_load(&input, tags, bytes);
        } else {
            dead_memory_pack(state, &input, tags, chunk);
        }
        if (operation == DEAD64_SAVE_MEMORY) {
            dead_memory_store(&output, tags, bytes);
        } else {
            dead_memory_unpack(state, &output, tags, chunk);
        }
        size -= chunk;
    }
    if (operation == DEAD64_SAVE_MEMORY) {
        for (size_t i = 0; i < dst_count; i++) {
            dead_memory_clear(state, destination[i].ram_addr,
                               destination[i].size);
        }
    }
    qemu_mutex_unlock(&state->lock);
    x86_dead_transfer_cleanup(cpu);
}
#endif

void HELPER(dead_memory)(CPUX86State *env)
{
#ifndef CONFIG_USER_ONLY
    CPUState *cs = env_cpu(env);
    target_ulong addr = env->regs[R_EDI];
    target_ulong len = env->regs[R_ECX];
    uintptr_t ra = GETPC();

    if (!len) {
        return;
    }
    if (len - 1 > (target_ulong)-1 - addr) {
        raise_exception_err_ra(env, EXCP0D_GPF, 0, ra);
    }
    /* Serialize tag changes with other CPUs' memory accesses. */
    if (tcg_cflags_has(cs, CF_PARALLEL) && !cpu_in_exclusive_context(cs)) {
        cpu_loop_exit_atomic(cs, ra);
    }
    dead_memory_range(env, addr, len, false, ra);
    dead_memory_range(env, addr, len, true, ra);
#else
    raise_exception(env, EXCP06_ILLOP);
#endif
}

void HELPER(dead_transfer)(CPUX86State *env, uint32_t operation)
{
#ifndef CONFIG_USER_ONLY
    dead_memory_transfer(env, operation, GETPC());
#else
    raise_exception(env, EXCP06_ILLOP);
#endif
}
