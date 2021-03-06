/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2010 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/*
 * Copyright 2015, Joyent, Inc. All rights reserved.
 */

/*
 * The LX Brand: emulation of a Linux operating environment within a zone.
 *
 * OVERVIEW
 *
 * The LX brand enables a full Linux userland -- including a C library,
 * init(1) framework, and some set of applications -- to run unmodified
 * within an illumos zone.  Unlike illumos, where applications are expected
 * to link against and consume functions exported from libraries, the
 * supported Linux binary compatibility boundary is the system call
 * interface.  By accurately emulating the behaviour of Linux system calls,
 * Linux software can be executed in this environment as if it were running
 * on a native Linux system.
 *
 * EMULATING LINUX SYSTEM CALLS
 *
 * Linux system calls are made in 32-bit processes via the "int 0x80"
 * instruction; in 64-bit processes the "syscall" instruction is used, as it
 * is with native illumos processes.  In both cases, arguments to system
 * calls are generally passed in registers and the usermode stack is not
 * interpreted or modified by the Linux kernel.
 *
 * When the emulated Linux process makes a system call, it traps into the
 * illumos kernel.  The in-kernel brand module contains various emulation
 * routines, and can fully service some emulated system calls; e.g. read(2)
 * and write(2).  Other system calls require assistance from the illumos
 * libc, bouncing back out to the brand library ("lx_brand.so.1") for
 * emulation.
 *
 * The brand mechanism allows for the provision of an alternative trap
 * handler for the various system call mechanisms.  Traditionally this was
 * used to immediately revector execution to the usermode emulation library,
 * which was responsible for handling all system calls.  In the interests of
 * more accurate emulation and increased performance, much of the regular
 * illumos system call path is now invoked.  Only the argument processing and
 * handler dispatch are replaced by the brand, via the per-LWP
 * "lwp_brand_syscall" interposition function pointer.
 *
 * THE NATIVE AND BRAND STACKS
 *
 * Some runtime environments (e.g. the Go language) allocate very small
 * thread stacks, preferring to grow or split the stack as necessary.  The
 * Linux kernel generally does not use the usermode stack when servicing
 * system calls, so this is not a problem.  In order for our emulation to
 * have the same zero stack impact, we must execute usermode emulation
 * routines on an _alternate_ stack.  This is similar, in principle, to the
 * use of sigaltstack(3C) to run signal handlers off the main thread stack.
 *
 * To this end, the brand library allocates and installs an alternate stack
 * (called the "native" stack) for each LWP.  The in-kernel brand code uses
 * this stack for usermode emulation calls and interposed signal delivery,
 * while the emulated Linux process sees only the data on the main thread
 * stack, known as the "brand" stack.  The stack mode is tracked in the
 * per-LWP brand-private data, using the LX_STACK_MODE_* enum.
 *
 * The stack mode doubles as a system call "mode bit".  When in the
 * LX_STACK_MODE_BRAND mode, system calls are processed as emulated Linux
 * system calls.  In other modes, system calls are assumed to be native
 * illumos system calls as made during brand library initialisation and
 * usermode emulation.
 *
 * USERMODE EMULATION
 *
 * When a Linux system call cannot be emulated within the kernel, we preserve
 * the register state of the Linux process and revector the LWP to the brand
 * library usermode emulation handler: the "lx_emulate()" function in
 * "lx_brand.so.1".  This revectoring is modelled on the delivery of signals,
 * and is performed in "lx_emulate_user()".
 *
 * First, the emulated process state is written out to the usermode stack of
 * the process as a "ucontext_t" object.  Arguments to the emulation routine
 * are passed on the stack or in registers, depending on the ABI.  When the
 * usermode emulation is complete, the result is passed back to the kernel
 * (via the "B_EMULATION_DONE" brandsys subcommand) with the saved context
 * for restoration.
 *
 * SIGNAL DELIVERY, SETCONTEXT AND GETCONTEXT
 *
 * When servicing emulated system calls in the usermode brand library, or
 * during signal delivery, various state is preserved by the kernel so that
 * the running LWP may be revectored to a handling routine.  The context
 * allows the kernel to restart the program at the point of interruption,
 * either at the return of the signal handler, via setcontext(3C); or after
 * the usermode emulation request has been serviced, via B_EMULATION_DONE.
 *
 * In illumos native processes, the saved context (a "ucontext_t" object)
 * includes the state of registers and the current signal mask at the point
 * of interruption.  The context also includes a link to the most recently
 * saved context, forming a chain to be unwound as requests complete.  The LX
 * brand requires additional book-keeping to describe the machine state: in
 * particular, the current stack mode and the occupied extent of the native
 * stack.
 *
 * The brand code is able to interpose on the context save and restore
 * operations in the kernel -- see "lx_savecontext()" and
 * "lx_restorecontext()" -- to enable getcontext(3C) and setcontext(3C) to
 * function correctly in the face of a dual stack LWP.  The brand also
 * interposes on the signal delivery mechanism -- see "lx_sendsig()" and
 * "lx_sendsig_stack()" -- to allow all signals to be delivered to the brand
 * library interposer on the native stack, regardless of the interrupted
 * execution mode.  Linux sigaltstack(2) emulation is performed entirely by
 * the usermode brand library during signal handler interposition.
 */

#include <sys/types.h>
#include <sys/kmem.h>
#include <sys/errno.h>
#include <sys/thread.h>
#include <sys/systm.h>
#include <sys/syscall.h>
#include <sys/proc.h>
#include <sys/modctl.h>
#include <sys/cmn_err.h>
#include <sys/model.h>
#include <sys/exec.h>
#include <sys/lx_impl.h>
#include <sys/machbrand.h>
#include <sys/lx_syscalls.h>
#include <sys/lx_misc.h>
#include <sys/lx_futex.h>
#include <sys/lx_brand.h>
#include <sys/param.h>
#include <sys/termios.h>
#include <sys/sunddi.h>
#include <sys/ddi.h>
#include <sys/vnode.h>
#include <sys/pathname.h>
#include <sys/auxv.h>
#include <sys/priv.h>
#include <sys/regset.h>
#include <sys/privregs.h>
#include <sys/archsystm.h>
#include <sys/zone.h>
#include <sys/brand.h>
#include <sys/sdt.h>
#include <sys/x86_archext.h>
#include <sys/controlregs.h>
#include <sys/core.h>
#include <sys/stack.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <lx_signum.h>
#include <util/sscanf.h>

int	lx_debug = 0;

void	lx_init_brand_data(zone_t *);
void	lx_free_brand_data(zone_t *);
void	lx_setbrand(proc_t *);
int	lx_getattr(zone_t *, int, void *, size_t *);
int	lx_setattr(zone_t *, int, void *, size_t);
int	lx_brandsys(int, int64_t *, uintptr_t, uintptr_t, uintptr_t,
		uintptr_t, uintptr_t);
void	lx_set_kern_version(zone_t *, char *);
void	lx_copy_procdata(proc_t *, proc_t *);

extern int getsetcontext(int, void *);
extern int waitsys(idtype_t, id_t, siginfo_t *, int);
#if defined(_SYSCALL32_IMPL)
extern int getsetcontext32(int, void *);
extern int waitsys32(idtype_t, id_t, siginfo_t *, int);
#endif

extern void lx_proc_exit(proc_t *);
extern int lx_sched_affinity(int, uintptr_t, int, uintptr_t, int64_t *);

extern void lx_ioctl_init();
extern void lx_ioctl_fini();
extern void lx_socket_init();
extern void lx_socket_fini();

lx_systrace_f *lx_systrace_entry_ptr;
lx_systrace_f *lx_systrace_return_ptr;

static int lx_systrace_enabled;

/*
 * cgroup file system maintenance functions which are set when cgroups loads.
 */
void (*lx_cgrp_forklwp)(vfs_t *, uint_t, pid_t);
void (*lx_cgrp_proc_exit)(vfs_t *, uint_t, pid_t);
void (*lx_cgrp_initlwp)(vfs_t *, uint_t, id_t, pid_t);
void (*lx_cgrp_freelwp)(vfs_t *, uint_t, id_t, pid_t);

/*
 * While this is effectively mmu.hole_start - PAGESIZE, we don't particularly
 * want an MMU dependency here (and should there be a microprocessor without
 * a hole, we don't want to start allocating from the top of the VA range).
 */
#define	LX_MAXSTACK64	0x7ffffff00000

uint64_t lx_maxstack64 = LX_MAXSTACK64;

static int lx_elfexec(struct vnode *vp, struct execa *uap, struct uarg *args,
    struct intpdata *idata, int level, long *execsz, int setid,
    caddr_t exec_file, struct cred *cred, int *brand_action);

static boolean_t lx_native_exec(uint8_t, const char **);
static uint32_t lx_map32limit(proc_t *);

static void lx_savecontext(ucontext_t *);
static void lx_restorecontext(ucontext_t *);
static caddr_t lx_sendsig_stack(int);
static void lx_sendsig(int);
#if defined(_SYSCALL32_IMPL)
static void lx_savecontext32(ucontext32_t *);
#endif
static int lx_setid_clear(vattr_t *, cred_t *);
#if defined(_LP64)
static int lx_pagefault(proc_t *, klwp_t *, caddr_t, enum fault_type,
    enum seg_rw);
#endif


/* lx brand */
struct brand_ops lx_brops = {
	lx_init_brand_data,		/* b_init_brand_data */
	lx_free_brand_data,		/* b_free_brand_data */
	lx_brandsys,			/* b_brandsys */
	lx_setbrand,			/* b_setbrand */
	lx_getattr,			/* b_getattr */
	lx_setattr,			/* b_setattr */
	lx_copy_procdata,		/* b_copy_procdata */
	lx_proc_exit,			/* b_proc_exit */
	lx_exec,			/* b_exec */
	lx_setrval,			/* b_lwp_setrval */
	lx_lwpdata_alloc,		/* b_lwpdata_alloc */
	lx_lwpdata_free,		/* b_lwpdata_free */
	lx_initlwp,			/* b_initlwp */
	lx_initlwp_post,		/* b_initlwp_post */
	lx_forklwp,			/* b_forklwp */
	lx_freelwp,			/* b_freelwp */
	lx_exitlwp,			/* b_lwpexit */
	lx_elfexec,			/* b_elfexec */
	NULL,				/* b_sigset_native_to_brand */
	NULL,				/* b_sigset_brand_to_native */
	lx_sigfd_translate,		/* b_sigfd_translate */
	NSIG,				/* b_nsig */
	lx_exit_with_sig,		/* b_exit_with_sig */
	lx_wait_filter,			/* b_wait_filter */
	lx_native_exec,			/* b_native_exec */
	lx_map32limit,			/* b_map32limit */
	lx_stop_notify,			/* b_stop_notify */
	lx_waitid_helper,		/* b_waitid_helper */
	lx_sigcld_repost,		/* b_sigcld_repost */
	lx_ptrace_issig_stop,		/* b_issig_stop */
	lx_ptrace_sig_ignorable,	/* b_sig_ignorable */
	lx_savecontext,			/* b_savecontext */
#if defined(_SYSCALL32_IMPL)
	lx_savecontext32,		/* b_savecontext32 */
#endif
	lx_restorecontext,		/* b_restorecontext */
	lx_sendsig_stack,		/* b_sendsig_stack */
	lx_sendsig,			/* b_sendsig */
	lx_setid_clear,			/* b_setid_clear */
#if defined(_LP64)
	lx_pagefault			/* b_pagefault */
#else
	NULL
#endif
};

struct brand_mach_ops lx_mops = {
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	lx_fixsegreg,
	lx_fsbase
};

struct brand lx_brand = {
	BRAND_VER_1,
	"lx",
	&lx_brops,
	&lx_mops,
	sizeof (struct lx_proc_data)
};

static struct modlbrand modlbrand = {
	&mod_brandops, "lx brand", &lx_brand
};

static struct modlinkage modlinkage = {
	MODREV_1, (void *)&modlbrand, NULL
};

void
lx_proc_exit(proc_t *p)
{
	lx_proc_data_t *lxpd;
	proc_t *cp;
	lx_zone_data_t *lxzdata;

	/* cgroup integration */
	lxzdata = ztolxzd(p->p_zone);
	if (lxzdata->lxzd_cgroup != NULL) {
		lx_lwp_data_t *lwpd = lwptolxlwp(ttolwp(curthread));
		ASSERT(lx_cgrp_proc_exit != NULL);
		(*lx_cgrp_proc_exit)(lxzdata->lxzd_cgroup,
		    lwpd->br_cgroupid, p->p_pid);
	}

	mutex_enter(&p->p_lock);
	VERIFY(lxpd = ptolxproc(p));
	VERIFY(lxpd->l_ptrace == 0);
	if ((lxpd->l_flags & LX_PROC_CHILD_DEATHSIG) == 0) {
		mutex_exit(&p->p_lock);
		return;
	}
	mutex_exit(&p->p_lock);

	/* Check for children which desire notification of parental death. */
	mutex_enter(&pidlock);
	for (cp = p->p_child; cp != NULL; cp = cp->p_sibling) {
		mutex_enter(&cp->p_lock);
		if ((lxpd = ptolxproc(cp)) == NULL) {
			mutex_exit(&cp->p_lock);
			continue;
		}
		if (lxpd->l_parent_deathsig != 0) {
			sigtoproc(cp, NULL, lxpd->l_parent_deathsig);
		}
		mutex_exit(&cp->p_lock);
	}
	mutex_exit(&pidlock);
}

void
lx_setbrand(proc_t *p)
{
	/* Send SIGCHLD to parent by default when child exits */
	ptolxproc(p)->l_signal = stol_signo[SIGCHLD];

	lx_read_argv_bounds(p);
}

/* ARGSUSED */
int
lx_setattr(zone_t *zone, int attr, void *ubuf, size_t ubufsz)
{
	lx_zone_data_t *lxzd = (lx_zone_data_t *)zone->zone_brand_data;

	switch (attr) {
	case LX_ATTR_KERN_RELEASE: {
		char buf[LX_KERN_RELEASE_MAX];
		bzero(buf, LX_KERN_RELEASE_MAX);
		if (ubufsz >= LX_KERN_RELEASE_MAX) {
			return (ERANGE);
		}
		if (copyin(ubuf, buf, ubufsz) != 0) {
			return (EFAULT);
		}
		(void) strlcpy(lxzd->lxzd_kernel_release, buf,
		    LX_KERN_RELEASE_MAX);
		return (0);
	}
	case LX_ATTR_KERN_VERSION: {
		char buf[LX_KERN_VERSION_MAX];
		bzero(buf, LX_KERN_VERSION_MAX);
		if (ubufsz >= LX_KERN_VERSION_MAX) {
			return (ERANGE);
		}
		if (copyin(ubuf, buf, ubufsz) != 0) {
			return (EFAULT);
		}
		(void) strlcpy(lxzd->lxzd_kernel_version, buf,
		    LX_KERN_VERSION_MAX);
		return (0);
	}
	default:
		return (EINVAL);
	}
}

/* ARGSUSED */
int
lx_getattr(zone_t *zone, int attr, void *ubuf, size_t *ubufsz)
{
	lx_zone_data_t *lxzd = (lx_zone_data_t *)zone->zone_brand_data;
	int len;

	switch (attr) {
	case LX_ATTR_KERN_RELEASE: {
		len = strnlen(lxzd->lxzd_kernel_release, LX_KERN_RELEASE_MAX);
		len++;
		if (*ubufsz < len) {
			return (ERANGE);
		}
		if (copyout(lxzd->lxzd_kernel_release, ubuf, len) != 0) {
			return (EFAULT);
		}
		*ubufsz = len;
		return (0);
	}
	case LX_ATTR_KERN_VERSION: {
		len = strnlen(lxzd->lxzd_kernel_version, LX_KERN_VERSION_MAX);
		len++;
		if (*ubufsz < len) {
			return (ERANGE);
		}
		if (copyout(lxzd->lxzd_kernel_version, ubuf, len) != 0) {
			return (EFAULT);
		}
		*ubufsz = len;
		return (0);
	}
	default:
		return (EINVAL);
	}
}

uint32_t
lx_map32limit(proc_t *p)
{
	/*
	 * To be bug-for-bug compatible with Linux, we have MAP_32BIT only
	 * allow mappings in the first 31 bits.  This was a nuance in the
	 * original Linux implementation circa 2002, and applications have
	 * come to depend on its behavior.
	 *
	 * This is only relevant for 64-bit processes.
	 */
	if (p->p_model == DATAMODEL_LP64)
		return (1 << 31);

	return ((uint32_t)USERLIMIT32);
}

void
lx_brand_systrace_enable(void)
{
	VERIFY(!lx_systrace_enabled);

	lx_systrace_enabled = 1;
}

void
lx_brand_systrace_disable(void)
{
	VERIFY(lx_systrace_enabled);

	lx_systrace_enabled = 0;
}

void
lx_lwp_set_native_stack_current(lx_lwp_data_t *lwpd, uintptr_t new_sp)
{
	VERIFY(lwpd->br_ntv_stack != 0);

	/*
	 * The "brand-lx-set-ntv-stack-current" probe has arguments:
	 *   arg0: stack pointer before change
	 *   arg1: stack pointer after change
	 *   arg2: current stack base
	 */
	DTRACE_PROBE3(brand__lx__set__ntv__stack__current,
	    uintptr_t, lwpd->br_ntv_stack_current,
	    uintptr_t, new_sp,
	    uintptr_t, lwpd->br_ntv_stack);

	lwpd->br_ntv_stack_current = new_sp;
}

#if defined(_LP64)
static int
lx_pagefault(proc_t *p, klwp_t *lwp, caddr_t addr, enum fault_type type,
    enum seg_rw rw)
{
	int syscall_num;

	/*
	 * We only want to handle a very specific set of circumstances.
	 * Namely: this is a 64-bit LX-branded process attempting to execute an
	 * address in a page for which it does not have a valid mapping.  If
	 * this is not the case, we bail out as fast as possible.
	 */
	VERIFY(PROC_IS_BRANDED(p));
	if (type != F_INVAL || rw != S_EXEC || lwp_getdatamodel(lwp) !=
	    DATAMODEL_NATIVE) {
		return (-1);
	}

	if (!lx_vsyscall_iscall(lwp, (uintptr_t)addr, &syscall_num)) {
		return (-1);
	}

	/*
	 * This is a valid vsyscall address.  We service the system call and
	 * return 0 to signal that the pagefault has been handled completely.
	 */
	lx_vsyscall_enter(p, lwp, syscall_num);
	return (0);
}
#endif

/*
 * This hook runs prior to sendsig() processing and allows us to nominate
 * an alternative stack pointer for delivery of the signal handling frame.
 * Critically, this routine should _not_ modify any LWP state as the
 * savecontext() does not run until after this hook.
 */
static caddr_t
lx_sendsig_stack(int sig)
{
	klwp_t *lwp = ttolwp(curthread);
	lx_lwp_data_t *lwpd = lwptolxlwp(lwp);

	/*
	 * We want to take signal delivery on the native stack, but only if
	 * one has been allocated and installed for this LWP.
	 */
	if (lwpd->br_stack_mode == LX_STACK_MODE_BRAND) {
		/*
		 * The program is not running on the native stack.  Return
		 * the native stack pointer from our brand-private data so
		 * that we may switch to it for signal handling.
		 */
		return ((caddr_t)lwpd->br_ntv_stack_current);
	} else {
		struct regs *rp = lwptoregs(lwp);

		/*
		 * Either the program is already running on the native stack,
		 * or one has not yet been allocated for this LWP.  Use the
		 * current stack pointer value.
		 */
		return ((caddr_t)rp->r_sp);
	}
}

/*
 * This hook runs after sendsig() processing and allows us to update the
 * per-LWP mode flags for system calls and stacks.  The pre-signal
 * context has already been saved and delivered to the user at this point.
 */
static void
lx_sendsig(int sig)
{
	klwp_t *lwp = ttolwp(curthread);
	lx_lwp_data_t *lwpd = lwptolxlwp(lwp);
	struct regs *rp = lwptoregs(lwp);

	switch (lwpd->br_stack_mode) {
	case LX_STACK_MODE_BRAND:
	case LX_STACK_MODE_NATIVE:
		/*
		 * In lx_sendsig_stack(), we nominated a stack pointer from the
		 * native stack.  Update the stack mode, and the current in-use
		 * extent of the native stack, accordingly:
		 */
		lwpd->br_stack_mode = LX_STACK_MODE_NATIVE;
		lx_lwp_set_native_stack_current(lwpd, rp->r_sp);

		/*
		 * Fix up segment registers, etc.
		 */
		lx_switch_to_native(lwp);
		break;

	default:
		/*
		 * Otherwise, the brand library has not yet installed the
		 * alternate stack for this LWP.  Signals will be handled on
		 * the regular stack thread.
		 */
		return;
	}
}

/*
 * This hook runs prior to the context restoration, allowing us to take action
 * or modify the context before it is loaded.
 */
static void
lx_restorecontext(ucontext_t *ucp)
{
	klwp_t *lwp = ttolwp(curthread);
	lx_lwp_data_t *lwpd = lwptolxlwp(lwp);
	uintptr_t flags = (uintptr_t)ucp->uc_brand_data[0];
	caddr_t sp = ucp->uc_brand_data[1];

	/*
	 * We have a saved native stack pointer value that we must restore
	 * into the per-LWP data.
	 */
	if (flags & LX_UC_RESTORE_NATIVE_SP) {
		lx_lwp_set_native_stack_current(lwpd, (uintptr_t)sp);
	}

	/*
	 * We do not wish to restore the value of uc_link in this context,
	 * so replace it with the value currently in the LWP.
	 */
	if (flags & LX_UC_IGNORE_LINK) {
		ucp->uc_link = (ucontext_t *)lwp->lwp_oldcontext;
	}

	/*
	 * Restore the stack mode:
	 */
	if (flags & LX_UC_STACK_NATIVE) {
		lwpd->br_stack_mode = LX_STACK_MODE_NATIVE;
	} else if (flags & LX_UC_STACK_BRAND) {
		lwpd->br_stack_mode = LX_STACK_MODE_BRAND;
	}

#if defined(__amd64)
	/*
	 * Override the fs/gsbase in the context with the value provided
	 * through the Linux arch_prctl(2) system call.
	 */
	if (flags & LX_UC_STACK_BRAND) {
		if (lwpd->br_lx_fsbase != 0) {
			ucp->uc_mcontext.gregs[REG_FSBASE] = lwpd->br_lx_fsbase;
		}
		if (lwpd->br_lx_gsbase != 0) {
			ucp->uc_mcontext.gregs[REG_GSBASE] = lwpd->br_lx_gsbase;
		}
	}
#endif
}

static void
lx_savecontext(ucontext_t *ucp)
{
	klwp_t *lwp = ttolwp(curthread);
	lx_lwp_data_t *lwpd = lwptolxlwp(lwp);
	uintptr_t flags = 0;

	/*
	 * The ucontext_t affords us three private pointer-sized members in
	 * "uc_brand_data".  We pack a variety of flags into the first element,
	 * and an optional stack pointer in the second element.  The flags
	 * determine which stack pointer (native or brand), if any, is stored
	 * in the second element.  The third element may contain the system
	 * call number; this is analogous to the "orig_[er]ax" member of a
	 * Linux "user_regs_struct".
	 */

	if (lwpd->br_stack_mode != LX_STACK_MODE_INIT &&
	    lwpd->br_stack_mode != LX_STACK_MODE_PREINIT) {
		/*
		 * Record the value of the native stack pointer to restore
		 * when returning to this branded context:
		 */
		flags |= LX_UC_RESTORE_NATIVE_SP;
		ucp->uc_brand_data[1] = (void *)lwpd->br_ntv_stack_current;
	}

	/*
	 * Save the stack mode:
	 */
	if (lwpd->br_stack_mode == LX_STACK_MODE_NATIVE) {
		flags |= LX_UC_STACK_NATIVE;
	} else if (lwpd->br_stack_mode == LX_STACK_MODE_BRAND) {
		flags |= LX_UC_STACK_BRAND;
	}

	/*
	 * If we might need to restart this system call, save that information
	 * in the context:
	 */
	if (lwpd->br_stack_mode == LX_STACK_MODE_BRAND) {
		ucp->uc_brand_data[2] =
		    (void *)(uintptr_t)lwpd->br_syscall_num;
		if (lwpd->br_syscall_restart) {
			flags |= LX_UC_RESTART_SYSCALL;
		}
	} else {
		ucp->uc_brand_data[2] = NULL;
	}

	ucp->uc_brand_data[0] = (void *)flags;
}

#if defined(_SYSCALL32_IMPL)
static void
lx_savecontext32(ucontext32_t *ucp)
{
	klwp_t *lwp = ttolwp(curthread);
	lx_lwp_data_t *lwpd = lwptolxlwp(lwp);
	unsigned int flags = 0;

	/*
	 * The ucontext_t affords us three private pointer-sized members in
	 * "uc_brand_data".  We pack a variety of flags into the first element,
	 * and an optional stack pointer in the second element.  The flags
	 * determine which stack pointer (native or brand), if any, is stored
	 * in the second element.  The third element may contain the system
	 * call number; this is analogous to the "orig_[er]ax" member of a
	 * Linux "user_regs_struct".
	 */

	if (lwpd->br_stack_mode != LX_STACK_MODE_INIT &&
	    lwpd->br_stack_mode != LX_STACK_MODE_PREINIT) {
		/*
		 * Record the value of the native stack pointer to restore
		 * when returning to this branded context:
		 */
		flags |= LX_UC_RESTORE_NATIVE_SP;
		ucp->uc_brand_data[1] = (caddr32_t)lwpd->br_ntv_stack_current;
	}

	/*
	 * Save the stack mode:
	 */
	if (lwpd->br_stack_mode == LX_STACK_MODE_NATIVE) {
		flags |= LX_UC_STACK_NATIVE;
	} else if (lwpd->br_stack_mode == LX_STACK_MODE_BRAND) {
		flags |= LX_UC_STACK_BRAND;
	}

	/*
	 * If we might need to restart this system call, save that information
	 * in the context:
	 */
	if (lwpd->br_stack_mode == LX_STACK_MODE_BRAND) {
		ucp->uc_brand_data[2] = (caddr32_t)lwpd->br_syscall_num;
		if (lwpd->br_syscall_restart) {
			flags |= LX_UC_RESTART_SYSCALL;
		}
	} else {
		ucp->uc_brand_data[2] = NULL;
	}

	ucp->uc_brand_data[0] = flags;
}
#endif

void
lx_init_brand_data(zone_t *zone)
{
	lx_zone_data_t *data;
	ASSERT(zone->zone_brand == &lx_brand);
	ASSERT(zone->zone_brand_data == NULL);
	data = (lx_zone_data_t *)kmem_zalloc(sizeof (lx_zone_data_t), KM_SLEEP);
	/*
	 * Set the default lxzd_kernel_version to 2.4.
	 * This can be changed by a call to setattr() during zone boot.
	 */
	(void) strlcpy(data->lxzd_kernel_release, "2.4.21",
	    LX_KERN_RELEASE_MAX);
	(void) strlcpy(data->lxzd_kernel_version, "BrandZ virtual linux",
	    LX_KERN_VERSION_MAX);

	/*
	 * Linux is not at all picky about address family when it comes to
	 * supporting interface-related ioctls.  To mimic this behavior, we'll
	 * attempt those ioctls against a ksocket configured for that purpose.
	 */
	(void) ksocket_socket(&data->lxzd_ioctl_sock, AF_INET, SOCK_DGRAM, 0,
	    0, zone->zone_kcred);

	zone->zone_brand_data = data;

	/*
	 * In Linux, if the init(1) process terminates the system panics.
	 * The zone must reboot to simulate this behaviour.
	 */
	zone->zone_reboot_on_init_exit = B_TRUE;
}

void
lx_free_brand_data(zone_t *zone)
{
	lx_zone_data_t *data = ztolxzd(zone);
	ASSERT(data != NULL);
	if (data->lxzd_ioctl_sock != NULL) {
		/*
		 * Since zone_kcred has been cleaned up already, close the
		 * socket using the global kcred.
		 */
		ksocket_close(data->lxzd_ioctl_sock, kcred);
		data->lxzd_ioctl_sock = NULL;
	}
	zone->zone_brand_data = NULL;
	kmem_free(data, sizeof (*data));
}

void
lx_unsupported(char *dmsg)
{
	lx_proc_data_t *pd = ttolxproc(curthread);

	DTRACE_PROBE1(brand__lx__unsupported, char *, dmsg);

	if (pd != NULL && (pd->l_flags & LX_PROC_STRICT_MODE) != 0) {
		/*
		 * If this process was run with strict mode enabled
		 * (via LX_STRICT in the environment), we mark this
		 * LWP as having triggered an unsupported behaviour.
		 * This flag will be checked at an appropriate point
		 * by lx_check_strict_failure().
		 */
		lx_lwp_data_t *lwpd = ttolxlwp(curthread);

		lwpd->br_strict_failure = B_TRUE;
	}
}

void
lx_check_strict_failure(lx_lwp_data_t *lwpd)
{
	proc_t *p;

	if (!lwpd->br_strict_failure) {
		return;
	}

	lwpd->br_strict_failure = B_FALSE;

	/*
	 * If this process is operating in strict mode (via LX_STRICT in
	 * the environment), and has triggered a call to
	 * lx_unsupported(), we drop SIGSYS on it as we return.
	 */
	p = curproc;
	mutex_enter(&p->p_lock);
	sigtoproc(p, curthread, SIGSYS);
	mutex_exit(&p->p_lock);
}

void
lx_trace_sysenter(int syscall_num, uintptr_t *args)
{
	if (lx_systrace_enabled) {
		VERIFY(lx_systrace_entry_ptr != NULL);

		(*lx_systrace_entry_ptr)(syscall_num, args[0], args[1],
		    args[2], args[3], args[4], args[5]);
	}
}

void
lx_trace_sysreturn(int syscall_num, long ret)
{
	if (lx_systrace_enabled) {
		VERIFY(lx_systrace_return_ptr != NULL);

		(*lx_systrace_return_ptr)(syscall_num, ret, ret, 0, 0, 0, 0);
	}
}

/*
 * Get the addresses of the user-space system call handler and attach it to
 * the proc structure. Returning 0 indicates success; the value returned
 * by the system call is the value stored in rval. Returning a non-zero
 * value indicates a failure; the value returned is used to set errno, -1
 * is returned from the syscall and the contents of rval are ignored. To
 * set errno and have the syscall return a value other than -1 we can
 * manually set errno and rval and return 0.
 */
int
lx_brandsys(int cmd, int64_t *rval, uintptr_t arg1, uintptr_t arg2,
    uintptr_t arg3, uintptr_t arg4, uintptr_t arg5)
{
	kthread_t *t = curthread;
	klwp_t *lwp = ttolwp(t);
	proc_t *p = ttoproc(t);
	lx_proc_data_t *pd;
	struct termios *termios;
	uint_t termios_len;
	int error;
	int code;
	int sig;
	lx_brand_registration_t reg;
	lx_lwp_data_t *lwpd = lwptolxlwp(lwp);

	/*
	 * There is one operation that is suppored for non-branded
	 * process.  B_EXEC_BRAND.  This is the equilivant of an
	 * exec call, but the new process that is created will be
	 * a branded process.
	 */
	if (cmd == B_EXEC_BRAND) {
		VERIFY(p->p_zone != NULL);
		VERIFY(p->p_zone->zone_brand == &lx_brand);
		return (exec_common(
		    (char *)arg1, (const char **)arg2, (const char **)arg3,
		    EBA_BRAND));
	}

	/* For all other operations this must be a branded process. */
	if (p->p_brand == NULL)
		return (ENOSYS);

	VERIFY(p->p_brand == &lx_brand);
	VERIFY(p->p_brand_data != NULL);

	switch (cmd) {
	case B_REGISTER:
		if (lwpd->br_stack_mode != LX_STACK_MODE_PREINIT) {
			lx_print("stack mode was not PREINIT during "
			    "REGISTER\n");
			return (EINVAL);
		}

		if (p->p_model == DATAMODEL_NATIVE) {
			if (copyin((void *)arg1, &reg, sizeof (reg)) != 0) {
				lx_print("Failed to copyin brand registration "
				    "at 0x%p\n", (void *)arg1);
				return (EFAULT);
			}
		}
#ifdef _LP64
		else {
			/* 32-bit userland on 64-bit kernel */
			lx_brand_registration32_t reg32;

			if (copyin((void *)arg1, &reg32, sizeof (reg32)) != 0) {
				lx_print("Failed to copyin brand registration "
				    "at 0x%p\n", (void *)arg1);
				return (EFAULT);
			}

			reg.lxbr_version = (uint_t)reg32.lxbr_version;
			reg.lxbr_handler =
			    (void *)(uintptr_t)reg32.lxbr_handler;
			reg.lxbr_flags = reg32.lxbr_flags;
		}
#endif

		if (reg.lxbr_version != LX_VERSION_1) {
			lx_print("Invalid brand library version (%u)\n",
			    reg.lxbr_version);
			return (EINVAL);
		}

		if ((reg.lxbr_flags & ~LX_PROC_ALL) != 0) {
			lx_print("Invalid brand flags (%u)\n",
			    reg.lxbr_flags);
			return (EINVAL);
		}

		lx_print("Assigning brand 0x%p and handler 0x%p to proc 0x%p\n",
		    (void *)&lx_brand, (void *)reg.lxbr_handler, (void *)p);
		pd = p->p_brand_data;
		pd->l_handler = (uintptr_t)reg.lxbr_handler;
		pd->l_flags = reg.lxbr_flags & LX_PROC_ALL;

		return (0);

	case B_TTYMODES:
		/* This is necessary for emulating TCGETS ioctls. */
		if (ddi_prop_lookup_byte_array(DDI_DEV_T_ANY, ddi_root_node(),
		    DDI_PROP_NOTPROM, "ttymodes", (uchar_t **)&termios,
		    &termios_len) != DDI_SUCCESS)
			return (EIO);

		ASSERT(termios_len == sizeof (*termios));

		if (copyout(&termios, (void *)arg1, sizeof (termios)) != 0) {
			ddi_prop_free(termios);
			return (EFAULT);
		}

		ddi_prop_free(termios);
		return (0);

	case B_ELFDATA: {
		mutex_enter(&p->p_lock);
		pd = curproc->p_brand_data;
		if (get_udatamodel() == DATAMODEL_NATIVE) {
			lx_elf_data_t led;

			bcopy(&pd->l_elf_data, &led, sizeof (led));
			mutex_exit(&p->p_lock);

			if (copyout(&led, (void *)arg1,
			    sizeof (lx_elf_data_t)) != 0) {
				return (EFAULT);
			}
		}
#if defined(_LP64)
		else {
			/* 32-bit userland on 64-bit kernel */
			lx_elf_data32_t led32;

			led32.ed_phdr = (int)pd->l_elf_data.ed_phdr;
			led32.ed_phent = (int)pd->l_elf_data.ed_phent;
			led32.ed_phnum = (int)pd->l_elf_data.ed_phnum;
			led32.ed_entry = (int)pd->l_elf_data.ed_entry;
			led32.ed_base = (int)pd->l_elf_data.ed_base;
			led32.ed_ldentry = (int)pd->l_elf_data.ed_ldentry;
			led32.ed_vdso = 0;
			mutex_exit(&p->p_lock);

			if (copyout(&led32, (void *)arg1,
			    sizeof (led32)) != 0) {
				return (EFAULT);
			}
		}
#endif
		return (0);
	}

	case B_EXEC_NATIVE:
		return (exec_common((char *)arg1, (const char **)arg2,
		    (const char **)arg3, EBA_NATIVE));

	/*
	 * The B_TRUSS_POINT subcommand is used so that we can make a no-op
	 * syscall for debugging purposes (dtracing) from within the user-level
	 * emulation.
	 */
	case B_TRUSS_POINT:
		return (0);

	case B_LPID_TO_SPAIR: {
		/*
		 * Given a Linux pid as arg1, return the Solaris pid in arg2 and
		 * the Solaris LWP in arg3.  We also translate pid 1 (which is
		 * hardcoded in many applications) to the zone's init process.
		 */
		pid_t s_pid;
		id_t s_tid;

		if ((pid_t)arg1 == 1) {
			s_pid = p->p_zone->zone_proc_initpid;
			/* handle the dead/missing init(1M) case */
			if (s_pid == -1)
				s_pid = 1;
			s_tid = 1;
		} else if (lx_lpid_to_spair((pid_t)arg1, &s_pid, &s_tid) < 0) {
			return (ESRCH);
		}

		if (copyout(&s_pid, (void *)arg2, sizeof (s_pid)) != 0 ||
		    copyout(&s_tid, (void *)arg3, sizeof (s_tid)) != 0) {
			return (EFAULT);
		}

		return (0);
	}

	case B_SIGEV_THREAD_ID: {
		/*
		 * Emulate Linux's timer_create(2) SIGEV_THREAD_ID
		 * notification method. This mechanism is only meant
		 * for userland threading libraries such as glibc and
		 * is documented as such. Therefore, assume this is
		 * only ever invoked for the purpose of alerting a
		 * Linux threading library. Assume that the tid is a
		 * member of the caller's process and the signal
		 * number is valid. See lx_sigev_thread_id() for the
		 * userland side of this emulation.
		 *
		 * The return code from this function is not checked
		 * by the caller since it executes in an asynchronous
		 * context and there is nothing much to be done. If
		 * this function does fail then it will manifest as
		 * Linux threads waiting for a signal they will never
		 * receive.
		 *
		 * arg1 -- Linux tid
		 * arg2 -- Linux signal number
		 * arg3 -- sigval pointer
		 */

		int native_sig = lx_ltos_signo((int)arg2, 0);
		pid_t native_pid;
		int native_tid;
		sigqueue_t *sqp;

		if (native_sig == 0)
			return (EINVAL);

		lx_lpid_to_spair((pid_t)arg1, &native_pid, &native_tid);
		sqp = kmem_zalloc(sizeof (sigqueue_t), KM_SLEEP);
		mutex_enter(&curproc->p_lock);

		if ((t = idtot(curproc, native_tid)) == NULL) {
			mutex_exit(&curproc->p_lock);
			kmem_free(sqp, sizeof (sigqueue_t));
			return (ESRCH);
		}

		sqp->sq_info.si_signo = native_sig;
		sqp->sq_info.si_code = SI_TIMER;
		sqp->sq_info.si_pid = curproc->p_pid;
		sqp->sq_info.si_zoneid = getzoneid();
		sqp->sq_info.si_uid = crgetruid(CRED());
		sqp->sq_info.si_value.sival_ptr = (void *)arg3;
		sigaddqa(curproc, t, sqp);

		mutex_exit(&curproc->p_lock);

		return (0);
	}

	case B_SET_AFFINITY_MASK:
	case B_GET_AFFINITY_MASK:
		/*
		 * Retrieve or store the CPU affinity mask for the
		 * requested linux pid.
		 *
		 * arg1 is a linux PID (0 means curthread).
		 * arg2 is the size of the given mask.
		 * arg3 is the address of the affinity mask.
		 */
		return (lx_sched_affinity(cmd, arg1, arg2, arg3, rval));

	case B_PTRACE_STOP_FOR_OPT:
		return (lx_ptrace_stop_for_option((int)arg1, arg2 == 0 ?
		    B_FALSE : B_TRUE, (ulong_t)arg3, arg4));

	case B_PTRACE_CLONE_BEGIN:
		return (lx_ptrace_set_clone_inherit((int)arg1, arg2 == 0 ?
		    B_FALSE : B_TRUE));

	case B_HELPER_WAITID: {
		idtype_t idtype = (idtype_t)arg1;
		id_t id = (id_t)arg2;
		siginfo_t *infop = (siginfo_t *)arg3;
		int options = (int)arg4;

		lwpd = ttolxlwp(curthread);

		/*
		 * Our brand-specific waitid helper only understands a subset of
		 * the possible idtypes.  Ensure we keep to that subset here:
		 */
		if (idtype != P_ALL && idtype != P_PID && idtype != P_PGID) {
			return (EINVAL);
		}

		/*
		 * Enable the return of emulated ptrace(2) stop conditions
		 * through lx_waitid_helper, and stash the Linux-specific
		 * extra waitid() flags.
		 */
		lwpd->br_waitid_emulate = B_TRUE;
		lwpd->br_waitid_flags = (int)arg5;

#if defined(_SYSCALL32_IMPL)
		if (get_udatamodel() != DATAMODEL_NATIVE) {
			return (waitsys32(idtype, id, infop, options));
		} else
#endif
		{
			return (waitsys(idtype, id, infop, options));
		}

		lwpd->br_waitid_emulate = B_FALSE;
		lwpd->br_waitid_flags = 0;

		return (0);
	}

	case B_UNSUPPORTED: {
		char dmsg[256];

		if (copyin((void *)arg1, &dmsg, sizeof (dmsg)) != 0) {
			lx_print("Failed to copyin unsupported msg "
			    "at 0x%p\n", (void *)arg1);
			return (EFAULT);
		}
		dmsg[255] = '\0';
		lx_unsupported(dmsg);

		lx_check_strict_failure(lwpd);

		return (0);
	}

	case B_STORE_ARGS: {
		/*
		 * B_STORE_ARGS subcommand
		 * arg1 = address of struct to be copied in
		 * arg2 = size of the struct being copied in
		 * arg3-arg6 ignored
		 * rval = the amount of data copied.
		 */
		void *buf;

		/* only have upper limit because arg2 is unsigned */
		if (arg2 > LX_BR_ARGS_SIZE_MAX) {
			return (EINVAL);
		}

		buf = kmem_alloc(arg2, KM_SLEEP);
		if (copyin((void *)arg1, buf, arg2) != 0) {
			lx_print("Failed to copyin scall arg at 0x%p\n",
			    (void *) arg1);
			kmem_free(buf, arg2);
			/*
			 * Purposely not setting br_scall_args to NULL
			 * to preserve data for debugging.
			 */
			return (EFAULT);
		}

		if (lwpd->br_scall_args != NULL) {
			ASSERT(lwpd->br_args_size > 0);
			kmem_free(lwpd->br_scall_args,
			    lwpd->br_args_size);
		}

		lwpd->br_scall_args = buf;
		lwpd->br_args_size = arg2;
		*rval = arg2;
		return (0);
	}

	case B_HELPER_CLONE:
		return (lx_helper_clone(rval, arg1, (void *)arg2, (void *)arg3,
		    (void *)arg4));

	case B_HELPER_SETGROUPS:
		return (lx_helper_setgroups(arg1, (gid_t *)arg2));

	case B_HELPER_SIGQUEUE:
		return (lx_helper_rt_sigqueueinfo(arg1, arg2,
		    (siginfo_t *)arg3));

	case B_HELPER_TGSIGQUEUE:
		return (lx_helper_rt_tgsigqueueinfo(arg1, arg2, arg3,
		    (siginfo_t *)arg4));

	case B_SET_THUNK_PID:
		lwpd->br_lx_thunk_pid = arg1;
		return (0);

	case B_GETPID:
		/*
		 * The usermode clone(2) code needs to be able to call
		 * lx_getpid() from native code:
		 */
		*rval = lx_getpid();
		return (0);

	case B_SET_NATIVE_STACK:
		/*
		 * B_SET_NATIVE_STACK subcommand
		 * arg1 = the base of the stack to use for emulation
		 */
		if (lwpd->br_stack_mode != LX_STACK_MODE_PREINIT) {
			lx_print("B_SET_NATIVE_STACK when stack was already "
			    "set to %p\n", (void *)arg1);
			return (EEXIST);
		}

		/*
		 * We move from the PREINIT state, where we have no brand
		 * emulation stack, to the INIT state.  Here, we are still
		 * running on what will become the BRAND stack, but are running
		 * emulation (i.e. native) code.  Once the initialisation
		 * process for this thread has finished, we will jump to
		 * brand-specific code, while moving to the BRAND mode.
		 *
		 * When a new LWP is created, lx_initlwp() will clear the
		 * stack data.  If that LWP is actually being duplicated
		 * into a child process by fork(2), lx_forklwp() will copy
		 * it so that the cloned thread will keep using the same
		 * alternate stack.
		 */
		lwpd->br_ntv_stack = arg1;
		lwpd->br_stack_mode = LX_STACK_MODE_INIT;
		lx_lwp_set_native_stack_current(lwpd, arg1);

		return (0);

	case B_GET_CURRENT_CONTEXT:
		/*
		 * B_GET_CURRENT_CONTEXT subcommand:
		 * arg1 = address for pointer to current ucontext_t
		 */

#if defined(_SYSCALL32_IMPL)
		if (get_udatamodel() != DATAMODEL_NATIVE) {
			caddr32_t addr = (caddr32_t)lwp->lwp_oldcontext;

			error = copyout(&addr, (void *)arg1, sizeof (addr));
		} else
#endif
		{
			error = copyout(&lwp->lwp_oldcontext, (void *)arg1,
			    sizeof (lwp->lwp_oldcontext));
		}

		return (error != 0 ? EFAULT : 0);

	case B_JUMP_TO_LINUX:
		/*
		 * B_JUMP_TO_LINUX subcommand:
		 * arg1 = ucontext_t pointer for jump state
		 */

		if (arg1 == NULL)
			return (EINVAL);

		switch (lwpd->br_stack_mode) {
		case LX_STACK_MODE_NATIVE: {
			struct regs *rp = lwptoregs(lwp);

			/*
			 * We are on the NATIVE stack, so we must preserve
			 * the extent of that stack.  The pointer will be
			 * reset by a future setcontext().
			 */
			lx_lwp_set_native_stack_current(lwpd,
			    (uintptr_t)rp->r_sp);
			break;
		}

		case LX_STACK_MODE_INIT:
			/*
			 * The LWP is transitioning to Linux code for the first
			 * time.
			 */
			break;

		case LX_STACK_MODE_PREINIT:
			/*
			 * This LWP has not installed an alternate stack for
			 * usermode emulation handling.
			 */
			return (ENOENT);

		case LX_STACK_MODE_BRAND:
			/*
			 * The LWP should not be on the BRAND stack.
			 */
			exit(CLD_KILLED, SIGSYS);
			return (0);
		}

		/*
		 * Transfer control to Linux:
		 */
		return (lx_runexe(lwp, (void *)arg1));

	case B_EMULATION_DONE:
		/*
		 * B_EMULATION_DONE subcommand:
		 * arg1 = ucontext_t * to restore
		 * arg2 = system call number
		 * arg3 = return code
		 * arg4 = if operation failed, the errno value
		 */

		/*
		 * The first part of this operation is a setcontext() to
		 * restore the register state to the copy we preserved
		 * before vectoring to the usermode emulation routine.
		 * If that fails, we return (hopefully) to the emulation
		 * routine and it will handle the error.
		 */
#if (_SYSCALL32_IMPL)
		if (get_udatamodel() != DATAMODEL_NATIVE) {
			error = getsetcontext32(SETCONTEXT, (void *)arg1);
		} else
#endif
		{
			error = getsetcontext(SETCONTEXT, (void *)arg1);
		}

		if (error != 0) {
			return (error);
		}

		/*
		 * The saved Linux context has been restored.  We handle the
		 * return value or errno with code common to the in-kernel
		 * system call emulation.
		 */
		if ((error = (int)arg4) != 0) {
			/*
			 * lx_syscall_return() looks at the errno in the LWP,
			 * so set it here:
			 */
			set_errno(error);
		}
		lx_syscall_return(ttolwp(curthread), (int)arg2, (long)arg3);

		return (0);

	case B_EXIT_AS_SIG:
		code = CLD_KILLED;
		sig = (int)arg1;
		proc_is_exiting(p);
		if (exitlwps(1) != 0) {
			mutex_enter(&p->p_lock);
			lwp_exit();
		}
		ttolwp(curthread)->lwp_cursig = sig;
		if (sig == SIGSEGV) {
			if (core(sig, 0) == 0)
				code = CLD_DUMPED;
		}
		exit(code, sig);
		/* NOTREACHED */
		break;

	case B_OVERRIDE_KERN_VER: {
		void *urel = (void *)arg1;
		void *uver = (void *)arg2;
		size_t len;

		pd = ptolxproc(p);
		if (urel != NULL) {
			if (copyinstr(urel, pd->l_uname_release,
			    LX_KERN_RELEASE_MAX, &len) != 0) {
				return (EFAULT);
			}
			pd->l_uname_release[LX_KERN_RELEASE_MAX - 1] = '\0';
		}
		if (uver != NULL) {
			if (copyinstr(uver, pd->l_uname_version,
			    LX_KERN_VERSION_MAX, &len) != 0) {
				return (EFAULT);
			}
			pd->l_uname_version[LX_KERN_VERSION_MAX - 1] = '\0';
		}

		return (0);
	}

	case B_NOTIFY_VDSO_LOC: {
#if defined(_LP64)
		if (get_udatamodel() == DATAMODEL_NATIVE) {
			int i;

			mutex_enter(&p->p_lock);
			pd = ptolxproc(p);
			pd->l_elf_data.ed_vdso = arg1;
			/* overwrite the auxv data too */
			for (i = 0; i < __KERN_NAUXV_IMPL; i++) {
				if (p->p_user.u_auxv[i].a_type ==
				    AT_SUN_BRAND_LX_SYSINFO_EHDR) {
					p->p_user.u_auxv[i].a_un.a_val = arg1;
					break;
				}
			}
			mutex_exit(&p->p_lock);
			return (0);
		}
#endif /* defined(_LP64) */
		/* This is not valid for 32bit processes */
		return (EINVAL);
	}

	}

	return (EINVAL);
}

/*
 * Compare linux kernel version to the one set for the zone.
 * Returns greater than 0 if zone version is higher, less than 0 if the zone
 * version is lower, and 0 if the versions are equal.
 */
int
lx_kern_release_cmp(zone_t *zone, const char *vers)
{
	int zvers[3] = {0, 0, 0};
	int cvers[3] = {0, 0, 0};
	int i;

	VERIFY(zone->zone_brand == &lx_brand);

	(void) sscanf(ztolxzd(zone)->lxzd_kernel_release, "%d.%d.%d", &zvers[0],
	    &zvers[1], &zvers[2]);
	(void) sscanf(vers, "%d.%d.%d", &cvers[0], &cvers[1], &cvers[2]);

	for (i = 0; i < 3; i++) {
		if (zvers[i] > cvers[i]) {
			return (1);
		} else if (zvers[i] < cvers[i]) {
			return (-1);
		}
	}
	return (0);
}

/*
 * Linux unconditionally removes the setuid and setgid bits when changing
 * file ownership.  This brand hook overrides the illumos native behaviour,
 * which is based on the PRIV_FILE_SETID privilege.
 */
static int
lx_setid_clear(vattr_t *vap, cred_t *cr)
{
	if (S_ISDIR(vap->va_mode)) {
		return (0);
	}

	if (vap->va_mode & S_ISUID) {
		vap->va_mask |= AT_MODE;
		vap->va_mode &= ~S_ISUID;
	}
	if ((vap->va_mode & (S_ISGID | S_IXGRP)) == (S_ISGID | S_IXGRP)) {
		vap->va_mask |= AT_MODE;
		vap->va_mode &= ~S_ISGID;
	}

	return (0);
}

/*
 * Copy the per-process brand data from a parent proc to a child.
 */
void
lx_copy_procdata(proc_t *cp, proc_t *pp)
{
	lx_proc_data_t *cpd, *ppd;

	/*
	 * Since b_copy_procdata is called during getproc(), while the child
	 * process is still being initialized, acquiring cp->p_lock should not
	 * be required.
	 */
	VERIFY(cp->p_brand == &lx_brand);
	VERIFY(cpd = cp->p_brand_data);

	mutex_enter(&pp->p_lock);
	VERIFY(pp->p_brand == &lx_brand);
	VERIFY(ppd = pp->p_brand_data);

	bcopy(ppd, cpd, sizeof (lx_proc_data_t));
	mutex_exit(&pp->p_lock);

	/*
	 * The l_ptrace count is normally manipulated only while under holding
	 * p_lock.  Since this is a freshly created process, it's safe to zero
	 * out.  If it is to be inherited, the attach will occur later.
	 */
	cpd->l_ptrace = 0;

	cpd->l_fake_limits[LX_RLFAKE_LOCKS].rlim_cur = LX_RLIM64_INFINITY;
	cpd->l_fake_limits[LX_RLFAKE_LOCKS].rlim_max = LX_RLIM64_INFINITY;

	cpd->l_fake_limits[LX_RLFAKE_NICE].rlim_cur = 20;
	cpd->l_fake_limits[LX_RLFAKE_NICE].rlim_max = 20;

	cpd->l_fake_limits[LX_RLFAKE_RTPRIO].rlim_cur = LX_RLIM64_INFINITY;
	cpd->l_fake_limits[LX_RLFAKE_RTPRIO].rlim_max = LX_RLIM64_INFINITY;

	cpd->l_fake_limits[LX_RLFAKE_RTTIME].rlim_cur = LX_RLIM64_INFINITY;
	cpd->l_fake_limits[LX_RLFAKE_RTTIME].rlim_max = LX_RLIM64_INFINITY;
}

#if defined(_LP64)
static void
Ehdr32to64(Elf32_Ehdr *src, Ehdr *dst)
{
	bcopy(src->e_ident, dst->e_ident, sizeof (src->e_ident));
	dst->e_type =		src->e_type;
	dst->e_machine =	src->e_machine;
	dst->e_version =	src->e_version;
	dst->e_entry =		src->e_entry;
	dst->e_phoff =		src->e_phoff;
	dst->e_shoff =		src->e_shoff;
	dst->e_flags =		src->e_flags;
	dst->e_ehsize =		src->e_ehsize;
	dst->e_phentsize =	src->e_phentsize;
	dst->e_phnum =		src->e_phnum;
	dst->e_shentsize =	src->e_shentsize;
	dst->e_shnum =		src->e_shnum;
	dst->e_shstrndx =	src->e_shstrndx;
}
#endif /* _LP64 */

static void
restoreexecenv(struct execenv *ep, stack_t *sp)
{
	klwp_t *lwp = ttolwp(curthread);

	setexecenv(ep);
	lwp->lwp_sigaltstack.ss_sp = sp->ss_sp;
	lwp->lwp_sigaltstack.ss_size = sp->ss_size;
	lwp->lwp_sigaltstack.ss_flags = sp->ss_flags;
}

extern int elfexec(vnode_t *, execa_t *, uarg_t *, intpdata_t *, int,
    long *, int, caddr_t, cred_t *, int *);

extern int elf32exec(struct vnode *, execa_t *, uarg_t *, intpdata_t *, int,
    long *, int, caddr_t, cred_t *, int *);

/*
 * Exec routine called by elfexec() to load either 32-bit or 64-bit Linux
 * binaries.
 */
static int
lx_elfexec(struct vnode *vp, struct execa *uap, struct uarg *args,
    struct intpdata *idata, int level, long *execsz, int setid,
    caddr_t exec_file, struct cred *cred, int *brand_action)
{
	int		error;
	vnode_t		*nvp;
	Ehdr		ehdr;
	Addr		uphdr_vaddr;
	intptr_t	voffset;
	char		*interp = NULL;
	uintptr_t	ldaddr = NULL;
	int		i;
	proc_t		*p = ttoproc(curthread);
	klwp_t		*lwp = ttolwp(curthread);
	struct execenv	env;
	struct execenv	origenv;
	stack_t		orig_sigaltstack;
	struct user	*up = PTOU(ttoproc(curthread));
	lx_elf_data_t	*edp;
	char		*lib_path = NULL;

	ASSERT(ttoproc(curthread)->p_brand == &lx_brand);
	ASSERT(ttoproc(curthread)->p_brand_data != NULL);

	edp = &ttolxproc(curthread)->l_elf_data;

	if (args->to_model == DATAMODEL_NATIVE) {
		lib_path = LX_LIB_PATH;
	}
#if defined(_LP64)
	else {
		lib_path = LX_LIB_PATH32;
	}
#endif

	/*
	 * Set the brandname and library name for the new process so that
	 * elfexec() puts them onto the stack.
	 */
	args->brandname = LX_BRANDNAME;
	args->emulator = lib_path;

#if defined(_LP64)
	/*
	 * To conform with the way Linux lays out the address space, we clamp
	 * the stack to be the top of the lower region of the x86-64 canonical
	 * form address space -- which has the side-effect of laying out the
	 * entire address space in that lower region.  Note that this only
	 * matters on 64-bit processes (this value will always be greater than
	 * the size of a 32-bit address space) and doesn't actually affect
	 * USERLIMIT:  if a Linux-branded processes wishes to map something
	 * into the top half of the address space, it can do so -- but with
	 * the user stack starting at the top of the bottom region, those high
	 * virtual addresses won't be used unless explicitly directed.
	 */
	args->maxstack = lx_maxstack64;
#endif

	/*
	 * We will first exec the brand library, then map in the linux
	 * executable and the linux linker.
	 */
	if ((error = lookupname(lib_path, UIO_SYSSPACE, FOLLOW, NULLVPP,
	    &nvp))) {
		uprintf("%s: not found.", lib_path);
		return (error);
	}

	/*
	 * We will eventually set the p_exec member to be the vnode for the new
	 * executable when we call setexecenv(). However, if we get an error
	 * before that call we need to restore the execenv to its original
	 * values so that when we return to the caller fop_close() works
	 * properly while cleaning up from the failed exec().  Restoring the
	 * original value will also properly decrement the 2nd VN_RELE that we
	 * took on the brand library.
	 */
	origenv.ex_bssbase = p->p_bssbase;
	origenv.ex_brkbase = p->p_brkbase;
	origenv.ex_brksize = p->p_brksize;
	origenv.ex_vp = p->p_exec;
	orig_sigaltstack.ss_sp = lwp->lwp_sigaltstack.ss_sp;
	orig_sigaltstack.ss_size = lwp->lwp_sigaltstack.ss_size;
	orig_sigaltstack.ss_flags = lwp->lwp_sigaltstack.ss_flags;

	if (args->to_model == DATAMODEL_NATIVE) {
		error = elfexec(nvp, uap, args, idata, INTP_MAXDEPTH + 1,
		    execsz, setid, exec_file, cred, brand_action);
	}
#if defined(_LP64)
	else {
		error = elf32exec(nvp, uap, args, idata, INTP_MAXDEPTH + 1,
		    execsz, setid, exec_file, cred, brand_action);
	}
#endif
	VN_RELE(nvp);
	if (error != 0) {
		restoreexecenv(&origenv, &orig_sigaltstack);
		return (error);
	}

	/*
	 * exec-ed in the brand library above.
	 * The u_auxv vectors are now setup by elfexec to point to the
	 * brand emulation library and its linker.
	 */

	bzero(&env, sizeof (env));

	/*
	 * map in the the Linux executable
	 */
	if (args->to_model == DATAMODEL_NATIVE) {
		error = mapexec_brand(vp, args, &ehdr, &uphdr_vaddr,
		    &voffset, exec_file, &interp, &env.ex_bssbase,
		    &env.ex_brkbase, &env.ex_brksize, NULL, NULL);
	}
#if defined(_LP64)
	else {
		Elf32_Ehdr	ehdr32;
		Elf32_Addr	uphdr_vaddr32;

		error = mapexec32_brand(vp, args, &ehdr32, &uphdr_vaddr32,
		    &voffset, exec_file, &interp, &env.ex_bssbase,
		    &env.ex_brkbase, &env.ex_brksize, NULL, NULL);

		Ehdr32to64(&ehdr32, &ehdr);

		if (uphdr_vaddr32 == (Elf32_Addr)-1)
			uphdr_vaddr = (Addr)-1;
		else
			uphdr_vaddr = uphdr_vaddr32;
	}
#endif
	if (error != 0) {
		restoreexecenv(&origenv, &orig_sigaltstack);

		if (interp != NULL)
			kmem_free(interp, MAXPATHLEN);

		return (error);
	}

	/*
	 * Save off the important properties of the lx executable. The brand
	 * library will ask us for this data later, when it is ready to set
	 * things up for the lx executable.
	 */
	edp->ed_phdr = (uphdr_vaddr == -1) ? voffset + ehdr.e_phoff :
	    voffset + uphdr_vaddr;
	edp->ed_entry = voffset + ehdr.e_entry;
	edp->ed_phent = ehdr.e_phentsize;
	edp->ed_phnum = ehdr.e_phnum;

	if (interp != NULL) {
		if (ehdr.e_type == ET_DYN) {
			/*
			 * This is a shared object executable, so we need to
			 * pick a reasonable place to put the heap. Just don't
			 * use the first page.
			 */
			env.ex_brkbase = (caddr_t)PAGESIZE;
			env.ex_bssbase = (caddr_t)PAGESIZE;
		}

		/*
		 * If the program needs an interpreter (most do), map it in and
		 * store relevant information about it in the aux vector, where
		 * the brand library can find it.
		 */
		if ((error = lookupname(interp, UIO_SYSSPACE, FOLLOW,
		    NULLVPP, &nvp))) {
			uprintf("%s: not found.", interp);
			restoreexecenv(&origenv, &orig_sigaltstack);
			kmem_free(interp, MAXPATHLEN);
			return (error);
		}

		kmem_free(interp, MAXPATHLEN);
		interp = NULL;

		/*
		 * map in the Linux linker
		 */
		if (args->to_model == DATAMODEL_NATIVE) {
			error = mapexec_brand(nvp, args, &ehdr,
			    &uphdr_vaddr, &voffset, exec_file, NULL, NULL,
			    NULL, NULL, NULL, &ldaddr);
		}
#if defined(_LP64)
		else {
			Elf32_Ehdr	ehdr32;
			Elf32_Addr	uphdr_vaddr32;

			error = mapexec32_brand(nvp, args, &ehdr32,
			    &uphdr_vaddr32, &voffset, exec_file, NULL, NULL,
			    NULL, NULL, NULL, &ldaddr);

			Ehdr32to64(&ehdr32, &ehdr);

			if (uphdr_vaddr32 == (Elf32_Addr)-1)
				uphdr_vaddr = (Addr)-1;
			else
				uphdr_vaddr = uphdr_vaddr32;
		}
#endif

		VN_RELE(nvp);
		if (error != 0) {
			restoreexecenv(&origenv, &orig_sigaltstack);
			return (error);
		}

		/*
		 * Now that we know the base address of the brand's linker,
		 * we also save this for later use by the brand library.
		 */
		edp->ed_base = voffset;
		edp->ed_ldentry = voffset + ehdr.e_entry;
	} else {
		/*
		 * This program has no interpreter. The lx brand library will
		 * jump to the address in the AT_SUN_BRAND_LDENTRY aux vector,
		 * so in this case, put the entry point of the main executable
		 * there.
		 */
		if (ehdr.e_type == ET_EXEC) {
			/*
			 * An executable with no interpreter, this must be a
			 * statically linked executable, which means we loaded
			 * it at the address specified in the elf header, in
			 * which case the e_entry field of the elf header is an
			 * absolute address.
			 */
			edp->ed_ldentry = ehdr.e_entry;
			edp->ed_entry = ehdr.e_entry;
		} else {
			/*
			 * A shared object with no interpreter, we use the
			 * calculated address from above.
			 */
			edp->ed_ldentry = edp->ed_entry;

			/*
			 * In all situations except an ET_DYN elf object with no
			 * interpreter, we want to leave the brk and base
			 * values set by mapexec_brand alone. Normally when
			 * running ET_DYN objects on Solaris (most likely
			 * /lib/ld.so.1) the kernel sets brk and base to 0 since
			 * it doesn't know where to put the heap, and later the
			 * linker will call brk() to initialize the heap in:
			 *	usr/src/cmd/sgs/rtld/common/setup.c:setup()
			 * after it has determined where to put it.  (This
			 * decision is made after the linker loads and inspects
			 * elf properties of the target executable being run.)
			 *
			 * So for ET_DYN Linux executables, we also don't know
			 * where the heap should go, so we'll set the brk and
			 * base to 0.  But in this case the Solaris linker will
			 * not initialize the heap, so when the Linux linker
			 * starts running there is no heap allocated.  This
			 * seems to be ok on Linux 2.4 based systems because the
			 * Linux linker/libc fall back to using mmap() to
			 * allocate memory. But on 2.6 systems, running
			 * applications by specifying them as command line
			 * arguments to the linker results in segfaults for an
			 * as yet undetermined reason (which seems to indicatej
			 * that a more permanent fix for heap initalization in
			 * these cases may be necessary).
			 */
			if (ehdr.e_type == ET_DYN) {
				env.ex_bssbase = (caddr_t)0;
				env.ex_brkbase = (caddr_t)0;
				env.ex_brksize = 0;
			}
		}

	}

	env.ex_vp = vp;
	setexecenv(&env);

	/*
	 * We try to keep /proc's view of the aux vector consistent with
	 * what's on the process stack.
	 */
	if (args->to_model == DATAMODEL_NATIVE) {
		auxv_t phdr_auxv[4] = {
		    { AT_SUN_BRAND_LX_PHDR, 0 },
		    { AT_SUN_BRAND_LX_INTERP, 0 },
		    { AT_SUN_BRAND_LX_CLKTCK, 0 },
		    { AT_SUN_BRAND_LX_SYSINFO_EHDR, 0 }
		};
		phdr_auxv[0].a_un.a_val = edp->ed_phdr;
		phdr_auxv[1].a_un.a_val = ldaddr;
		phdr_auxv[2].a_un.a_val = hz;
		/*
		 * The userspace brand library will map in the vDSO and notify
		 * the kernel of its location during lx_init.
		 */
		phdr_auxv[3].a_un.a_val = 1;

		if (copyout(&phdr_auxv, args->auxp_brand,
		    sizeof (phdr_auxv)) == -1)
			return (EFAULT);
	}
#if defined(_LP64)
	else {
		auxv32_t phdr_auxv32[4] = {
		    { AT_SUN_BRAND_LX_PHDR, 0 },
		    { AT_SUN_BRAND_LX_INTERP, 0 },
		    { AT_SUN_BRAND_LX_CLKTCK, 0 },
		    { AT_SUN_BRAND_LX_SYSINFO_EHDR, 0 }
		};
		phdr_auxv32[0].a_un.a_val = edp->ed_phdr;
		phdr_auxv32[1].a_un.a_val = ldaddr;
		phdr_auxv32[2].a_un.a_val = hz;
		/*
		 * Unused on i386 due to lack of vDSO.
		 * It will be cleaned up during lx_init.
		 */
		phdr_auxv32[3].a_un.a_val = 0;

		if (copyout(&phdr_auxv32, args->auxp_brand,
		    sizeof (phdr_auxv32)) == -1)
			return (EFAULT);
	}
#endif

	/*
	 * /proc uses the AT_ENTRY aux vector entry to deduce
	 * the location of the executable in the address space. The user
	 * structure contains a copy of the aux vector that needs to have those
	 * entries patched with the values of the real lx executable (they
	 * currently contain the values from the lx brand library that was
	 * elfexec'd, above).
	 *
	 * For live processes, AT_BASE is used to locate the linker segment,
	 * which /proc and friends will later use to find Solaris symbols
	 * (such as rtld_db_preinit). However, for core files, /proc uses
	 * AT_ENTRY to find the right segment to label as the executable.
	 * So we set AT_ENTRY to be the entry point of the linux executable,
	 * but leave AT_BASE to be the address of the Solaris linker.
	 */
	for (i = 0; i < __KERN_NAUXV_IMPL; i++) {
		switch (up->u_auxv[i].a_type) {
		case AT_ENTRY:
			up->u_auxv[i].a_un.a_val = edp->ed_entry;
			break;

		case AT_SUN_BRAND_LX_PHDR:
			up->u_auxv[i].a_un.a_val = edp->ed_phdr;
			break;

		case AT_SUN_BRAND_LX_INTERP:
			up->u_auxv[i].a_un.a_val = ldaddr;
			break;

		case AT_SUN_BRAND_LX_CLKTCK:
			up->u_auxv[i].a_un.a_val = hz;
			break;

		default:
			break;
		}
	}

	return (0);
}

boolean_t
lx_native_exec(uint8_t osabi, const char **interp)
{
	if (osabi != ELFOSABI_SOLARIS)
		return (B_FALSE);

	/*
	 * If the process root matches the zone root, prepend /native to the
	 * interpreter path for native executables.  Absolute precision from
	 * VN_CMP is not necessary since any change of process root is likely
	 * to make native binaries inaccessible via /native.
	 *
	 * Processes which chroot directly into /native will be able to
	 * function as expected with no need for the prefix.
	 */
	if (VN_CMP(curproc->p_user.u_rdir, curproc->p_zone->zone_rootvp)) {
		*interp = "/native";
	}

	return (B_TRUE);
}

static void
lx_syscall_init(void)
{
	int i;

	/*
	 * Count up the 32-bit Linux system calls.  Note that lx_sysent32
	 * has (LX_NSYSCALLS + 1) entries.
	 */
	for (i = 0; i <= LX_NSYSCALLS && lx_sysent32[i].sy_name != NULL; i++)
		continue;
	lx_nsysent32 = i;

#if defined(_LP64)
	/*
	 * Count up the 64-bit Linux system calls.  Note that lx_sysent64
	 * has (LX_NSYSCALLS + 1) entries.
	 */
	for (i = 0; i <= LX_NSYSCALLS && lx_sysent64[i].sy_name != NULL; i++)
		continue;
	lx_nsysent64 = i;
#endif
}

int
_init(void)
{
	int err = 0;

	lx_syscall_init();
	lx_pid_init();
	lx_ioctl_init();
	lx_futex_init();
	lx_ptrace_init();
	lx_socket_init();

	err = mod_install(&modlinkage);
	if (err != 0) {
		cmn_err(CE_WARN, "Couldn't install lx brand module");

		/*
		 * This looks drastic, but it should never happen.  These
		 * two data structures should be completely free-able until
		 * they are used by Linux processes.  Since the brand
		 * wasn't loaded there should be no Linux processes, and
		 * thus no way for these data structures to be modified.
		 */
		lx_pid_fini();
		lx_ioctl_fini();
		if (lx_futex_fini())
			panic("lx brand module cannot be loaded or unloaded.");
	}
	return (err);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&modlinkage, modinfop));
}

int
_fini(void)
{
	int err;
	int futex_done = 0;

	/*
	 * If there are any zones using this brand, we can't allow it to be
	 * unloaded.
	 */
	if (brand_zone_count(&lx_brand))
		return (EBUSY);

	lx_ptrace_fini();
	lx_pid_fini();
	lx_ioctl_fini();
	lx_socket_fini();

	if ((err = lx_futex_fini()) != 0) {
		goto done;
	}
	futex_done = 1;

	err = mod_remove(&modlinkage);

done:
	if (err) {
		/*
		 * If we can't unload the module, then we have to get it
		 * back into a sane state.
		 */
		lx_ptrace_init();
		lx_pid_init();
		lx_ioctl_init();
		lx_socket_init();

		if (futex_done) {
			lx_futex_init();
		}
	}

	return (err);
}
