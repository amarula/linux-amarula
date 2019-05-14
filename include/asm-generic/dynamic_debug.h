/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GENERIC_DYNAMIC_DEBUG_H
#define _ASM_GENERIC_DYNAMIC_DEBUG_H

#ifndef _DYNAMIC_DEBUG_H
#error "don't include asm/dynamic_debug.h directly"
#endif

#include <linux/build_bug.h>
#ifdef CONFIG_JUMP_LABEL
#include <linux/jump_label.h>
#endif

/*
 * We need to know the exact layout of struct _ddebug in order to
 * initialize it in assembly. Check that all members are at expected
 * offsets - if any of these fail, the arch cannot use this generic
 * dynamic_debug.h. DYNAMIC_DEBUG_RELATIVE_POINTERS is pointless for
 * !64BIT, so we expect the static_key to be at an 8-byte boundary
 * since it contains stuff which is long-aligned.
 */

static_assert(BITS_PER_LONG == 64);
static_assert(offsetof(struct _ddebug, modname_disp)  == 0);
static_assert(offsetof(struct _ddebug, function_disp) == 4);
static_assert(offsetof(struct _ddebug, filename_disp) == 8);
static_assert(offsetof(struct _ddebug, format_disp)   == 12);
static_assert(offsetof(struct _ddebug, flags_lineno)  == 16);

#ifdef CONFIG_JUMP_LABEL
static_assert(offsetof(struct _ddebug, key)        == 24);
static_assert(offsetof(struct static_key, enabled) == 0);
static_assert(offsetof(struct static_key, type)    == 8);

/* <4 bytes padding>, .enabled, <4 bytes padding>, .type */
# ifdef DEBUG
# define _DPRINTK_ASM_KEY_INIT \
	"\t.int 0\n" "\t.int 1\n" "\t.int 0\n" "\t.quad "__stringify(__JUMP_TYPE_TRUE)"\n"
# else
# define _DPRINTK_ASM_KEY_INIT \
	"\t.int 0\n" "\t.int 0\n" "\t.int 0\n" "\t.quad "__stringify(__JUMP_TYPE_FALSE)"\n"
# endif
#else /* !CONFIG_JUMP_LABEL */
#define _DPRINTK_ASM_KEY_INIT ""
#endif

/*
 * There's a bit of magic involved here.
 *
 * First, unlike the bug table entries, we need to define an object in
 * assembly which we can reference from C code (for use by the
 * DYNAMIC_DEBUG_BRANCH macro), but we don't want 'name' to have
 * external linkage (that would require use of globally unique
 * identifiers, which we can't guarantee). Fortunately, the "extern"
 * keyword just tells the compiler that _somebody_ provides that
 * symbol - usually that somebody is the linker, but in this case it's
 * the assembler, and since we do not do .globl name, the symbol gets
 * internal linkage.
 *
 * So far so good. The next problem is that there's no scope in
 * assembly, so the identifier 'name' has to be unique within each
 * translation unit - otherwise all uses of that identifier end up
 * referring to the same struct _ddebug instance. pr_debug and friends
 * do this by use of indirection and __UNIQUE_ID(), and new users of
 * DEFINE_DYNAMIC_DEBUG_METADATA() should do something similar. We
 * need to catch cases where this is not done at build time.
 *
 * With assembly-level .ifndef we can ensure that we only define a
 * given identifier once, preventing "symbol 'foo' already defined"
 * errors. But we still need to detect and fail on multiple uses of
 * the same identifer. The simplest, and wrong, solution to that is to
 * add an .else .error branch to the .ifndef. The problem is that just
 * because the DEFINE_DYNAMIC_DEBUG_METADATA() macro is only expanded
 * once with a given identifier, the compiler may emit the assembly
 * code multiple times, e.g. if the macro appears in an inline
 * function. Now, in a normal case like
 *
 *   static inline get_next_id(void) { static int v; return ++v; }
 *
 * all inlined copies of get_next_id are _supposed_ to refer to the
 * same object 'v'. So we do need to allow this chunk of assembly to
 * appear multiple times with the same 'name', as long as they all
 * came from the same DEFINE_DYNAMIC_DEBUG_METADATA() instance. To do
 * that, we pass __COUNTER__ to the asm(), and set an assembler symbol
 * name.ddebug.once to that value when we first define 'name'. When we
 * meet a second attempt at defining 'name', we compare
 * name.ddebug.once to %6 and error out if they are different.
 */

#define DEFINE_DYNAMIC_DEBUG_METADATA(name, fmt)			\
	extern struct _ddebug name;					\
	asm volatile(".ifndef " __stringify(name) "\n"			\
		     ".pushsection __verbose,\"aw\"\n"			\
		     ".type "__stringify(name)", STT_OBJECT\n"		\
		     ".size "__stringify(name)", %c5\n"			\
		     "1:\n"						\
		     __stringify(name) ":\n"				\
		     "\t.int %c0 - 1b /* _ddebug::modname_disp */\n"	\
		     "\t.int %c1 - 1b /* _ddebug::function_disp */\n"	\
		     "\t.int %c2 - 1b /* _ddebug::filename_disp */\n"	\
		     "\t.int %c3 - 1b /* _ddebug::format_disp */\n"	\
		     "\t.int %c4      /* _ddebug::flags_lineno */\n"	\
		     _DPRINTK_ASM_KEY_INIT				\
		     "\t.org 1b+%c5\n"					\
		     ".popsection\n"					\
		     ".set "__stringify(name)".ddebug.once, %c6\n"	\
		     ".elseif "__stringify(name)".ddebug.once - %c6\n"	\
		     ".line "__stringify(__LINE__) " - 1\n"             \
		     ".error \"'"__stringify(name)"' used as _ddebug identifier more than once\"\n" \
		     ".endif\n"						\
		     : : "i" (KBUILD_MODNAME), "i" (__func__),		\
		       "i" (__FILE__), "i" (fmt),			\
		       "i" (_DPRINTK_FLAGS_LINENO_INIT),		\
		       "i" (sizeof(struct _ddebug)), "i" (__COUNTER__))

#endif /* _ASM_GENERIC_DYNAMIC_DEBUG_H */
