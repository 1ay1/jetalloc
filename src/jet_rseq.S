/*
 * jetalloc — restartable-sequence (rseq) per-CPU LIFO primitives, x86-64.
 * SPDX-License-Identifier: MIT
 *
 * These two functions implement a lock-free, atomic-free per-CPU stack pop and
 * push using Linux restartable sequences (rseq(2)). The whole point: on the
 * common path an alloc/free touches ONLY the slab owned by the CPU the thread
 * is currently running on — no cross-thread atomics, no locks. If the kernel
 * migrates or preempts the thread anywhere inside the critical section, it
 * resets rseq_cs to NULL and jumps us to the abort label, which retries.
 *
 * This is exactly the mechanism tcmalloc's per-CPU cache and the kernel's own
 * rseq selftests use. glibc (>= 2.35) registers the per-thread `struct rseq`
 * for us and exposes __rseq_offset (offset from the thread pointer, %fs base).
 *
 * The per-CPU slab layout is a flat array of `void*` heads:
 *     head = base + cpu * stride + cls * 8
 * Callers pass `base` (the slab array), `stride` (bytes per CPU), and the byte
 * offset `cls*8`. We compute the current CPU from the rseq area itself
 * (cpu_id_start, at offset 0 of struct rseq).
 *
 * ABI (System V AMD64):
 *   void* jet_rseq_pop (void* base, long stride, long cls_off, long rseq_off);
 *          rdi=base    rsi=stride  rdx=cls_off  rcx=rseq_off
 *      returns popped block in rax, or NULL if the per-CPU list was empty
 *      (caller then takes the slow path). rseq_off = __rseq_offset.
 *
 *   int jet_rseq_push(void* base, long stride, long cls_off, long rseq_off,
 *                     void* block);
 *          rdi=base    rsi=stride  rdx=cls_off  rcx=rseq_off  r8=block
 *      returns 1 on success, 0 never (push always succeeds once it commits).
 */

#if defined(__x86_64__) && defined(__linux__)

/* struct rseq layout (see linux/rseq.h): cpu_id_start @0, cpu_id @4,
 * rseq_cs @8 (8 bytes). RSEQ_SIG is the required pre-abort signature. */
#define RSEQ_CPU_ID_START   0
#define RSEQ_CS_OFFSET      8
#define RSEQ_SIG            0x53053053

/* An rseq_cs descriptor (struct rseq_cs): version(u32), flags(u32),
 * start_ip(u64), post_commit_offset(u64), abort_ip(u64) — 32 bytes, 32-aligned.
 * We emit one per critical section into a dedicated section. */

	.text

/* ───────────────────────── jet_rseq_pop ─────────────────────────────── */
	.globl	jet_rseq_pop
	.type	jet_rseq_pop, @function
	.align	64
jet_rseq_pop:
	/* rdi=base rsi=stride rdx=cls_off rcx=rseq_off */
1:
	/* Register the critical section: store &__rseq_cs_pop into rseq.rseq_cs.
	 * The rseq area lives at %fs:(rcx) (rcx = __rseq_offset). */
	leaq	__rseq_cs_pop(%rip), %rax
	movq	%rax, %fs:RSEQ_CS_OFFSET(%rcx)
	/* ---- critical section start (start_ip) ---- */
.Lpop_start:
	/* cpu = rseq.cpu_id_start */
	movl	%fs:RSEQ_CPU_ID_START(%rcx), %eax
	/* head_slot = base + cpu*stride + cls_off */
	imulq	%rsi, %rax
	addq	%rdi, %rax
	addq	%rdx, %rax          /* rax = &slab[cpu].head[cls] */
	movq	(%rax), %r9         /* r9 = head */
	testq	%r9, %r9
	jz	.Lpop_empty         /* empty list → return NULL (slow path) */
	movq	(%r9), %r10         /* r10 = *head = next */
	/* COMMIT: publish next as the new head. This single store is the point
	 * the kernel guarantees is atomic w.r.t. preemption/migration. */
	movq	%r10, (%rax)
.Lpop_commit:
	/* ---- critical section end (post_commit_ip) ---- */
	movq	$0, %fs:RSEQ_CS_OFFSET(%rcx)   /* clear rseq_cs */
	movq	%r9, %rax           /* return popped block */
	ret
.Lpop_empty:
	movq	$0, %fs:RSEQ_CS_OFFSET(%rcx)
	xorl	%eax, %eax
	ret

	/* Abort handler: kernel jumps here on preempt/migrate/signal inside the
	 * CS. The 4-byte RSEQ_SIG must immediately precede the landing label. */
	.align	16
	.long	RSEQ_SIG
.Lpop_abort:
	jmp	1b
	.size	jet_rseq_pop, .-jet_rseq_pop

/* ───────────────────────── jet_rseq_push ────────────────────────────── */
	.globl	jet_rseq_push
	.type	jet_rseq_push, @function
	.align	64
jet_rseq_push:
	/* rdi=base rsi=stride rdx=cls_off rcx=rseq_off r8=block */
2:
	leaq	__rseq_cs_push(%rip), %rax
	movq	%rax, %fs:RSEQ_CS_OFFSET(%rcx)
.Lpush_start:
	movl	%fs:RSEQ_CPU_ID_START(%rcx), %eax
	imulq	%rsi, %rax
	addq	%rdi, %rax
	addq	%rdx, %rax          /* rax = &slab[cpu].head[cls] */
	movq	(%rax), %r9         /* r9 = old head */
	movq	%r9, (%r8)          /* block->next = old head (prepare) */
	/* COMMIT: publish block as the new head. */
	movq	%r8, (%rax)
.Lpush_commit:
	movq	$0, %fs:RSEQ_CS_OFFSET(%rcx)
	movl	$1, %eax
	ret

	.align	16
	.long	RSEQ_SIG
.Lpush_abort:
	jmp	2b
	.size	jet_rseq_push, .-jet_rseq_push

/* ─────────────────── rseq_cs descriptors (read-only) ─────────────────── */
	.section	__rseq_cs, "aw", @progbits
	.align	32
__rseq_cs_pop:
	.long	0                        /* version */
	.long	0                        /* flags   */
	.quad	.Lpop_start              /* start_ip */
	.quad	.Lpop_commit - .Lpop_start   /* post_commit_offset */
	.quad	.Lpop_abort              /* abort_ip */

	.align	32
__rseq_cs_push:
	.long	0
	.long	0
	.quad	.Lpush_start
	.quad	.Lpush_commit - .Lpush_start
	.quad	.Lpush_abort

/* Mark the stack as non-executable. */
	.section	.note.GNU-stack, "", @progbits

#endif /* __x86_64__ && __linux__ */
