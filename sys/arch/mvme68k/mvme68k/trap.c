/*	$OpenBSD: trap.c,v 1.54 2004/04/18 20:19:52 miod Exp $ */

/*
 * Copyright (c) 1995 Theo de Raadt
 * Copyright (c) 1999 Steve Murphree, Jr. (68060 support)
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Copyright (c) 1988 University of Utah.
 * Copyright (c) 1982, 1986, 1990, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * the Systems Programming Group of the University of Utah Computer
 * Science Department.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * from: Utah $Hdr: trap.c 1.37 92/12/20$
 *
 *	@(#)trap.c	8.5 (Berkeley) 1/4/94
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/acct.h>
#include <sys/kernel.h>
#include <sys/signalvar.h>
#include <sys/resourcevar.h>
#include <sys/syscall.h>
#include <sys/syslog.h>
#include <sys/user.h>
#ifdef KTRACE
#include <sys/ktrace.h>
#endif

#include <machine/db_machdep.h>
#include <machine/cpu.h>
#include <machine/psl.h>
#include <machine/reg.h>
#include <machine/trap.h>

#ifdef COMPAT_SUNOS
#include <compat/sunos/sunos_syscall.h>
extern struct emul emul_sunos;
#endif

#include "systrace.h"
#include <dev/systrace.h>

#include <uvm/uvm_extern.h>
#include <uvm/uvm_pmap.h>

#ifdef COMPAT_HPUX
#include <compat/hpux/hpux.h>
#endif

int	astpending;
int	want_resched;

char  *trap_type[] = {
	"Bus error",
	"Address error",
	"Illegal instruction",
	"Zero divide",
	"CHK instruction",
	"TRAPV instruction",
	"Privilege violation",
	"Trace trap",
	"MMU fault",
	"SSIR trap",
	"Format error",
	"68881 exception",
	"Coprocessor violation",
	"Async system trap"
};
int   trap_types = sizeof trap_type / sizeof trap_type[0];

/*
 * Size of various exception stack frames (minus the standard 8 bytes)
 */
short exframesize[] = {
	FMT0SIZE,	/* type 0 - normal (68020/030/040/060) */
	FMT1SIZE,	/* type 1 - throwaway (68020/030/040) */
	FMT2SIZE,	/* type 2 - normal 6-word (68020/030/040/060) */
	FMT3SIZE,	/* type 3 - FP post-instruction (68040/060) */
	FMT4SIZE,	/* type 4 - access error/fp disabled (68060) */
	-1, -1,		/* type 5-6 - undefined */
	FMT7SIZE,	/* type 7 - access error (68040) */
	58,			/* type 8 - bus fault (68010) */
	FMT9SIZE,	/* type 9 - coprocessor mid-instruction (68020/030) */
	FMTASIZE,	/* type A - short bus fault (68020/030) */
	FMTBSIZE,	/* type B - long bus fault (68020/030) */
	-1, -1, -1, -1	/* type C-F - undefined */
};


#if defined(M68040) || defined(M68060)
#define KDFAULT(c)    (mmutype == MMU_68060 ? ((c) & FSLW_TM_SV) : \
             mmutype == MMU_68040 ? ((c) & SSW4_TMMASK) == SSW4_TMKD : \
			    ((c) & (SSW_DF|FC_SUPERD)) == (SSW_DF|FC_SUPERD))
#define WRFAULT(c)    (mmutype == MMU_68060 ? ((c) & FSLW_RW_W) : \
             mmutype == MMU_68040 ? ((c) & SSW4_RW) == 0 : \
			    ((c) & (SSW_DF|SSW_RW)) == SSW_DF)
#else
#define KDFAULT(c)	(((c) & (SSW_DF|SSW_FCMASK)) == (SSW_DF|FC_SUPERD))
#define WRFAULT(c)	(((c) & (SSW_DF|SSW_RW)) == SSW_DF)
#endif

#ifdef DEBUG
int mmudebug = 0;
int mmupid = -1;
#define MDB_FOLLOW	1
#define MDB_WBFOLLOW	2
#define MDB_WBFAILED	4
#define MDB_ISPID(p)	(p) == mmupid
#endif

#define NSIR	8
void (*sir_routines[NSIR])(void *);
void *sir_args[NSIR];
u_char next_sir;

void trap(int, u_int, u_int, struct frame);
void syscall(register_t, struct frame);
void init_sir(void);
void hardintr(int, int, void *);
int writeback(struct frame *fp, int docachepush);

/*
 * trap and syscall both need the following work done before returning
 * to user mode.
 */
void
userret(p, fp, oticks, faultaddr, fromtrap)
	register struct proc *p;
	register struct frame *fp;
	u_quad_t oticks;
	u_int faultaddr;
	int fromtrap;
{
	int sig;
#if defined(M68040)
	int beenhere = 0;

again:
#endif
	/* take pending signals */
	while ((sig = CURSIG(p)) != 0)
		postsig(sig);
	p->p_priority = p->p_usrpri;
	if (want_resched) {
		/*
		 * We're being preempted.
		 */
		preempt(NULL);
		while ((sig = CURSIG(p)) != 0)
			postsig(sig);
	}

	/*
	 * If profiling, charge system time to the trapped pc.
	 */
	if (p->p_flag & P_PROFIL) {
		extern int psratio;

		addupc_task(p, fp->f_pc, 
			    (int)(p->p_sticks - oticks) * psratio);
	}
#if defined(M68040)
	/*
	 * Deal with user mode writebacks (from trap, or from sigreturn).
	 * If any writeback fails, go back and attempt signal delivery.
	 * unless we have already been here and attempted the writeback
	 * (e.g. bad address with user ignoring SIGSEGV).  In that case
	 * we just return to the user without successfully completing
	 * the writebacks.  Maybe we should just drop the sucker?
	 */
	if (mmutype == MMU_68040 && fp->f_format == FMT7) {
		if (beenhere) {
#ifdef DEBUG
			if (mmudebug & MDB_WBFAILED)
				printf(fromtrap ?
			 "pid %d(%s): writeback aborted, pc=%x, fa=%x\n" :
			 "pid %d(%s): writeback aborted in sigreturn, pc=%x\n",
				     p->p_pid, p->p_comm, fp->f_pc, faultaddr);
#endif
		} else if ((sig = writeback(fp, fromtrap))) {
			register union sigval sv;

			beenhere = 1;
			oticks = p->p_sticks;
			sv.sival_int = faultaddr;
			trapsignal(p, sig, VM_PROT_WRITE, SEGV_MAPERR, sv);
			goto again;
		}
	}
#endif
	curpriority = p->p_priority;
}

/*
 * Trap is called from locore to handle most types of processor traps,
 * including events such as simulated software interrupts/AST's.
 * System calls are broken out for efficiency. T_ADDRERR
 */
/*ARGSUSED*/
void
trap(type, code, v, frame)
	int type;
	u_int code;
	register u_int v;
	struct frame frame;
{
	register struct proc *p;
	register int i;
	u_int ucode;
	u_quad_t sticks;
	int typ = 0, bit;
#ifdef COMPAT_HPUX
	extern struct emul emul_hpux;
#endif
#ifdef COMPAT_SUNOS
	extern struct emul emul_sunos;
#endif
	register union sigval sv;

	uvmexp.traps++;
	p = curproc;
	ucode = 0;
	if (USERMODE(frame.f_sr)) {
		type |= T_USER;
		sticks = p->p_sticks;
		p->p_md.md_regs = frame.f_regs;
	}
	switch (type) {
	default:
dopanic:
		printf("trap type %d, code = %x, v = %x\n", type, code, v);
#ifdef DDB
		if (kdb_trap(type, (db_regs_t *)&frame))
			return;
#endif
		regdump(&(frame.F_t), 128);
		type &= ~T_USER;
		if ((u_int)type < trap_types)
			panic(trap_type[type]);
		panic("trap");

	case T_BUSERR:		/* kernel bus error */
		if (!p || !p->p_addr->u_pcb.pcb_onfault)
			goto dopanic;
copyfault:
		/*
		 * If we have arranged to catch this fault in any of the
		 * copy to/from user space routines, set PC to return to
		 * indicated location and set flag informing buserror code
		 * that it may need to clean up stack frame.
		 */
   		frame.f_stackadj = exframesize[frame.f_format];
   		frame.f_format = frame.f_vector = 0;
   		frame.f_pc = (int) p->p_addr->u_pcb.pcb_onfault;
   		return;

	case T_BUSERR|T_USER:	/* bus error */
		typ = BUS_OBJERR;
		ucode = code & ~T_USER;
		i = SIGBUS;
		break;
	case T_ADDRERR|T_USER:	/* address error */
		typ = BUS_ADRALN;
		ucode = code & ~T_USER;
		i = SIGBUS;
		break;

	case T_COPERR:		/* kernel coprocessor violation */
	case T_FMTERR|T_USER:	/* do all RTE errors come in as T_USER? */
	case T_FMTERR:		/* ...just in case... */
		/*
		 * The user has most likely trashed the RTE or FP state info
		 * in the stack frame of a signal handler.
		 */
		printf("pid %d: kernel %s exception\n", p->p_pid,
				 type==T_COPERR ? "coprocessor" : "format");
		type |= T_USER;
		p->p_sigacts->ps_sigact[SIGILL] = SIG_DFL;
		i = sigmask(SIGILL);
		p->p_sigignore &= ~i;
		p->p_sigcatch &= ~i;
		p->p_sigmask &= ~i;
		i = SIGILL;
		ucode = frame.f_format;	/* XXX was ILL_RESAD_FAULT */
		typ = ILL_COPROC;
		v = frame.f_pc;
		break;

	case T_COPERR|T_USER:	/* user coprocessor violation */
		/* What is a proper response here? */
		typ = FPE_FLTINV;
		ucode = 0;
		i = SIGFPE;
		break;

	case T_FPERR|T_USER:	/* 68881 exceptions */
		/*
		 * We pass along the 68881 status register which locore stashed
		 * in code for us.  Note that there is a possibility that the
		 * bit pattern of this register will conflict with one of the
		 * FPE_* codes defined in signal.h.  Fortunately for us, the
		 * only such codes we use are all in the range 1-7 and the low
		 * 3 bits of the status register are defined as 0 so there is
		 * no clash.
		 */
		typ = FPE_FLTRES;
		ucode = code;
		i = SIGFPE;
		v = frame.f_pc;
		break;

#if defined(M68040) || defined(M68060)
	case T_FPEMULI|T_USER:	/* unimplemented FP instruction */
	case T_FPEMULD|T_USER:	/* unimplemented FP data type */
		/* XXX need to FSAVE */
		printf("pid %d(%s): unimplemented FP %s at %x (EA %x)\n",
				 p->p_pid, p->p_comm,
				 frame.f_format == 2 ? "instruction" : "data type",
				 frame.f_pc, frame.f_fmt2.f_iaddr);
		/* XXX need to FRESTORE */
		typ = FPE_FLTINV;
		i = SIGFPE;
		v = frame.f_pc;
		break;
#endif

	case T_ILLINST|T_USER:	/* illegal instruction fault */
#ifdef COMPAT_HPUX
		if (p->p_emul == &emul_hpux) {
			typ = 0;
			ucode = HPUX_ILL_ILLINST_TRAP;
			i = SIGILL;
			break;
		}
#endif
		ucode = frame.f_format;	/* XXX was ILL_PRIVIN_FAULT */
		typ = ILL_ILLOPC;
		i = SIGILL;
		v = frame.f_pc;
		break;

	case T_PRIVINST|T_USER:	/* privileged instruction fault */
#ifdef COMPAT_HPUX
		if (p->p_emul == &emul_hpux)
			ucode = HPUX_ILL_PRIV_TRAP;
		else
#endif
		ucode	= frame.f_format;	/* XXX was ILL_PRIVIN_FAULT */
		typ = ILL_PRVOPC;
		i = SIGILL;
		v = frame.f_pc;
		break;

	case T_ZERODIV|T_USER:	/* Divide by zero */
#ifdef COMPAT_HPUX
		if (p->p_emul == &emul_hpux)
			ucode = HPUX_FPE_INTDIV_TRAP;
		else
#endif
		ucode	= frame.f_format;	/* XXX was FPE_INTDIV_TRAP */
		typ = FPE_INTDIV;
		i = SIGFPE;
		v = frame.f_pc;
		break;

	case T_CHKINST|T_USER:	/* CHK instruction trap */
#ifdef COMPAT_HPUX
		if (p->p_emul == &emul_hpux) {
			/* handled differently under hp-ux */
			i = SIGILL;
			ucode = HPUX_ILL_CHK_TRAP;
			break;
		}
#endif
		ucode = frame.f_format;	/* XXX was FPE_SUBRNG_TRAP */
		typ = FPE_FLTSUB;
		i = SIGFPE;
		v = frame.f_pc;
		break;

	case T_TRAPVINST|T_USER:	/* TRAPV instruction trap */
#ifdef COMPAT_HPUX
		if (p->p_emul == &emul_hpux) {
			/* handled differently under hp-ux */
			i = SIGILL;
			ucode = HPUX_ILL_TRAPV_TRAP;
			break;
		}
#endif
		ucode = frame.f_format;	/* XXX was FPE_INTOVF_TRAP */
		typ = ILL_ILLTRP;
		i = SIGILL;
		v = frame.f_pc;
		break;

		/*
		 * XXX: Trace traps are a nightmare.
		 *
		 *	HP-UX uses trap #1 for breakpoints,
		 *	OpenBSD/m68k uses trap #2,
		 *	SUN 3.x uses trap #15,
		 *	KGDB uses trap #15 (for kernel breakpoints; handled elsewhere).
		 *
		 * OpenBSD and HP-UX traps both get mapped by locore.s into
		 * T_TRACE.
		 * SUN 3.x traps get passed through as T_TRAP15 and are not really
		 * supported yet.
		 */
	case T_TRAP15:		/* kernel breakpoint */
#ifdef DEBUG
		printf("unexpected kernel trace trap, type = %d\n", type);
		printf("program counter = 0x%x\n", frame.f_pc);
#endif
		frame.f_sr &= ~PSL_T;
		return;

	case T_TRACE|T_USER:	/* user trace trap */
#ifdef COMPAT_SUNOS
		/*
		 * SunOS uses Trap #2 for a "CPU cache flush"
		 * Just flush the on-chip caches and return.
		 */
		if (p->p_emul == &emul_sunos) {
			ICIA();
			DCIU();
			return;
		}
#endif
		/* FALLTHROUGH */

	case T_TRACE:
	case T_TRAP15|T_USER:	/* SUN user trace trap */
		frame.f_sr &= ~PSL_T;
		i = SIGTRAP;
		typ = TRAP_TRACE;
		break;

	case T_ASTFLT:		/* system async trap, cannot happen */
		goto dopanic;

	case T_ASTFLT|T_USER:	/* user async trap */
		astpending = 0;
		/*
		 * We check for software interrupts first.  This is because
		 * they are at a higher level than ASTs, and on a VAX would
		 * interrupt the AST.  We assume that if we are processing
		 * an AST that we must be at IPL0 so we don't bother to
		 * check.  Note that we ensure that we are at least at SIR
		 * IPL while processing the SIR.
		 */
		spl1();
		/* FALLTHROUGH */

	case T_SSIR:		/* software interrupt */
	case T_SSIR|T_USER:
		while ((bit = ffs(ssir))) {
			--bit;
			ssir &= ~(1 << bit);
			uvmexp.softs++;
			if (sir_routines[bit])
				sir_routines[bit](sir_args[bit]);
		}
		/*
		 * If this was not an AST trap, we are all done.
		 */
		if (type != (T_ASTFLT|T_USER)) {
			uvmexp.traps--;
			return;
		}
		spl0();
		if (p->p_flag & P_OWEUPC) {
			p->p_flag &= ~P_OWEUPC;
			ADDUPROF(p);
		}
		goto out;

	case T_MMUFLT:		/* kernel mode page fault */
	case T_MMUFLT|T_USER:	/* page fault */
		{
			vm_offset_t va;
			struct vmspace *vm = NULL;
			struct vm_map *map;
			int rv;
			vm_prot_t ftype, vftype;
			extern struct vm_map *kernel_map;

			/* vmspace only significant if T_USER */
			if (p)
				vm = p->p_vmspace;

#ifdef DEBUG
			if ((mmudebug & MDB_WBFOLLOW) || MDB_ISPID(p->p_pid))
				printf("trap: T_MMUFLT pid=%d, code=%x, v=%x, pc=%x, sr=%x\n",
				    p->p_pid, code, v, frame.f_pc, frame.f_sr);
#endif
			/*
			 * It is only a kernel address space fault iff:
			 * 	1. (type & T_USER) == 0  and
			 * 	2. pcb_onfault not set or
			 *	3. pcb_onfault set but supervisor space data fault
			 * The last can occur during an exec() copyin where the
			 * argument space is lazy-allocated.
			 */
			if (type == T_MMUFLT &&
			    ((p && !p->p_addr->u_pcb.pcb_onfault) || KDFAULT(code)))
				map = kernel_map;
			else
				map = vm ? &vm->vm_map : kernel_map;
			if (WRFAULT(code)) {
				vftype = VM_PROT_WRITE;
				ftype = VM_PROT_READ | VM_PROT_WRITE;
			} else
				vftype = ftype = VM_PROT_READ;
			va = trunc_page((vm_offset_t)v);

			if (map == kernel_map && va == 0) {
				printf("trap: bad kernel access at %x\n", v);
				goto dopanic;
			}
#ifdef COMPAT_HPUX
			if (ISHPMMADDR(va)) {
				vm_offset_t bva;

				rv = pmap_mapmulti(map->pmap, va);
				if (rv) {
					bva = HPMMBASEADDR(va);
					rv = uvm_fault(map, bva, 0, ftype);
					if (rv == 0)
						(void) pmap_mapmulti(map->pmap, va);
				}
			} else
#endif
			rv = uvm_fault(map, va, 0, ftype);
#ifdef DEBUG
			if (rv && MDB_ISPID(p->p_pid))
				printf("uvm_fault(%x, %x, 0, %x) -> %x\n",
					 map, va, ftype, rv);
#endif
			/*
			 * If this was a stack access we keep track of the maximum
			 * accessed stack size.  Also, if vm_fault gets a protection
			 * failure it is due to accessing the stack region outside
			 * the current limit and we need to reflect that as an access
			 * error.
			 */
			if ((vm != NULL && (caddr_t)va >= vm->vm_maxsaddr)
			    && map != kernel_map) {
				if (rv == 0) {
					u_int nss;

					nss = btoc(USRSTACK-(u_int)va);
					if (nss > vm->vm_ssize)
						vm->vm_ssize = nss;
				} else if (rv == EACCES)
					rv = EFAULT;
			}
			if (rv == 0) {
				if (type == T_MMUFLT) {
#if defined(M68040)
					if (mmutype == MMU_68040)
						(void) writeback(&frame, 1);
#endif
					return;
				}
				goto out;
			}
			if (type == T_MMUFLT) {
				if (p && p->p_addr->u_pcb.pcb_onfault)
					goto copyfault;
				printf("uvm_fault(%p, %lx, 0, %x) -> %x\n",
					 map, va, ftype, rv);
				printf("  type %x, code [mmu,,ssw]: %x\n",
					 type, code);
				goto dopanic;
			}
			frame.f_pad = code & 0xffff;
			ucode = vftype;
			typ = SEGV_MAPERR;
			i = SIGSEGV;
			break;
		}
	}
	sv.sival_int = v;
	trapsignal(p, i, ucode, typ, sv);
out:
	if ((type & T_USER) == 0)
		return;
	userret(p, &frame, sticks, v, 1);
}

#if defined(M68040)
#ifdef DEBUG
struct writebackstats {
	int calls;
	int cpushes;
	int move16s;
	int wb1s, wb2s, wb3s;
	int wbsize[4];
} wbstats;

char *f7sz[] = { "longword", "byte", "word", "line"};
char *f7tt[] = { "normal", "MOVE16", "AFC", "ACK"};
char *f7tm[] = { "d-push", "u-data", "u-code", "M-data",
	"M-code", "k-data", "k-code", "RES"};
char wberrstr[] =
"WARNING: pid %d(%s) writeback [%s] failed, pc=%x fa=%x wba=%x wbd=%x\n";
#endif

int
writeback(fp, docachepush)
	struct frame *fp;
	int docachepush;
{
	register struct fmt7 *f = &fp->f_fmt7;
	register struct proc *p = curproc;
	int err = 0;
	u_int fa;
	caddr_t oonfault = p->p_addr->u_pcb.pcb_onfault;

#ifdef DEBUG
	if ((mmudebug & MDB_WBFOLLOW) || MDB_ISPID(p->p_pid)) {
		printf(" pid=%d, fa=%x,", p->p_pid, f->f_fa);
		dumpssw(f->f_ssw);
	}
	wbstats.calls++;
#endif
	/*
	 * Deal with special cases first.
	 */
	if ((f->f_ssw & SSW4_TMMASK) == SSW4_TMDCP) {
		/*
		 * Dcache push fault.
		 * Line-align the address and write out the push data to
		 * the indicated physical address.
		 */
#ifdef DEBUG
		if ((mmudebug & MDB_WBFOLLOW) || MDB_ISPID(p->p_pid)) {
			printf(" pushing %s to PA %x, data %x",
					 f7sz[(f->f_ssw & SSW4_SZMASK) >> 5],
					 f->f_fa, f->f_pd0);
			if ((f->f_ssw & SSW4_SZMASK) == SSW4_SZLN)
				printf("/%x/%x/%x",
						 f->f_pd1, f->f_pd2, f->f_pd3);
			printf("\n");
		}
		if (f->f_wb1s & SSW4_WBSV)
			panic("writeback: cache push with WB1S valid");
		wbstats.cpushes++;
#endif
		/*
		 * XXX there are security problems if we attempt to do a
		 * cache push after a signal handler has been called.
		 */
		if (docachepush) {
			paddr_t pa;

			pmap_enter(pmap_kernel(), (vm_offset_t)vmmap,
						  trunc_page(f->f_fa), VM_PROT_WRITE, VM_PROT_WRITE|PMAP_WIRED);
			pmap_update(pmap_kernel());
			fa = (u_int)&vmmap[(f->f_fa & PGOFSET) & ~0xF];
			bcopy((caddr_t)&f->f_pd0, (caddr_t)fa, 16);
			pmap_extract(pmap_kernel(), (vm_offset_t)fa, &pa);
			DCFL(pa);
			pmap_remove(pmap_kernel(), (vm_offset_t)vmmap,
							(vm_offset_t)&vmmap[NBPG]);
			pmap_update(pmap_kernel());
		} else
			printf("WARNING: pid %d(%s) uid %u: CPUSH not done\n",
					 p->p_pid, p->p_comm, p->p_ucred->cr_uid);
	} else if ((f->f_ssw & (SSW4_RW|SSW4_TTMASK)) == SSW4_TTM16) {
		/*
		 * MOVE16 fault.
		 * Line-align the address and write out the push data to
		 * the indicated virtual address.
		 */
#ifdef DEBUG
		if ((mmudebug & MDB_WBFOLLOW) || MDB_ISPID(p->p_pid))
			printf(" MOVE16 to VA %x(%x), data %x/%x/%x/%x\n",
					 f->f_fa, f->f_fa & ~0xF, f->f_pd0, f->f_pd1,
					 f->f_pd2, f->f_pd3);
		if (f->f_wb1s & SSW4_WBSV)
			panic("writeback: MOVE16 with WB1S valid");
		wbstats.move16s++;
#endif
		if (KDFAULT(f->f_wb1s))
			bcopy((caddr_t)&f->f_pd0, (caddr_t)(f->f_fa & ~0xF), 16);
		else
			err = suline((caddr_t)(f->f_fa & ~0xF), (caddr_t)&f->f_pd0);
		if (err) {
			fa = f->f_fa & ~0xF;
#ifdef DEBUG
			if (mmudebug & MDB_WBFAILED)
				printf(wberrstr, p->p_pid, p->p_comm,
						 "MOVE16", fp->f_pc, f->f_fa,
						 f->f_fa & ~0xF, f->f_pd0);
#endif
		}
	} else if (f->f_wb1s & SSW4_WBSV) {
		/*
		 * Writeback #1.
		 * Position the "memory-aligned" data and write it out.
		 */
		u_int wb1d = f->f_wb1d;
		int off;

#ifdef DEBUG
		if ((mmudebug & MDB_WBFOLLOW) || MDB_ISPID(p->p_pid))
			dumpwb(1, f->f_wb1s, f->f_wb1a, f->f_wb1d);
		wbstats.wb1s++;
		wbstats.wbsize[(f->f_wb2s&SSW4_SZMASK)>>5]++;
#endif
		off = (f->f_wb1a & 3) * 8;
		switch (f->f_wb1s & SSW4_SZMASK) {
			case SSW4_SZLW:
				if (off)
					wb1d = (wb1d >> (32 - off)) | (wb1d << off);
				if (KDFAULT(f->f_wb1s))
					*(long *)f->f_wb1a = wb1d;
				else
					err = copyout(&wb1d,
					    (caddr_t)f->f_wb1a, sizeof(int));
				break;
			case SSW4_SZB:
				off = 24 - off;
				if (off)
					wb1d >>= off;
				if (KDFAULT(f->f_wb1s))
					*(char *)f->f_wb1a = wb1d;
				else {
					char tmp = wb1d;

					err = copyout(&tmp,
					    (caddr_t)f->f_wb1a, sizeof(char));
				}
				break;
			case SSW4_SZW:
				off = (off + 16) % 32;
				if (off)
					wb1d = (wb1d >> (32 - off)) | (wb1d << off);
				if (KDFAULT(f->f_wb1s))
					*(short *)f->f_wb1a = wb1d;
				else {
					short tmp = wb1d;

					err = copyout(&tmp,
					    (caddr_t)f->f_wb1a, sizeof(long));
				}
				break;
		}
		if (err) {
			fa = f->f_wb1a;
#ifdef DEBUG
			if (mmudebug & MDB_WBFAILED)
				printf(wberrstr, p->p_pid, p->p_comm,
						 "#1", fp->f_pc, f->f_fa,
						 f->f_wb1a, f->f_wb1d);
#endif
		}
	}
	/*
	 * Deal with the "normal" writebacks.
	 *
	 * XXX writeback2 is known to reflect a LINE size writeback after
	 * a MOVE16 was already dealt with above.  Ignore it.
	 */
	if (err == 0 && (f->f_wb2s & SSW4_WBSV) &&
		 (f->f_wb2s & SSW4_SZMASK) != SSW4_SZLN) {
#ifdef DEBUG
		if ((mmudebug & MDB_WBFOLLOW) || MDB_ISPID(p->p_pid))
			dumpwb(2, f->f_wb2s, f->f_wb2a, f->f_wb2d);
		wbstats.wb2s++;
		wbstats.wbsize[(f->f_wb2s&SSW4_SZMASK)>>5]++;
#endif
		switch (f->f_wb2s & SSW4_SZMASK) {
			case SSW4_SZLW:
				if (KDFAULT(f->f_wb2s))
					*(long *)f->f_wb2a = f->f_wb2d;
				else
					err = copyout(&f->f_wb2d,
					    (caddr_t)f->f_wb2a, sizeof(int));
				break;
			case SSW4_SZB:
				if (KDFAULT(f->f_wb2s))
					*(char *)f->f_wb2a = f->f_wb2d;
				else {
					char tmp = f->f_wb2d;

					err = copyout(&tmp,
					    (caddr_t)f->f_wb2a, sizeof(char));
				}
				break;
			case SSW4_SZW:
				if (KDFAULT(f->f_wb2s))
					*(short *)f->f_wb2a = f->f_wb2d;
				else {
					short tmp = f->f_wb2d;

					err = copyout(&tmp,
					    (caddr_t)f->f_wb2a, sizeof(short));
				}
				break;
		}
		if (err) {
			fa = f->f_wb2a;
#ifdef DEBUG
			if (mmudebug & MDB_WBFAILED) {
				printf(wberrstr, p->p_pid, p->p_comm,
						 "#2", fp->f_pc, f->f_fa,
						 f->f_wb2a, f->f_wb2d);
				dumpssw(f->f_ssw);
				dumpwb(2, f->f_wb2s, f->f_wb2a, f->f_wb2d);
			}
#endif
		}
	}
	if (err == 0 && (f->f_wb3s & SSW4_WBSV)) {
#ifdef DEBUG
		if ((mmudebug & MDB_WBFOLLOW) || MDB_ISPID(p->p_pid))
			dumpwb(3, f->f_wb3s, f->f_wb3a, f->f_wb3d);
		wbstats.wb3s++;
		wbstats.wbsize[(f->f_wb3s&SSW4_SZMASK)>>5]++;
#endif
		switch (f->f_wb3s & SSW4_SZMASK) {
			case SSW4_SZLW:
				if (KDFAULT(f->f_wb3s))
					*(long *)f->f_wb3a = f->f_wb3d;
				else
					err = copyout(&f->f_wb3d,
					    (caddr_t)f->f_wb3a, sizeof(int));
				break;
			case SSW4_SZB:
				if (KDFAULT(f->f_wb3s))
					*(char *)f->f_wb3a = f->f_wb3d;
				else {
					char tmp = f->f_wb3d;

					err = copyout(&tmp,
					    (caddr_t)f->f_wb3a, sizeof(char));
				}
				break;
			case SSW4_SZW:
				if (KDFAULT(f->f_wb3s))
					*(short *)f->f_wb3a = f->f_wb3d;
				else {
					short tmp = f->f_wb3d;

					err = copyout(&tmp,
					    (caddr_t)f->f_wb3a, sizeof(short));
				}
				break;
#ifdef DEBUG
			case SSW4_SZLN:
				panic("writeback: wb3s indicates LINE write");
#endif
		}
		if (err) {
			fa = f->f_wb3a;
#ifdef DEBUG
			if (mmudebug & MDB_WBFAILED)
				printf(wberrstr, p->p_pid, p->p_comm,
						 "#3", fp->f_pc, f->f_fa,
						 f->f_wb3a, f->f_wb3d);
#endif
		}
	}
	p->p_addr->u_pcb.pcb_onfault = oonfault;
	/*
	 * Any problems are SIGSEGV's
	 */
	if (err)
		err = SIGSEGV;
	return (err);
}

#ifdef DEBUG
void
dumpssw(ssw)
	register u_short ssw;
{
	printf(" SSW: %x: ", ssw);
	if (ssw & SSW4_CP)
		printf("CP,");
	if (ssw & SSW4_CU)
		printf("CU,");
	if (ssw & SSW4_CT)
		printf("CT,");
	if (ssw & SSW4_CM)
		printf("CM,");
	if (ssw & SSW4_MA)
		printf("MA,");
	if (ssw & SSW4_ATC)
		printf("ATC,");
	if (ssw & SSW4_LK)
		printf("LK,");
	if (ssw & SSW4_RW)
		printf("RW,");
	printf(" SZ=%s, TT=%s, TM=%s\n",
			 f7sz[(ssw & SSW4_SZMASK) >> 5],
			 f7tt[(ssw & SSW4_TTMASK) >> 3],
			 f7tm[ssw & SSW4_TMMASK]);
}

void
dumpwb(num, s, a, d)
	int num;
	u_short s;
	u_int a, d;
{
	register struct proc *p = curproc;
	vm_offset_t pa;
	int tmp;

	printf(" writeback #%d: VA %x, data %x, SZ=%s, TT=%s, TM=%s\n",
			 num, a, d, f7sz[(s & SSW4_SZMASK) >> 5],
			 f7tt[(s & SSW4_TTMASK) >> 3], f7tm[s & SSW4_TMMASK]);
	printf("	       PA ");
	if (pmap_extract(p->p_vmspace->vm_map.pmap, (vm_offset_t)a, &pa) == FALSE)
		printf("<invalid address>");
	else {
		if (copyin((caddr_t)a, &tmp, sizeof(int)) == 0)
			printf("%lx, current value %lx", pa, tmp);
		else
			printf("%lx, current value inaccessible", pa);
	}
	printf("\n");
}
#endif
#endif

/*
 * Process a system call.
 */
void
syscall(code, frame)
	register_t code;
	struct frame frame;
{
	register caddr_t params;
	register struct sysent *callp;
	register struct proc *p;
	int error, opc, nsys;
	size_t argsize;
	register_t args[8], rval[2];
	u_quad_t sticks;
#ifdef COMPAT_SUNOS
	extern struct emul emul_sunos;
#endif
	uvmexp.syscalls++;
	
	if (!USERMODE(frame.f_sr))
		panic("syscall");
	p = curproc;
	sticks = p->p_sticks;
	p->p_md.md_regs = frame.f_regs;
	opc = frame.f_pc;

	nsys = p->p_emul->e_nsysent;
	callp = p->p_emul->e_sysent;

#ifdef COMPAT_SUNOS
	if (p->p_emul == &emul_sunos) {
		/*
		 * SunOS passes the syscall-number on the stack, whereas
		 * BSD passes it in D0. So, we have to get the real "code"
		 * from the stack, and clean up the stack, as SunOS glue
		 * code assumes the kernel pops the syscall argument the
		 * glue pushed on the stack. Sigh...
		 */
		if (copyin((caddr_t)frame.f_regs[SP], &code,
		    sizeof(register_t)) != 0)
			code = -1;

		/*
		 * XXX
		 * Don't do this for sunos_sigreturn, as there's no stored pc
		 * on the stack to skip, the argument follows the syscall
		 * number without a gap.
		 */
		if (code != SUNOS_SYS_sigreturn) {
			frame.f_regs[SP] += sizeof (int);
			/*
			 * remember that we adjusted the SP,
			 * might have to undo this if the system call
			 * returns ERESTART.
			 */
			p->p_md.md_flags |= MDP_STACKADJ;
		} else
			p->p_md.md_flags &= ~MDP_STACKADJ;
	}
#endif

	params = (caddr_t)frame.f_regs[SP] + sizeof(int);

	switch (code) {
	case SYS_syscall:
		/*
		 * Code is first argument, followed by actual args.
		 */
		if (copyin(params, &code, sizeof(register_t)) != 0)
			code = -1;
		params += sizeof(int);
		/*
		 * XXX sigreturn requires special stack manipulation
		 * that is only done if entered via the sigreturn
		 * trap.  Cannot allow it here so make sure we fail.
		 */
		if (code == SYS_sigreturn)
			code = nsys;
		break;
	case SYS___syscall:
		/*
		 * Like syscall, but code is a quad, so as to maintain
		 * quad alignment for the rest of the arguments.
		 */
		if (callp != sysent)
			break;
		if (copyin(params + _QUAD_LOWWORD * sizeof(int), &code,
		    sizeof(register_t)) != 0)
			code = -1;
		params += sizeof(quad_t);
		break;
	default:
		break;
	}
	if (code < 0 || code >= nsys)
		callp += p->p_emul->e_nosys;		/* illegal */
	else
		callp	+= code;
	argsize = callp->sy_argsize;
	if (argsize)
		error = copyin(params, (caddr_t)args, argsize);
	else
		error	= 0;
#ifdef SYSCALL_DEBUG
	scdebug_call(p, code, args);
#endif
#ifdef KTRACE
	if (KTRPOINT(p, KTR_SYSCALL))
		ktrsyscall(p, code, argsize, args);
#endif
	if (error)
		goto bad;
	rval[0] = 0;
	rval[1] = frame.f_regs[D1];
#if NSYSTRACE > 0
	if (ISSET(p->p_flag, P_SYSTRACE))
		error = systrace_redirect(code, p, args, rval);
	else
#endif
		error = (*callp->sy_call)(p, args, rval);
	switch (error) {
	case 0:
		frame.f_regs[D0] = rval[0];
		frame.f_regs[D1] = rval[1];
		frame.f_sr &= ~PSL_C;	/* carry bit */
		break;
	case ERESTART:
		/*
		 * We always enter through a `trap' instruction, which is 2
		 * bytes, so adjust the pc by that amount.
		 */
		frame.f_pc = opc - 2;
		break;
	case EJUSTRETURN:
		/* nothing to do */
		break;
	default:
bad:
		if (p->p_emul->e_errno)
			error = p->p_emul->e_errno[error];
		frame.f_regs[D0] = error;
		frame.f_sr |= PSL_C;	/* carry bit */
		break;
	}

#ifdef SYSCALL_DEBUG
	scdebug_ret(p, code, error, rval);
#endif
#ifdef COMPAT_SUNOS
	/* need new p-value for this */
	if (error == ERESTART && (p->p_md.md_flags & MDP_STACKADJ))
		frame.f_regs[SP] -= sizeof (int);
#endif
	userret(p, &frame, sticks, 0, 0);
#ifdef KTRACE
	if (KTRPOINT(p, KTR_SYSRET))
		ktrsysret(p, code, error, rval[0]);
#endif
}

/*
 * Allocation routines for software interrupts.
 */
u_long
allocate_sir(proc, arg)
	void (*proc)(void *);
	void *arg;
{
	int bit;

	if (next_sir >= NSIR)
		panic("allocate_sir: none left");
	bit = next_sir++;
	sir_routines[bit] = proc;
	sir_args[bit] = arg;
	return (1 << bit);
}

void
init_sir()
{
	extern void netintr(void *);

	sir_routines[0] = netintr;
	sir_routines[1] = (void (*)(void *))softclock;
	next_sir = 2;
}

struct intrhand *intrs[256];

/*
 * XXX
 * This is an EXTREMELY good candidate for rewriting in assembly!!
 */
#ifndef INTR_ASM
void
hardintr(pc, evec, frame)
	int pc;
	int evec;
	void *frame;
{
	extern void straytrap(int, u_short);
	int vec = (evec & 0xfff) >> 2;	/* XXX should be m68k macro? */
	/*extern u_long intrcnt[];*/	/* XXX from locore */
	struct intrhand *ih;
	int count = 0;
	int r;

	uvmexp.intrs++;
/*	intrcnt[level]++; */
	for (ih = intrs[vec]; ih; ih = ih->ih_next) {
#if 0
		if (vec >= 0x70 && vec <= 0x73) {
			zscnputc(0, '[');
			zscnputc(0, '0' + (vec - 0x70));
		}
#endif
		r = (*ih->ih_fn)(ih->ih_wantframe ? frame : ih->ih_arg);
		if (r > 0)
			count++;
	}
	if (count != 0)
		return;

	straytrap(pc, evec);
}
#endif /* !INTR_ASM */

/*
 * find a useable interrupt vector in the range start, end. It starts at
 * the end of the range, and searches backwards (to increase the chances
 * of not conflicting with more normal users)
 */
int
intr_findvec(start, end)
	int start, end;
{
	extern u_long *vectab[], hardtrap, badtrap;
	int vec;

	if (start < 0 || end > 255 || start > end)
		return (-1);
	for (vec = end; vec > start; --vec)
		if (vectab[vec] == &badtrap || vectab[vec] == &hardtrap)
			return (vec);
	return (-1);
}

/*
 * Chain the interrupt handler in. But first check if the vector
 * offset chosen is legal. It either must be a badtrap (not allocated
 * for a `system' purpose), or it must be a hardtrap (ie. already
 * allocated to deal with chained interrupt handlers).
 */
int
intr_establish(vec, ih)
	int vec;
	struct intrhand *ih;
{
	extern u_long *vectab[], hardtrap, badtrap;
	struct intrhand *ihx;

	if (vectab[vec] != &badtrap && vectab[vec] != &hardtrap) {
		printf("intr_establish: vec %d unavailable\n", vec);
		return (-1);
	}
	vectab[vec] = &hardtrap;

	ih->ih_next = NULL;	/* just in case */

	/* attach at tail */
	if ((ihx = intrs[vec])) {
		while (ihx->ih_next)
			ihx = ihx->ih_next;
		ihx->ih_next = ih;
	} else
		intrs[vec] = ih;
	return (0);
}

#ifdef DDB
#include <sys/reboot.h>
#include <machine/db_machdep.h>
#include <ddb/db_command.h>

void db_prom_cmd(db_expr_t, int, db_expr_t, char *);
void db_machine_init(void);

/* ARGSUSED */
void
db_prom_cmd(addr, have_addr, count, modif)
	db_expr_t addr;
	int have_addr;
	db_expr_t count;
	char *modif;
{
	doboot();
}

struct db_command db_machine_cmds[] = {
	{ "prom",   db_prom_cmd,   0, 0},
	{ (char *)0,}
};

void
db_machine_init()
{
	db_machine_commands_install(db_machine_cmds);
}
#endif /* DDB */
