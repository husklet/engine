#ifndef HL_LINUX_ABI_PAGE_H
#define HL_LINUX_ABI_PAGE_H

/*
 * Linux guest ABI page size.  This is deliberately independent of the host
 * VM allocation granularity: Apple Silicon hosts map in 16 KiB units, while
 * the Linux ABI presented by both supported guest ISAs uses 4 KiB pages.
 * Host-facing mmap reconciliation must continue to use getpagesize().
 */
#define HL_LINUX_GUEST_PAGE_SIZE 4096u

#endif
