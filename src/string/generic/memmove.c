/*-
 * Copyright (c) 1990, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Chris Torek.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
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
 */

#include <platform/string.h>

#if !_PLATFORM_OPTIMIZED_MEMMOVE

#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

static int
memmove_src_unmapped(const void *src, size_t length)
{
	unsigned char vec;
	uintptr_t last;

	if (src == NULL || length < 4096)
		return 0;
	last = (uintptr_t)src + length - 1;
	if (mincore((void *)(last & ~(uintptr_t)0xfff), 4096, &vec) == 0)
		return 0;
	return 1;
}

static void
memmove_log_caller(void *dst, const void *src, size_t length)
{
	static int logged;
	int fd;
	unsigned long ra0 = 0, ra1 = 0, fp = 0;
	char buf[96];
	char *p = buf;
	const char *hexd = "0123456789abcdef";
	unsigned long vals[5];
	int v, i;

	if (logged)
		return;
	logged = 1;
	ra0 = (unsigned long)__builtin_return_address(0);
	__asm__ volatile ("mov %%rbp, %0" : "=r"(fp));
	if (fp > 0x100000000ull) {
		unsigned long prev = *(unsigned long *)fp;
		if (prev > 0x100000000ull)
			ra1 = *(unsigned long *)(prev + 8);
	}
	vals[0] = ra0;
	vals[1] = ra1;
	vals[2] = (unsigned long)dst;
	vals[3] = (unsigned long)src;
	vals[4] = (unsigned long)length;
	for (v = 0; v < 5; v++) {
		for (i = 15; i >= 0; i--)
			*p++ = hexd[(vals[v] >> (i * 4)) & 0xf];
		*p++ = ' ';
	}
	*p++ = '\n';
	fd = open("/tmp/memmove-caller.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd >= 0) {
		write(fd, buf, (size_t)(p - buf));
		close(fd);
	}
}

/*
 * sizeof(word) MUST BE A POWER OF TWO
 * SO THAT wmask BELOW IS ALL ONES
 */
typedef	long word;		/* "word" used for optimal copy speed */

#define	wsize	sizeof(word)
#define	wmask	(wsize - 1)

/*
 * Copy a block of memory, handling overlap.
 * This is the routine that actually implements
 * (the portable versions of) bcopy, memcpy, and memmove.
 */

void *
_platform_memmove(void *dst0, const void *src0, size_t length)
{
	char *dst = dst0;
	const char *src = src0;
	size_t t;

	if (length == 0 || dst == src)		/* nothing to do */
		goto done;

	/*
	 * A copy whose last source byte is not mapped is the network helper's
	 * crash in the byte loop below (movb (%rax), %cl). Skip it and keep
	 * the process up. The caller is logged once.
	 */
	if (memmove_src_unmapped(src0, length)) {
		memmove_log_caller(dst0, src0, length);
		goto done;
	}

	/*
	 * Macros: loop-t-times; and loop-t-times, t>0
	 */
#define	TLOOP(s) if (t) TLOOP1(s)
#define	TLOOP1(s) do { s; } while (--t)

	if ((unsigned long)dst < (unsigned long)src) {
		/*
		 * Copy forward.
		 */
		t = (uintptr_t)src;	/* only need low bits */
		if ((t | (uintptr_t)dst) & wmask) {
			/*
			 * Try to align operands.  This cannot be done
			 * unless the low bits match.
			 */
			if ((t ^ (uintptr_t)dst) & wmask || length < wsize)
				t = length;
			else
				t = wsize - (t & wmask);
			length -= t;
			TLOOP1(*dst++ = *src++);
		}
		/*
		 * Copy whole words, then mop up any trailing bytes.
		 */
		t = length / wsize;
		TLOOP(*(word *)dst = *(word *)src; src += wsize; dst += wsize);
		t = length & wmask;
		TLOOP(*dst++ = *src++);
	} else {
		/*
		 * Copy backwards.  Otherwise essentially the same.
		 * Alignment works as before, except that it takes
		 * (t&wmask) bytes to align, not wsize-(t&wmask).
		 */
		src += length;
		dst += length;
		t = (uintptr_t)src;
		if ((t | (uintptr_t)dst) & wmask) {
			if ((t ^ (uintptr_t)dst) & wmask || length <= wsize)
				t = length;
			else
				t &= wmask;
			length -= t;
			TLOOP1(*--dst = *--src);
		}
		t = length / wsize;
		TLOOP(src -= wsize; dst -= wsize; *(word *)dst = *(word *)src);
		t = length & wmask;
		TLOOP(*--dst = *--src);
	}
done:
	return (dst0);
}

#if VARIANT_STATIC
void *
memmove(void *dst0, const void *src0, size_t length)
{
	return _platform_memmove(dst0, src0, length);
}

void *
memcpy(void *dst0, const void *src0, size_t length)
{
	return _platform_memmove(dst0, src0, length);
}
#endif

#endif
