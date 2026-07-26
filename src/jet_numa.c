/*
 * jetalloc — NUMA / topology-aware span placement.
 * SPDX-License-Identifier: MIT
 *
 * On a multi-socket machine, memory has a "home" node and accessing a remote
 * node's DRAM costs 1.5-2x the latency and less bandwidth. The allocator can
 * cut that by binding each freshly-mapped span to the NUMA node of the CPU that
 * asked for it, so a thread's allocations live in its local DRAM. This is what
 * tcmalloc/jemalloc's NUMA-aware arenas do; we get 90% of the benefit with a
 * single mbind(2) per span and zero libnuma dependency.
 *
 * SELF-DISABLING. On a single-node system (the common case, including this dev
 * box) there is nothing to bind — every page is already local — so the whole
 * layer collapses to one predicted-not-taken branch (`g_numa_on == 0`) and
 * never issues a syscall. We detect the node count once at init from
 * /sys/devices/system/node and only arm binding when it exceeds 1.
 *
 * BEST-EFFORT. We bind with MPOL_PREFERRED (a hint, not MPOL_BIND), so if the
 * local node is full the kernel silently falls back to another node rather than
 * failing the allocation. First-touch then places the physical pages on the
 * preferred node as the span is written. mbind failures are ignored: NUMA
 * placement is an optimisation, never a correctness requirement.
 */
#include "jet_internal.h"

#if defined(__linux__)
#  define JET_HAVE_NUMA 1
#  include <unistd.h>
#  include <sched.h>
#  include <sys/syscall.h>
#  include <stdlib.h>
#  include <fcntl.h>
#  include <string.h>
#else
#  define JET_HAVE_NUMA 0
#endif

#if JET_HAVE_NUMA

/* Armed only when the machine has >1 NUMA node AND JET_NUMA isn't disabled. */
static int g_numa_on    = 0;
static int g_numa_nodes = 1;

/* Count online NUMA nodes by scanning /sys/devices/system/node/nodeN. Cheap,
 * done once. Returns >=1. */
static int numa_count_nodes(void) {
    int n = 0;
    for (int i = 0; i < 4096; ++i) {
        char path[64];
        /* Build "/sys/devices/system/node/nodeI" without snprintf pulling in
         * locale machinery on the hot path — this is cold init, plain is fine. */
        int len = 0;
        const char* base = "/sys/devices/system/node/node";
        for (const char* p = base; *p; ++p) path[len++] = *p;
        /* itoa */
        char tmp[12]; int tl = 0, v = i;
        if (v == 0) tmp[tl++] = '0';
        while (v) { tmp[tl++] = (char)('0' + v % 10); v /= 10; }
        while (tl) path[len++] = tmp[--tl];
        path[len] = '\0';
        if (access(path, F_OK) != 0) break;
        ++n;
    }
    return n < 1 ? 1 : n;
}

void jet_numa_init(void) {
    const char* env = getenv("JET_NUMA");
    /* Default ON where it helps; JET_NUMA=0 forces it off. */
    if (env && (env[0] == '0' || env[0] == '\0')) { g_numa_on = 0; return; }
    g_numa_nodes = numa_count_nodes();
    g_numa_on = (g_numa_nodes > 1);
}

#if defined(__GNUC__) || defined(__clang__)
/* Detect topology before the first span is ever mapped (which happens inside
 * the allocator's own bootstrap, so a lazy init would be too late / racy). */
__attribute__((constructor))
static void jet_numa_ctor(void) { jet_numa_init(); }
#endif

int jet_numa_active(void) { return g_numa_on; }

/* The NUMA node the calling thread is currently running on, or -1 if unknown.
 * getcpu(2) writes the node id through its second argument. */
static int numa_current_node(void) {
    unsigned cpu = 0, node = 0;
#if defined(SYS_getcpu)
    if (syscall(SYS_getcpu, &cpu, &node, NULL) != 0) return -1;
    return (int)node;
#else
    (void)cpu; (void)node;
    return -1;
#endif
}

/* Bind [addr, addr+len) to prefer the current thread's local NUMA node.
 * Best-effort: any failure is ignored (placement is an optimisation). No-op
 * unless the machine actually has multiple nodes. */
void jet_numa_bind_local(void* addr, size_t len) {
    if (!g_numa_on) return;
    int node = numa_current_node();
    if (node < 0) return;

    /* mbind(addr, len, MPOL_PREFERRED, nodemask, maxnode, flags). The nodemask
     * is a bit array of unsigned longs; set the one bit for `node`. maxnode is
     * the number of bits the kernel should scan (node+1, rounded up). */
    unsigned long mask[ (4096 + 8*sizeof(unsigned long) - 1) /
                        (8*sizeof(unsigned long)) ] = {0};
    if (node >= (int)(sizeof(mask) * 8)) return;
    mask[node / (int)(8*sizeof(unsigned long))] |=
        1UL << (node % (int)(8*sizeof(unsigned long)));
    unsigned long maxnode = (unsigned long)node + 2;   /* bits to consider     */

#if defined(SYS_mbind)
    /* MPOL_PREFERRED == 1 (see linux/mempolicy.h). Passing it inline avoids a
     * header dependency; the value is stable kernel ABI. */
    (void)syscall(SYS_mbind, addr, len, /*MPOL_PREFERRED*/ 1,
                  mask, maxnode, /*flags*/ 0);
#else
    (void)addr; (void)len;
#endif
}

#else  /* !JET_HAVE_NUMA — portable no-ops */

void jet_numa_init(void)                        {}
int  jet_numa_active(void)                      { return 0; }
void jet_numa_bind_local(void* a, size_t l)     { (void)a; (void)l; }

#endif /* JET_HAVE_NUMA */
