/*
 * Copyright (c) 1999, 2014, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

// no precompiled headers
#include "jvm.h"
#include "asm/macroAssembler.hpp"
#include "classfile/classLoader.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/vmSymbols.hpp"
#include "code/icBuffer.hpp"
#include "code/vtableStubs.hpp"
#include "interpreter/interpreter.hpp"
#include "memory/allocation.inline.hpp"
#include "os_share_haiku.hpp"
#include "prims/jniFastGetField.hpp"
#include "prims/jvm_misc.hpp"
#include "runtime/arguments.hpp"
#include "runtime/extendedPC.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/java.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/osThread.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/thread.inline.hpp"
#include "runtime/timer.hpp"
#include "utilities/events.hpp"
#include "utilities/vmError.hpp"

// put OS-includes here
# include <sys/types.h>
# include <sys/mman.h>
# include <pthread.h>
# include <signal.h>
# include <errno.h>
# include <dlfcn.h>
# include <stdlib.h>
# include <stdio.h>
# include <unistd.h>
# include <sys/resource.h>
# include <pthread.h>
# include <sys/stat.h>
# include <sys/time.h>
# include <sys/utsname.h>
# include <sys/socket.h>
# include <sys/wait.h>
# include <pwd.h>
# include <poll.h>

#ifdef AMD64
#define REG_SP rsp
#define REG_PC rip
#define REG_FP rbp
#define SPELL_REG_SP "rsp"
#define SPELL_REG_FP "rbp"
#else
#define REG_SP esp
#define REG_PC eip
#define REG_FP ebp
#define SPELL_REG_SP "esp"
#define SPELL_REG_FP "ebp"
#endif // AMD64

address os::current_stack_pointer() {
#ifdef SPARC_WORKS
  register void *esp;
  __asm__("mov %%"SPELL_REG_SP", %0":"=r"(esp));
  return (address) ((char*)esp + sizeof(long)*2);
#elif defined(__clang__)
  intptr_t* esp;
  __asm__ __volatile__ ("mov %%"SPELL_REG_SP", %0":"=r"(esp):);
  return (address) esp;
#else
  register void *esp __asm__ (SPELL_REG_SP);
  return (address) esp;
#endif
}

char* os::non_memory_address_word() {
  // Must never look like an address returned by reserve_memory,
  // even in its subfields (as defined by the CPU immediate fields,
  // if the CPU splits constants across multiple instructions).

  return (char*) -1;
}

address os::Haiku::ucontext_get_pc(const ucontext_t * uc) {
  return (address)uc->uc_mcontext.REG_PC;
}

void os::Haiku::ucontext_set_pc(ucontext_t* uc, address pc) {
  uc->uc_mcontext.REG_PC = (unsigned long)pc;
}

intptr_t* os::Haiku::ucontext_get_sp(const ucontext_t * uc) {
  return (intptr_t*)uc->uc_mcontext.REG_SP;
}

intptr_t* os::Haiku::ucontext_get_fp(const ucontext_t * uc) {
  return (intptr_t*)uc->uc_mcontext.REG_FP;
}

// For Forte Analyzer AsyncGetCallTrace profiling support - thread
// is currently interrupted by SIGPROF.
// os::Solaris::fetch_frame_from_ucontext() tries to skip nested signal
// frames. Currently we don't do that on Linux, so it's the same as
// os::fetch_frame_from_context().
ExtendedPC os::Haiku::fetch_frame_from_ucontext(Thread* thread,
  ucontext_t* uc, intptr_t** ret_sp, intptr_t** ret_fp) {

  assert(thread != NULL, "just checking");
  assert(ret_sp != NULL, "just checking");
  assert(ret_fp != NULL, "just checking");

  return os::fetch_frame_from_context(uc, ret_sp, ret_fp);
}

ExtendedPC os::fetch_frame_from_context(const void* ucVoid,
                    intptr_t** ret_sp, intptr_t** ret_fp) {

  ExtendedPC  epc;
  const ucontext_t* uc = (const ucontext_t*)ucVoid;

  if (uc != NULL) {
    epc = ExtendedPC(os::Haiku::ucontext_get_pc(uc));
    if (ret_sp) *ret_sp = os::Haiku::ucontext_get_sp(uc);
    if (ret_fp) *ret_fp = os::Haiku::ucontext_get_fp(uc);
  } else {
    // construct empty ExtendedPC for return value checking
    epc = ExtendedPC(NULL);
    if (ret_sp) *ret_sp = (intptr_t *)NULL;
    if (ret_fp) *ret_fp = (intptr_t *)NULL;
  }

  return epc;
}

frame os::fetch_frame_from_context(const void* ucVoid) {
  intptr_t* sp;
  intptr_t* fp;
  ExtendedPC epc = fetch_frame_from_context(ucVoid, &sp, &fp);
  return frame(sp, fp, epc.pc());
}

frame os::fetch_frame_from_ucontext(Thread* thread, void* ucVoid) {
  intptr_t* sp;
  intptr_t* fp;
  ExtendedPC epc = os::Haiku::fetch_frame_from_ucontext(thread, (ucontext_t*)ucVoid, &sp, &fp);
  return frame(sp, fp, epc.pc());
}

bool os::Haiku::get_frame_at_stack_banging_point(JavaThread* thread, ucontext_t* uc, frame* fr) {
  address pc = (address) os::Haiku::ucontext_get_pc(uc);
  if (Interpreter::contains(pc)) {
    // interpreter performs stack banging after the fixed frame header has
    // been generated while the compilers perform it before. To maintain
    // semantic consistency between interpreted and compiled frames, the
    // method returns the Java sender of the current frame.
    *fr = os::fetch_frame_from_ucontext(thread, uc);
    if (!fr->is_first_java_frame()) {
      // get_frame_at_stack_banging_point() is only called when we
      // have well defined stacks so java_sender() calls do not need
      // to assert safe_for_sender() first.
      *fr = fr->java_sender();
    }
  } else {
    // more complex code with compiled code
    assert(!Interpreter::contains(pc), "Interpreted methods should have been handled above");
    CodeBlob* cb = CodeCache::find_blob(pc);
    if (cb == NULL || !cb->is_nmethod() || cb->is_frame_complete_at(pc)) {
      // Not sure where the pc points to, fallback to default
      // stack overflow handling
      return false;
    } else {
      *fr = os::fetch_frame_from_ucontext(thread, uc);
      // in compiled code, the stack banging is performed just after the return pc
      // has been pushed on the stack
      *fr = frame(fr->sp() + 1, fr->fp(), (address)*(fr->sp()));
      if (!fr->is_java_frame()) {
        // See java_sender() comment above.
        *fr = fr->java_sender();
      }
    }
  }
  assert(fr->is_java_frame(), "Safety check");
  return true;
}

// By default, gcc always save frame pointer (%ebp/%rbp) on stack. It may get
// turned off by -fomit-frame-pointer,
frame os::get_sender_for_C_frame(frame* fr) {
  return frame(fr->sender_sp(), fr->link(), fr->sender_pc());
}

intptr_t* _get_previous_fp() {
#ifdef SPARC_WORKS
  register intptr_t **ebp;
  __asm__("mov %%"SPELL_REG_FP", %0":"=r"(ebp));
#elif defined(__clang__)
  intptr_t **ebp;
  __asm__ __volatile__ ("mov %%"SPELL_REG_FP", %0":"=r"(ebp):);
#else
  register intptr_t **ebp __asm__ (SPELL_REG_FP);
#endif
  return (intptr_t*) *ebp;   // we want what it points to.
}


frame os::current_frame() {
  intptr_t* fp = _get_previous_fp();
  frame myframe((intptr_t*)os::current_stack_pointer(),
                (intptr_t*)fp,
                CAST_FROM_FN_PTR(address, os::current_frame));
  if (os::is_first_C_frame(&myframe)) {
    // stack is not walkable
    return frame();
  } else {
    return os::get_sender_for_C_frame(&myframe);
  }
}

// Utility functions

// From IA32 System Programming Guide
enum {
  trap_page_fault = 0xE
};

extern "C" JNIEXPORT int
JVM_handle_haiku_signal(int sig,
                        siginfo_t* info,
                        void* ucVoid,
                        int abort_if_unrecognized) {
  ucontext_t* uc = (ucontext_t*) ucVoid;

  Thread* t = Thread::current_or_null_safe();

  // Must do this before SignalHandlerMark, if crash protection installed we will longjmp away
  // (no destructors can be run)
  os::ThreadCrashProtection::check_crash_protection(sig, t);

  SignalHandlerMark shm(t);

  // Note: it's not uncommon that JNI code uses signal/sigset to install
  // then restore certain signal handler (e.g. to temporarily block SIGPIPE,
  // or have a SIGILL handler when detecting CPU type). When that happens,
  // JVM_handle_haiku_signal() might be invoked with junk info/ucVoid. To
  // avoid unnecessary crash when libjsig is not preloaded, try handle signals
  // that do not require siginfo/ucontext first.

  if (sig == SIGPIPE || sig == SIGXFSZ) {
    // allow chained handler to go first
    if (os::Haiku::chained_handler(sig, info, ucVoid)) {
      return true;
    } else {
      if (PrintMiscellaneous && (WizardMode || Verbose)) {
        char buf[64];
        warning("Ignoring %s - see bugs 4229104 or 646499219",
                os::exception_name(sig, buf, sizeof(buf)));
      }
      return true;
    }
  }

  JavaThread* thread = NULL;
  VMThread* vmthread = NULL;
  if (os::Haiku::signal_handlers_are_installed) {
    if (t != NULL ){
      if(t->is_Java_thread()) {
        thread = (JavaThread*)t;
      }
      else if(t->is_VM_thread()){
        vmthread = (VMThread *)t;
      }
    }
  }
/*
  NOTE: does not seem to work on linux.
  if (info == NULL || info->si_code <= 0 || info->si_code == SI_NOINFO) {
    // can't decode this kind of signal
    info = NULL;
  } else {
    assert(sig == info->si_signo, "bad siginfo");
  }
*/
  // decide if this trap can be handled by a stub
  address stub = NULL;

  address pc          = NULL;

  //%note os_trap_1
  if (info != NULL && uc != NULL && thread != NULL) {
    pc = (address) os::Haiku::ucontext_get_pc(uc);

    if (StubRoutines::is_safefetch_fault(pc)) {
      os::Haiku::ucontext_set_pc(uc, StubRoutines::continuation_for_safefetch_fault(pc));
      return true;
    }

    // Handle ALL stack overflow variations here
    if (sig == SIGSEGV) {
      address addr = (address) info->si_addr;

      // check if fault address is within thread stack
      if (thread->on_local_stack(addr)) {
        // stack overflow
        if (thread->in_stack_yellow_reserved_zone(addr)) {
          if (thread->thread_state() == _thread_in_Java) {
            if (thread->in_stack_reserved_zone(addr)) {
              frame fr;
              if (os::Haiku::get_frame_at_stack_banging_point(thread, uc, &fr)) {
                assert(fr.is_java_frame(), "Must be a Java frame");
                frame activation = SharedRuntime::look_for_reserved_stack_annotated_method(thread, fr);
                if (activation.sp() != NULL) {
                  thread->disable_stack_reserved_zone();
                  if (activation.is_interpreted_frame()) {
                    thread->set_reserved_stack_activation((address)(
                      activation.fp() + frame::interpreter_frame_initial_sp_offset));
                  } else {
                    thread->set_reserved_stack_activation((address)activation.unextended_sp());
                  }
                  return 1;
                }
              }
            }
            // Throw a stack overflow exception.  Guard pages will be reenabled
            // while unwinding the stack.
            thread->disable_stack_yellow_reserved_zone();
            stub = SharedRuntime::continuation_for_implicit_exception(thread, pc, SharedRuntime::STACK_OVERFLOW);
          } else {
            // Thread was in the vm or native code.  Return and try to finish.
            thread->disable_stack_yellow_reserved_zone();
            return 1;
          }
        } else if (thread->in_stack_red_zone(addr)) {
          // Fatal red zone violation.  Disable the guard pages and fall through
          // to handle_unexpected_exception way down below.
          thread->disable_stack_red_zone();
          tty->print_raw_cr("An irrecoverable stack overflow has occurred.");
        } else {
          // The thread stack is already entirely mapped on Haiku, but I'm
          // keeping this code around in case that changes.
#if 0
          // Accessing stack address below sp may cause SEGV if current
          // thread has MAP_GROWSDOWN stack. This should only happen when
          // current thread was created by user code with MAP_GROWSDOWN flag
          // and then attached to VM. See notes in os_linux.cpp.
          if (thread->osthread()->expanding_stack() == 0) {
             thread->osthread()->set_expanding_stack();
             if (os::Haiku::manually_expand_stack(thread, addr)) {
               thread->osthread()->clear_expanding_stack();
               return 1;
             }
             thread->osthread()->clear_expanding_stack();
          } else {
             fatal("recursive segv. expanding stack.");
          }
#endif
          // We should never reach here
          fatal("segv. within stack range, but not on guard pages!");
        }
      }
    }

    if ((sig == SIGSEGV) && VM_Version::is_cpuinfo_segv_addr(pc)) {
      // Verify that OS save/restore AVX registers.
      stub = VM_Version::cpuinfo_cont_addr();
    }

    if (thread->thread_state() == _thread_in_Java) {
      // Java thread running in Java code => find exception handler if any
      // a fault inside compiled code, the interpreter, or a stub

      if (sig == SIGSEGV && os::is_poll_address((address)info->si_addr)) {
        stub = SharedRuntime::get_poll_stub(pc);
      } else if (sig == SIGBUS /* && info->si_code == BUS_OBJERR */) {
        // BugId 4454115: A read from a MappedByteBuffer can fault
        // here if the underlying file has been truncated.
        // Do not crash the VM in such a case.
        CodeBlob* cb = CodeCache::find_blob_unsafe(pc);
        CompiledMethod* nm = (cb != NULL) ? cb->as_compiled_method_or_null() : NULL;
        if (nm != NULL && nm->has_unsafe_access()) {
          address next_pc = Assembler::locate_next_instruction(pc);
          stub = SharedRuntime::handle_unsafe_access(thread, next_pc);
        }
      }
      else

#ifdef AMD64
      if (sig == SIGFPE  &&
          (info->si_code == FPE_INTDIV || info->si_code == FPE_FLTDIV)) {
        stub =
          SharedRuntime::
          continuation_for_implicit_exception(thread,
                                              pc,
                                              SharedRuntime::
                                              IMPLICIT_DIVIDE_BY_ZERO);
#else
      if (sig == SIGFPE /* && info->si_code == FPE_INTDIV */) {
        // HACK: si_code does not work on linux 2.2.12-20!!!
        int op = pc[0];
        if (op == 0xDB) {
          // FIST
          // TODO: The encoding of D2I in i486.ad can cause an exception
          // prior to the fist instruction if there was an invalid operation
          // pending. We want to dismiss that exception. From the win_32
          // side it also seems that if it really was the fist causing
          // the exception that we do the d2i by hand with different
          // rounding. Seems kind of weird.
          // NOTE: that we take the exception at the NEXT floating point instruction.
          assert(pc[0] == 0xDB, "not a FIST opcode");
          assert(pc[1] == 0x14, "not a FIST opcode");
          assert(pc[2] == 0x24, "not a FIST opcode");
          return true;
        } else if (op == 0xF7) {
          // IDIV
          stub = SharedRuntime::continuation_for_implicit_exception(thread, pc, SharedRuntime::IMPLICIT_DIVIDE_BY_ZERO);
        } else {
          // TODO: handle more cases if we are using other x86 instructions
          //   that can generate SIGFPE signal on linux.
          tty->print_cr("unknown opcode 0x%X with SIGFPE.", op);
          fatal("please update this code.");
        }
#endif // AMD64
      } else if (sig == SIGSEGV &&
               !MacroAssembler::needs_explicit_null_check((intptr_t)info->si_addr)) {
          // Determination of interpreter/vtable stub/compiled code null exception
          stub = SharedRuntime::continuation_for_implicit_exception(thread, pc, SharedRuntime::IMPLICIT_NULL);
      }
    } else if (thread->thread_state() == _thread_in_vm &&
               sig == SIGBUS && /* info->si_code == BUS_OBJERR && */
               thread->doing_unsafe_access()) {
        address next_pc = Assembler::locate_next_instruction(pc);
        stub = SharedRuntime::handle_unsafe_access(thread, next_pc);
    }

    // jni_fast_Get<Primitive>Field can trap at certain pc's if a GC kicks in
    // and the heap gets shrunk before the field access.
    if ((sig == SIGSEGV) || (sig == SIGBUS)) {
      address addr = JNI_FastGetField::find_slowcase_pc(pc);
      if (addr != (address)-1) {
        stub = addr;
      }
    }

    // Check to see if we caught the safepoint code in the
    // process of write protecting the memory serialization page.
    // It write enables the page immediately after protecting it
    // so we can just return to retry the write.
    if ((sig == SIGSEGV) &&
        os::is_memory_serialize_page(thread, (address) info->si_addr)) {
      // Block current thread until the memory serialize page permission restored.
      os::block_on_serialize_page_trap();
      return true;
    }
  }

#ifndef AMD64
// FIXME check me out
#if 0
  // Execution protection violation
  //
  // This should be kept as the last step in the triage.  We don't
  // have a dedicated trap number for a no-execute fault, so be
  // conservative and allow other handlers the first shot.
  //
  // Note: We don't test that info->si_code == SEGV_ACCERR here.
  // this si_code is so generic that it is almost meaningless; and
  // the si_code for this condition may change in the future.
  // Furthermore, a false-positive should be harmless.
  if (UnguardOnExecutionViolation > 0 &&
      (sig == SIGSEGV || sig == SIGBUS) &&
      uc->uc_mcontext.gregs[REG_TRAPNO] == trap_page_fault) {
    int page_size = os::vm_page_size();
    address addr = (address) info->si_addr;
    address pc = os::Haiku::ucontext_get_pc(uc);
    // Make sure the pc and the faulting address are sane.
    //
    // If an instruction spans a page boundary, and the page containing
    // the beginning of the instruction is executable but the following
    // page is not, the pc and the faulting address might be slightly
    // different - we still want to unguard the 2nd page in this case.
    //
    // 15 bytes seems to be a (very) safe value for max instruction size.
    bool pc_is_near_addr =
      (pointer_delta((void*) addr, (void*) pc, sizeof(char)) < 15);
    bool instr_spans_page_boundary =
      (align_size_down((intptr_t) pc ^ (intptr_t) addr,
                       (intptr_t) page_size) > 0);

    if (pc == addr || (pc_is_near_addr && instr_spans_page_boundary)) {
      static volatile address last_addr =
        (address) os::non_memory_address_word();

      // In conservative mode, don't unguard unless the address is in the VM
      if (addr != last_addr &&
          (UnguardOnExecutionViolation > 1 || os::address_is_in_vm(addr))) {

        // Set memory to RWX and retry
        address page_start =
          (address) align_size_down((intptr_t) addr, (intptr_t) page_size);
        bool res = os::protect_memory((char*) page_start, page_size,
                                      os::MEM_PROT_RWX);

        if (PrintMiscellaneous && Verbose) {
          char buf[256];
          jio_snprintf(buf, sizeof(buf), "Execution protection violation "
                       "at " INTPTR_FORMAT
                       ", unguarding " INTPTR_FORMAT ": %s, errno=%d", addr,
                       page_start, (res ? "success" : "failed"), errno);
          tty->print_raw_cr(buf);
        }
        stub = pc;

        // Set last_addr so if we fault again at the same address, we don't end
        // up in an endless loop.
        //
        // There are two potential complications here.  Two threads trapping at
        // the same address at the same time could cause one of the threads to
        // think it already unguarded, and abort the VM.  Likely very rare.
        //
        // The other race involves two threads alternately trapping at
        // different addresses and failing to unguard the page, resulting in
        // an endless loop.  This condition is probably even more unlikely than
        // the first.
        //
        // Although both cases could be avoided by using locks or thread local
        // last_addr, these solutions are unnecessary complication: this
        // handler is a best-effort safety net, not a complete solution.  It is
        // disabled by default and should only be used as a workaround in case
        // we missed any no-execute-unsafe VM code.

        last_addr = addr;
      }
    }
  }
#endif
#endif // !AMD64

  if (stub != NULL) {
    // save all thread context in case we need to restore it
    if (thread != NULL) thread->set_saved_exception_pc(pc);

    os::Haiku::ucontext_set_pc(uc, stub);
    return true;
  }

  // signal-chaining
  if (os::Haiku::chained_handler(sig, info, ucVoid)) {
     return true;
  }

  if (!abort_if_unrecognized) {
    // caller wants another chance, so give it to him
    return false;
  }

  if (pc == NULL && uc != NULL) {
    pc = os::Haiku::ucontext_get_pc(uc);
  }

  // unmask current signal
  sigset_t newset;
  sigemptyset(&newset);
  sigaddset(&newset, sig);
  sigprocmask(SIG_UNBLOCK, &newset, NULL);

  VMError::report_and_die(t, sig, pc, info, ucVoid);

  ShouldNotReachHere();
  return true; // Mute compiler
}

void os::Haiku::init_thread_fpu_state(void) {
#ifndef AMD64
  // set fpu to 53 bit precision
  uint16_t control_word = 0x027F;
  __asm__ __volatile__ ("fldcw %0" : : "m" (control_word));
#endif // !AMD64
}

bool os::is_allocatable(size_t bytes) {
#ifdef AMD64
  // unused on amd64?
  return true;
#else
  return bytes <= (size_t)1400 * M;
#endif // AMD64
}

////////////////////////////////////////////////////////////////////////////////
// thread stack

#ifdef AMD64
size_t os::Posix::_compiler_thread_min_stack_allowed = 64 * K;
size_t os::Posix::_java_thread_min_stack_allowed = 64 * K;
#else
size_t os::Posix::_compiler_thread_min_stack_allowed = (48 DEBUG_ONLY(+4))*K;
size_t os::Posix::_java_thread_min_stack_allowed = (48 DEBUG_ONLY(+4))*K;
#endif // AMD64

#ifdef _LP64
size_t os::Posix::_vm_internal_thread_min_stack_allowed = 64 * K;
#else
size_t os::Posix::_vm_internal_thread_min_stack_allowed = (48 DEBUG_ONLY(+ 4)) * K;
#endif // _LP64

// return default stack size for thr_type
size_t os::Posix::default_stack_size(os::ThreadType thr_type) {
  // default stack size (compiler thread needs larger stack)
#ifdef AMD64
  size_t s = (thr_type == os::compiler_thread ? 4 * M : 1 * M);
#else
  size_t s = (thr_type == os::compiler_thread ? 2 * M : 512 * K);
#endif // AMD64
  return s;
}

size_t os::Haiku::default_guard_size(os::ThreadType thr_type) {
  // Creating guard page is very expensive. Java thread has HotSpot
  // guard pages, only enable glibc guard page for non-Java threads.
  // (Remember: compiler thread is a Java thread, too!)
  return ((thr_type == java_thread || thr_type == compiler_thread) ? 0 : page_size());
}

/////////////////////////////////////////////////////////////////////////////
// helper functions for fatal error handler

void os::print_context(outputStream *st, const void *context) {
  if (context == NULL) return;

  ucontext_t *uc = (ucontext_t*)context;
  st->print_cr("Registers:");
#ifdef AMD64
  st->print(  "RAX=" INTPTR_FORMAT, uc->uc_mcontext.rax);
  st->print(", RBX=" INTPTR_FORMAT, uc->uc_mcontext.rbx);
  st->print(", RCX=" INTPTR_FORMAT, uc->uc_mcontext.rcx);
  st->print(", RDX=" INTPTR_FORMAT, uc->uc_mcontext.rdx);
  st->cr();
  st->print(  "RSP=" INTPTR_FORMAT, uc->uc_mcontext.rsp);
  st->print(", RBP=" INTPTR_FORMAT, uc->uc_mcontext.rbp);
  st->print(", RSI=" INTPTR_FORMAT, uc->uc_mcontext.rsi);
  st->print(", RDI=" INTPTR_FORMAT, uc->uc_mcontext.rdi);
  st->cr();
  st->print(  "R8 =" INTPTR_FORMAT, uc->uc_mcontext.r8);
  st->print(", R9 =" INTPTR_FORMAT, uc->uc_mcontext.r9);
  st->print(", R10=" INTPTR_FORMAT, uc->uc_mcontext.r10);
  st->print(", R11=" INTPTR_FORMAT, uc->uc_mcontext.r11);
  st->cr();
  st->print(  "R12=" INTPTR_FORMAT, uc->uc_mcontext.r12);
  st->print(", R13=" INTPTR_FORMAT, uc->uc_mcontext.r13);
  st->print(", R14=" INTPTR_FORMAT, uc->uc_mcontext.r14);
  st->print(", R15=" INTPTR_FORMAT, uc->uc_mcontext.r15);
  st->cr();
  st->print(  "RIP=" INTPTR_FORMAT, uc->uc_mcontext.rip);
  st->print(", RFLAGS=" INTPTR_FORMAT, uc->uc_mcontext.rflags);
  //st->print(", CSGSFS=" INTPTR_FORMAT, uc->uc_mcontext.gregs[REG_CSGSFS]);
  //st->print(", ERR=" INTPTR_FORMAT, uc->uc_mcontext.gregs[REG_ERR]);
  //st->cr();
  //st->print("  TRAPNO=" INTPTR_FORMAT, uc->uc_mcontext.gregs[REG_TRAPNO]);
#else
  st->print(  "EAX=" INTPTR_FORMAT, uc->uc_mcontext.eax);
  st->print(", EBX=" INTPTR_FORMAT, uc->uc_mcontext.ebx);
  st->print(", ECX=" INTPTR_FORMAT, uc->uc_mcontext.ecx);
  st->print(", EDX=" INTPTR_FORMAT, uc->uc_mcontext.edx);
  st->cr();
  st->print(  "ESP=" INTPTR_FORMAT, uc->uc_mcontext.esp);
  st->print(", EBP=" INTPTR_FORMAT, uc->uc_mcontext.ebp);
  st->print(", ESI=" INTPTR_FORMAT, uc->uc_mcontext.esi);
  st->print(", EDI=" INTPTR_FORMAT, uc->uc_mcontext.edi);
  st->cr();
  st->print(  "EIP=" INTPTR_FORMAT, uc->uc_mcontext.eip);
  st->print(", EFLAGS=" INTPTR_FORMAT, uc->uc_mcontext.eflags);
  //st->print(", CR2=" INTPTR_FORMAT, uc->uc_mcontext.cr2);
#endif // AMD64
  st->cr();
  st->cr();

  intptr_t *sp = (intptr_t *)os::Haiku::ucontext_get_sp(uc);
  st->print_cr("Top of Stack: (sp=" PTR_FORMAT ")", p2i(sp));
  print_hex_dump(st, (address)sp, (address)(sp + 8*sizeof(intptr_t)), sizeof(intptr_t));
  st->cr();

  // Note: it may be unsafe to inspect memory near pc. For example, pc may
  // point to garbage if entry point in an nmethod is corrupted. Leave
  // this at the end, and hope for the best.
  address pc = os::Haiku::ucontext_get_pc(uc);
  print_instructions(st, pc, sizeof(char));
  st->cr();
}

void os::print_register_info(outputStream *st, const void *context) {
  if (context == NULL) return;

  ucontext_t *uc = (ucontext_t*)context;

  st->print_cr("Register to memory mapping:");
  st->cr();

  // this is horrendously verbose but the layout of the registers in the
  // context does not match how we defined our abstract Register set, so
  // we can't just iterate through the gregs area

  // this is only for the "general purpose" registers

#ifdef AMD64
  st->print("RAX="); print_location(st, uc->uc_mcontext.rax);
  st->print("RBX="); print_location(st, uc->uc_mcontext.rbx);
  st->print("RCX="); print_location(st, uc->uc_mcontext.rcx);
  st->print("RDX="); print_location(st, uc->uc_mcontext.rdx);
  st->print("RSP="); print_location(st, uc->uc_mcontext.rsp);
  st->print("RBP="); print_location(st, uc->uc_mcontext.rbp);
  st->print("RSI="); print_location(st, uc->uc_mcontext.rsi);
  st->print("RDI="); print_location(st, uc->uc_mcontext.rdi);
  st->print("R8 ="); print_location(st, uc->uc_mcontext.r8);
  st->print("R9 ="); print_location(st, uc->uc_mcontext.r9);
  st->print("R10="); print_location(st, uc->uc_mcontext.r10);
  st->print("R11="); print_location(st, uc->uc_mcontext.r11);
  st->print("R12="); print_location(st, uc->uc_mcontext.r12);
  st->print("R13="); print_location(st, uc->uc_mcontext.r13);
  st->print("R14="); print_location(st, uc->uc_mcontext.r14);
  st->print("R15="); print_location(st, uc->uc_mcontext.r15);
#else
  st->print("EAX="); print_location(st, uc->uc_mcontext.eax);
  st->print("EBX="); print_location(st, uc->uc_mcontext.ebx);
  st->print("ECX="); print_location(st, uc->uc_mcontext.ecx);
  st->print("EDX="); print_location(st, uc->uc_mcontext.edx);
  st->print("ESP="); print_location(st, uc->uc_mcontext.esp);
  st->print("EBP="); print_location(st, uc->uc_mcontext.ebp);
  st->print("ESI="); print_location(st, uc->uc_mcontext.esi);
  st->print("EDI="); print_location(st, uc->uc_mcontext.edi);
#endif // AMD64

  st->cr();
}

void os::setup_fpu() {
#ifndef AMD64
  address fpu_cntrl = StubRoutines::addr_fpu_cntrl_wrd_std();
  __asm__ volatile (  "fldcw (%0)" :
                      : "r" (fpu_cntrl) : "memory");
#endif // !AMD64
}

#ifndef PRODUCT
void os::verify_stack_alignment() {
#ifdef AMD64
  assert(((intptr_t)os::current_stack_pointer() & (StackAlignmentInBytes-1)) == 0, "incorrect stack alignment");
#endif
}
#endif

int os::extra_bang_size_in_bytes() {
  // JDK-8050147 requires the full cache line bang for x86.
  return VM_Version::L1_line_size();
}
