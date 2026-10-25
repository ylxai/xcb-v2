/* Implementasi insecure_memzero (secure erasure) untuk external/yespower.
 * Upstream meng-generate header ini lewat Makefile; versi kosong bawaan
 * cukup untuk kebenaran, tapi ini versi riil: tulis nol lewat pointer
 * volatile sehingga compiler tidak boleh mengeliminasinya. */
#ifndef INSECURE_MEMZERO_H_
#define INSECURE_MEMZERO_H_

#include <stddef.h>

static inline void insecure_memzero_impl(void *buf, size_t len)
{
	volatile unsigned char *p = (volatile unsigned char *)buf;
	while (len--)
		*p++ = 0;
}

#define insecure_memzero(buf, len) insecure_memzero_impl((buf), (len))

#endif /* INSECURE_MEMZERO_H_ */
