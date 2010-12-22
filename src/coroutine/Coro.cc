/*
 Credits

	Originally based on Edgar Toernig's Minimalistic cooperative multitasking
	http://www.goron.de/~froese/
	reorg by Steve Dekorte and Chis Double
	Symbian and Cygwin support by Chis Double
	Linux/PCC, Linux/Opteron, Irix and FreeBSD/Alpha, ucontext support by Austin Kurahone
	FreeBSD/Intel support by Faried Nawaz
	Mingw support by Pit Capitain
	Visual C support by Daniel Vollmer
	Solaris support by Manpreet Singh
	Fibers support by Jonas Eschenburg
	Ucontext arg support by Olivier Ansaldi
	Ucontext x86-64 support by James Burgess and Jonathan Wright
	Russ Cox for the newer portable ucontext implementions.

 Notes

	This is the system dependent coro code.
	Setup a jmp_buf so when we longjmp, it will invoke 'func' using 'stack'.
	Important: 'func' must not return!

	Usually done by setting the program counter and stack pointer of a new, empty stack.
	If you're adding a new platform, look in the setjmp.h for PC and SP members
	of the stack structure

	If you don't see those members, Kentaro suggests writting a simple
	test app that calls setjmp and dumps out the contents of the jmp_buf.
	(The PC and SP should be in jmp_buf->__jmpbuf).

	Using something like GDB to be able to peek into register contents right
	before the setjmp occurs would be helpful also.
 */

//#include "Base.h"
#include "Coro.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <ucontext.h>
#include "ucontext_i.h"
#include "arch/arch.hpp"

typedef char coro_stack_t[16*1024];

const int max_threads = 50;
const int stacks_per_thread = 10000;

struct alloc_state_t {
    //For real, this would be a free list
    coro_stack_t *base;
    coro_stack_t *current;
} allocators[max_threads]; //Hopefully never more than 50 threads

coro_stack_t *get_stack() {
    alloc_state_t *allocator = &allocators[get_thread_id()];
    //printf("getting stack #%zd on thread %d\n", allocator->current - allocator->base, get_thread_id());
    if (allocator->base + stacks_per_thread <= allocator->current) allocator->current = allocator->base;
    return allocator->current++;
}

void initialize_stacks() {
    allocators[get_thread_id()].base = allocators[get_thread_id()].current = (coro_stack_t *)calloc(1, sizeof(coro_stack_t)*stacks_per_thread);
}

void destroy_stacks() {
    free(allocators[get_thread_id()].base);
}

#ifdef USE_VALGRIND
#include <valgrind/valgrind.h>
#define STACK_REGISTER(coro) \
{ \
	Coro *c = (coro); \
		c->valgrindStackId = VALGRIND_STACK_REGISTER( \
											 c->stack, \
											 (size_t)c->stack + c->requestedStackSize); \
}

#define STACK_DEREGISTER(coro) \
VALGRIND_STACK_DEREGISTER((coro)->valgrindStackId)

#else
#define STACK_REGISTER(coro)
#define STACK_DEREGISTER(coro)
#endif

extern "C" {
	extern int lightweight_getcontext(ucontext_t *ucp);
	extern int lightweight_swapcontext(ucontext_t *oucp, const ucontext_t *ucp);
}

typedef struct CallbackBlock
{
	void *context;
	CoroStartCallback *func;
} CallbackBlock;

#if !defined(USE_UCONTEXT) || !defined(__x86_64__)
//Must be in a conditional, since unused outisde of this case and we take warnings as errors
static CallbackBlock globalCallbackBlock;
#endif

Coro *Coro_new(void)
{
	Coro *self = (Coro *)calloc(1, sizeof(Coro));
	self->requestedStackSize = CORO_DEFAULT_STACK_SIZE;
	self->allocatedStackSize = 0;

#ifdef USE_FIBERS
	self->fiber = NULL;
#else
	self->stack = NULL;
#endif
	return self;
}

void Coro_allocStack(Coro *self)
{
	self->stack = (void *)get_stack();
	self->allocatedStackSize = self->requestedStackSize;
	STACK_REGISTER(self);
}

void Coro_free(Coro *self)
{
#ifdef USE_FIBERS
	// If this coro has a fiber, delete it.
	// Don't delete the main fiber. We don't want to commit suicide.
	if (self->fiber && !self->isMain)
	{
		DeleteFiber(self->fiber);
	}
#else
	STACK_DEREGISTER(self);
#endif
	/*if (self->stack)
	{
		free(self->stack);
	}*/

	//printf("Coro_%p io_free\n", (void *)self);

	if (self->isMain) destroy_stacks();
	free(self);
}

// stack

void *Coro_stack(Coro *self)
{
	return self->stack;
}

size_t Coro_stackSize(Coro *self)
{
	return self->requestedStackSize;
}

void Coro_setStackSize_(Coro *self, size_t sizeInBytes)
{
	self->requestedStackSize = sizeInBytes;
	//self->stack = (void *)io_realloc(self->stack, sizeInBytes);
	//printf("Coro_%p io_reallocating stack size %i\n", (void *)self, sizeInBytes);
}

#if __GNUC__ == 4
uint8_t *Coro_CurrentStackPointer(void) __attribute__ ((noinline));
#endif

uint8_t *Coro_CurrentStackPointer(void)
{
	uint8_t a;
	uint8_t *b = &a; // to avoid compiler warning about unused variables
	return b;
}

size_t Coro_bytesLeftOnStack(Coro *self)
{
	unsigned char dummy;
	ptrdiff_t p1 = (ptrdiff_t)(&dummy);
	ptrdiff_t p2 = (ptrdiff_t)Coro_CurrentStackPointer();
	int stackMovesUp = p2 > p1;
	ptrdiff_t start = ((ptrdiff_t)self->stack);
	ptrdiff_t end   = start + self->requestedStackSize;

	if (stackMovesUp) // like x86
	{
		return end - p1;
	}
	else // like OSX on PPC
	{
		return p1 - start;
	}
}

int Coro_stackSpaceAlmostGone(Coro *self)
{
	return Coro_bytesLeftOnStack(self) < CORO_STACK_SIZE_MIN;
}

void Coro_initializeMainCoro(Coro *self)
{
	self->isMain = 1;
	initialize_stacks();
#ifdef USE_FIBERS
	// We must convert the current thread into a fiber if it hasn't already been done.
	if ((LPVOID) 0x1e00 == GetCurrentFiber()) // value returned when not a fiber
	{
		// Make this thread a fiber and set its data field to the main coro's address
		ConvertThreadToFiber(self);
	}
	// Make the main coro represent the current fiber
	self->fiber = GetCurrentFiber();
#endif
}

void Coro_startCoro_(Coro *self, Coro *other, void *context, CoroStartCallback *callback)
{
	CallbackBlock sblock;
	CallbackBlock *block = &sblock;
	//CallbackBlock *block = malloc(sizeof(CallbackBlock)); // memory leak
	block->context = context;
	block->func    = callback;
	
	Coro_allocStack(other);
	Coro_setup(other, block);
	Coro_switchTo_(self, other);
}


#if defined(USE_UCONTEXT) && defined(__x86_64__)
void Coro_StartWithArg(unsigned int hiArg, unsigned int loArg)
{
	CallbackBlock *block = (CallbackBlock*)(((long long)hiArg << 32) | (long long)loArg);
	(block->func)(block->context);
	printf("Scheduler error: returned from coro start function\n");
	exit(-1);
}

#else
void Coro_StartWithArg(CallbackBlock *block)
{
	(block->func)(block->context);
	printf("Scheduler error: returned from coro start function\n");
	exit(-1);
}


void Coro_Start(void)
{
	CallbackBlock block = globalCallbackBlock;
	Coro_StartWithArg(&block);
}
 
#endif

// --------------------------------------------------------------------

void Coro_UnsupportedPlatformError(void)
{
	printf("Io Scheduler error: no Coro_setupJmpbuf entry for this platform\n.");
	exit(1);
}


void Coro_switchTo_(Coro *self, Coro *next)
{
#if defined(__SYMBIAN32__)
	ProcessUIEvent();
#elif defined(USE_FIBERS)
	SwitchToFiber(next->fiber);
#elif defined(USE_UCONTEXT)
	lightweight_swapcontext(&self->env, &next->env);
#elif defined(USE_SETJMP)
	if (setjmp(self->env) == 0)
	{
		longjmp(next->env, 1);
	}
#endif
}

// ---- setup ------------------------------------------

#if defined(USE_SETJMP) && defined(__x86_64__)

void Coro_setup(Coro *self, void *arg)
{
	/* since ucontext seems to be broken on amg64 */

	setjmp(self->env);
	/* This is probably not nice in that it deals directly with
	* something with __ in front of it.
	*
	* Anyhow, Coro.h makes the member env of a struct Coro a
	* jmp_buf. A jmp_buf, as defined in the amd64 setjmp.h
	* is an array of one struct that wraps the actual __jmp_buf type
	* which is the array of longs (on a 64 bit machine) that
	* the programmer below expected. This struct begins with
	* the __jmp_buf array of longs, so I think it was supposed
	* to work like he originally had it, but for some reason
	* it didn't. I don't know why.
	* - Bryce Schroeder, 16 December 2006
	*
	*   Explaination of `magic' numbers: 6 is the stack pointer
	*   (RSP, the 64 bit equivalent of ESP), 7 is the program counter.
	*   This information came from this file on my Gentoo linux
	*   amd64 computer:
	*   /usr/include/gento-multilib/amd64/bits/setjmp.h
	*   Which was ultimatly included from setjmp.h in /usr/include. */
	self->env[0].__jmpbuf[6] = ((unsigned long)(Coro_stack(self)));
	self->env[0].__jmpbuf[7] = ((long)Coro_Start);
}

#elif defined(HAS_UCONTEXT_ON_PRE_SOLARIS_10)

typedef void (*makecontext_func)(void);

void Coro_setup(Coro *self, void *arg)
{
	ucontext_t *ucp = (ucontext_t *) &self->env;

	lightweight_getcontext(ucp);

	ucp->uc_stack.ss_sp    = Coro_stack(self) + Coro_stackSize(self) - 8;
	ucp->uc_stack.ss_size  = Coro_stackSize(self);
	ucp->uc_stack.ss_flags = 0;
	ucp->uc_link = NULL;

	makecontext(ucp, (makecontext_func)Coro_StartWithArg, 1, arg); }


#elif defined(USE_UCONTEXT)

typedef void (*makecontext_func)(void);

void Coro_setup(Coro *self, void *arg)
{
	ucontext_t *ucp = (ucontext_t *) &self->env;

	lightweight_getcontext(ucp);

	ucp->uc_stack.ss_sp    = Coro_stack(self);
	ucp->uc_stack.ss_size  = Coro_stackSize(self);
#if !defined(__APPLE__)
	ucp->uc_stack.ss_flags = 0;
	ucp->uc_link = NULL;
#endif

#if defined(__x86_64__)
	unsigned int hiArg = (unsigned int)((long long)arg >> 32);
	unsigned int loArg = (unsigned int)((long long)arg & 0xFFFFFFFF);
	makecontext(ucp, (makecontext_func)Coro_StartWithArg, 2, hiArg, loArg);
#else
	makecontext(ucp, (makecontext_func)Coro_StartWithArg, 1, arg);
#endif
}

#elif defined(USE_FIBERS)

void Coro_setup(Coro *self, void *arg)
{
	// If this coro was recycled and already has a fiber, delete it.
	// Don't delete the main fiber. We don't want to commit suicide.

	if (self->fiber && !self->isMain)
	{
		DeleteFiber(self->fiber);
	}

	self->fiber = CreateFiber(Coro_stackSize(self),
							  (LPFIBER_START_ROUTINE)Coro_StartWithArg,
							 (LPVOID)arg);
	if (!self->fiber) {
		DWORD err = GetLastError();
		exit(err);
	}
}

#elif defined(__CYGWIN__)

#define buf (self->env)

void Coro_setup(Coro *self, void *arg)
{
	setjmp(buf);
	buf[7] = (long)(Coro_stack(self) + Coro_stackSize(self) - 16);
	buf[8] = (long)Coro_Start;
	globalCallbackBlock.context=((CallbackBlock*)arg)->context;
	globalCallbackBlock.func=((CallbackBlock*)arg)->func;
}

#elif defined(__SYMBIAN32__)

void Coro_setup(Coro *self, void *arg)
{
	/*
	setjmp/longjmp is flakey under Symbian.
	If the setjmp is done inside the call then a crash occurs.
	Inlining it here solves the problem
	*/

	setjmp(self->env);
	self->env[0] = 0;
	self->env[1] = 0;
	self->env[2] = 0;
	self->env[3] = (unsigned long)(Coro_stack(self))
		+ Coro_stackSize(self) - 64;
	self->env[9] = (long)Coro_Start;
	self->env[8] =  self->env[3] + 32;
}

#elif defined(_BSD_PPC_SETJMP_H_)

#define buf (self->env)
#define setjmp  _setjmp
#define longjmp _longjmp

void Coro_setup(Coro *self, void *arg)
{
	size_t *sp = (size_t *)(((intptr_t)Coro_stack(self)
						+ Coro_stackSize(self) - 64 + 15) & ~15);

	setjmp(buf);

	//printf("self = %p\n", self);
	//printf("sp = %p\n", sp);
	buf[0]  = (long)sp;
	buf[21] = (long)Coro_Start;
	globalCallbackBlock.context=((CallbackBlock*)arg)->context;
	globalCallbackBlock.func=((CallbackBlock*)arg)->func;
	//sp[-4] = (size_t)self; // for G5 10.3
	//sp[-6] = (size_t)self; // for G4 10.4

	//printf("self = %p\n", (void *)self);
	//printf("sp = %p\n", sp);
}

#elif defined(__DragonFly__)

#define buf (self->env)

void Coro_setup(Coro *self, void *arg)
{
	void *stack = Coro_stack(self);
	size_t stacksize = Coro_stackSize(self);
	void *func = (void *)Coro_Start;

	setjmp(buf);

	buf->_jb[2] = (long)(stack + stacksize);
	buf->_jb[0] = (long)func;
	return;
}

#elif defined(__arm__)
// contributed by Peter van Hardenberg

#define buf (self->env)

void Coro_setup(Coro *self, void *arg)
{
	setjmp(buf);
	buf[8] = (int)Coro_stack(self) + (int)Coro_stackSize(self) - 16;
	buf[9] = (int)Coro_Start;
}

#else

#error "Coro.c Error: Coro_setup() function needs to be defined for this platform."

#endif
