/*
 * jetalloc — per-CPU cache management (rseq front end).
 * SPDX-License-Identifier: MIT
 *
 * We keep, per online CPU, a per-size-class LIFO of recycled blocks. The head
 * of each list is a single `void*` updated by the restartable-sequence asm in
 * jet_rseq.S: a pop/push that the kernel guarantees is atomic w.r.t. thread
 * migration and preemption WITHOUT any lock or atomic instruction. When a
 * thread runs on CPU N it only ever touches slab[N]; two threads that share a
 * core serialise naturally (they can't run simultaneously) so no ping-pong.
 *
 * This is the same idea as tcmalloc's per-CPU cache. It sits in FRONT of the
 * per-thread tcache: alloc tries the CPU slab first, then the tcache, then
 * pages; free pushes to the CPU slab first. On any platform without rseq the
 * whole layer disables itself and jet_core falls straight through to the
 * tcache, so correctness never depends on rseq being present.
 *
 * Memory bounding: each (cpu,class) slab is capped at JET_PERCPU_MAX blocks via
 * a relaxed per-slot counter. The counter can drift by a handful under
 * concurrent push/pop on the same CPU (only possible across a migration window)
 * but the drift is O(threads) and self-corrects, so total cached memory stays
 * within a small constant factor of nr_cpus * NUM_CLASSES * MAX * block_size.
 */
#include "jet_internal.h"

/* rseq needs glibc's <sys/rseq.h> (>= 2.35) which exposes __rseq_offset /
 * __rseq_size and the kernel's registered area. musl does NOT ship this header
 * (nor auto-register an rseq area), so restrict the whole layer to glibc. On
 * musl / any other libc the layer compiles to a no-op and jet_core falls
 * straight through to the tcache — see the file header on why correctness never
 * depends on rseq. __GLIBC__ is defined via <features.h> (pulled by
 * jet_internal.h's standard headers). */
#if defined(__x86_64__) && defined(__linux__) && defined(__GLIBC__)
#  define JET_HAVE_RSEQ 1
#  include <sys/rseq.h>          /* __rseq_offset / __rseq_size (glibc >= 2.35) */
#  include <unistd.h>
#  include <stdlib.h>
#else
#  define JET_HAVE_RSEQ 0
#endif

/* ThreadSanitizer happens-before annotations. The block handoff between a
 * freeing thread (push) and a later allocating thread (pop) is made safe by the
 * rseq restartable-sequence semantics in jet_rseq.S — hand-written asm that
 * TSan's instrumentation cannot see. Without help TSan reports a spurious data
 * race on the reused block's payload (old owner's read vs new owner's write)
 * because it has no synchronizes-with edge through the opaque asm. Teaching it
 * the edge with release-on-push / acquire-on-pop keyed on the block address
 * makes `JET_PERCPU=1` runs TSan-clean without a blanket suppression. Compiles
 * to nothing outside a TSan build. */
#if defined(__SANITIZE_THREAD__)
#  define JET_TSAN 1
#elif defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define JET_TSAN 1
#  endif
#endif
#if defined(JET_TSAN)
void __tsan_acquire(void*);
void __tsan_release(void*);
#  define JET_TSAN_ACQUIRE(p) __tsan_acquire(p)
#  define JET_TSAN_RELEASE(p) __tsan_release(p)
#else
#  define JET_TSAN_ACQUIRE(p) ((void)0)
#  define JET_TSAN_RELEASE(p) ((void)0)
#endif

#if JET_HAVE_RSEQ

/* Hot-path guard (see jet_internal.h). 0 until init arms the per-CPU cache. */
int jet_percpu_on = 0;

/* One cache line of state per (cpu, class): the LIFO head touched by rseq asm,
 * plus a relaxed depth counter for capacity bounding. Heads for one CPU are
 * laid out contiguously (head[0..NUM_CLASSES-1]) so the asm can index by
 * cls*8; the counters live in a parallel array to keep the head array a dense
 * pointer table (stride = NUM_CLASSES*8, exactly what the asm expects). */
typedef struct {
    void* head[JET_NUM_CLASSES];
} jet_cpu_slab;

/* Global per-CPU slab table + parallel depth counters. Allocated once at init
 * sized to the number of possible CPUs. */
static jet_cpu_slab*      g_slabs   = NULL;
static _Atomic(uint32_t)* g_depth   = NULL;   /* [ncpu * NUM_CLASSES]          */
static long               g_stride  = 0;      /* bytes per CPU (= sizeof slab)  */
static long               g_rseq_off = 0;     /* __rseq_offset snapshot         */
static int                g_ncpu    = 0;
static int                g_active  = 0;

/* Read the current CPU from the registered rseq area (cpu_id_start @ %fs off).
 * Only used for the depth counter index; the asm reads it independently for
 * the head. A tiny skew between the two is harmless (counter is advisory). */
static JET_ALWAYS_INLINE unsigned jet_cur_cpu(void) {
    unsigned cpu;
    /* struct rseq.cpu_id_start is at offset 0 of the rseq area. */
    __asm__ volatile("movl %%fs:0(%1), %0"
                     : "=r"(cpu)
                     : "r"(g_rseq_off)
                     : "memory");
    return cpu;
}

int jet_percpu_init(void) {
    /* Opt-in: the per-CPU cache is a WIN when threads greatly outnumber cores
     * (it eliminates per-thread cache memory blow-up and cross-thread frees
     * collapse to same-CPU pushes), but it is pure overhead stacked in front
     * of an already-lock-free per-thread tcache when thread count ≈ core
     * count. So it is off unless JET_PERCPU is set to a non-zero value. */
    const char* env = getenv("JET_PERCPU");
    if (!env || env[0] == '0' || env[0] == '\0') return 0;

    /* glibc registered rseq iff __rseq_size > 0. If it didn't, bail to tcache. */
    if (__rseq_size == 0) return 0;

    long n = sysconf(_SC_NPROCESSORS_CONF);
    if (n <= 0) n = 1;
    if (n > 4096) n = 4096;               /* sane upper bound                  */
    g_ncpu   = (int)n;
    g_stride = (long)sizeof(jet_cpu_slab);
    g_rseq_off = __rseq_offset;

    g_slabs = (jet_cpu_slab*)jet_os_map((size_t)g_ncpu * sizeof(jet_cpu_slab));
    if (!g_slabs) return 0;
    g_depth = (_Atomic(uint32_t)*)jet_os_map(
        (size_t)g_ncpu * JET_NUM_CLASSES * sizeof(uint32_t));
    if (!g_depth) { g_slabs = NULL; return 0; }
    /* jet_os_map returns zeroed pages, so all heads are NULL and depths 0. */

    g_active = 1;
    jet_percpu_on = 1;
    return 1;
}

int jet_percpu_active(void) { return g_active; }

void* jet_percpu_pop(int cls) {
    if (!g_active) return NULL;
    void* b = jet_rseq_pop((void*)g_slabs, g_stride,
                           (long)cls * (long)sizeof(void*), g_rseq_off);
    if (b) {
        /* Acquire: pair with the JET_TSAN_RELEASE in push so TSan sees the
         * freeing thread's writes happen-before our reuse of this block. */
        JET_TSAN_ACQUIRE(b);
        /* Decrement the depth counter for wherever we are now. Advisory. */
        unsigned cpu = jet_cur_cpu();
        if (cpu < (unsigned)g_ncpu) {
            _Atomic(uint32_t)* d = &g_depth[cpu * JET_NUM_CLASSES + cls];
            uint32_t cur = atomic_load_explicit(d, memory_order_relaxed);
            if (cur) atomic_store_explicit(d, cur - 1, memory_order_relaxed);
        }
    }
    return b;
}

int jet_percpu_push(int cls, void* block) {
    if (!g_active) return 0;
    unsigned cpu = jet_cur_cpu();
    if (cpu >= (unsigned)g_ncpu) return 0;
    _Atomic(uint32_t)* d = &g_depth[cpu * JET_NUM_CLASSES + cls];
    /* Capacity gate: refuse the push if this slab is already full so memory
     * stays bounded. Relaxed read is fine — the cap is soft. */
    if (atomic_load_explicit(d, memory_order_relaxed) >= JET_PERCPU_MAX)
        return 0;
    /* Release: publish this thread's writes to the block before it becomes
     * visible to a later popper (pairs with JET_TSAN_ACQUIRE in pop). */
    JET_TSAN_RELEASE(block);
    int ok = jet_rseq_push((void*)g_slabs, g_stride,
                           (long)cls * (long)sizeof(void*), g_rseq_off, block);
    if (ok) {
        /* Plain relaxed increment (not an RMW): the cap is soft, so a lost
         * update under a migration race only perturbs the bound slightly. */
        uint32_t cur = atomic_load_explicit(d, memory_order_relaxed);
        atomic_store_explicit(d, cur + 1, memory_order_relaxed);
    }
    return ok;
}

#else  /* !JET_HAVE_RSEQ — portable no-op stubs */

int jet_percpu_on = 0;
int   jet_percpu_init(void)          { return 0; }
int   jet_percpu_active(void)        { return 0; }
void* jet_percpu_pop(int cls)        { (void)cls; return NULL; }
int   jet_percpu_push(int cls, void* b) { (void)cls; (void)b; return 0; }

#endif /* JET_HAVE_RSEQ */
