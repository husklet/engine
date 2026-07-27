#include "test.h"

#include "../../src/linux_abi/errno.h"

int main(void) {
    HL_CHECK(hl_linux_errno_from_macos(0) == 0);
    HL_CHECK(hl_linux_errno_from_macos(1) == 1);
#if defined(__linux__)
    HL_CHECK(hl_linux_errno_from_macos(11) == 11);
    HL_CHECK(hl_linux_errno_from_macos(35) == 35);
    HL_CHECK(hl_linux_errno_from_macos(62) == 62);
    HL_CHECK(hl_linux_errno_from_macos(4095) == 4095);
#elif defined(_WIN32)
    /* The refusal this whole arm exists for: UCRT ENOSYS 40 -> Linux ENOSYS 38.
     * Before the Windows table it went through the Darwin one and came out 90,
     * EMSGSIZE. */
    HL_CHECK(hl_linux_errno_from_macos(40) == 38);
    /* The classic block, including the four numbers whose UCRT value differs
     * from Linux's: EDEADLK 36->35, ENAMETOOLONG 38->36, ENOLCK 39->37,
     * ENOTEMPTY 41->39. */
    HL_CHECK(hl_linux_errno_from_macos(11) == 11); /* EAGAIN, same number */
    HL_CHECK(hl_linux_errno_from_macos(36) == 35); /* EDEADLK */
    HL_CHECK(hl_linux_errno_from_macos(38) == 36); /* ENAMETOOLONG */
    HL_CHECK(hl_linux_errno_from_macos(39) == 37); /* ENOLCK */
    HL_CHECK(hl_linux_errno_from_macos(41) == 39); /* ENOTEMPTY */
    HL_CHECK(hl_linux_errno_from_macos(42) == 84); /* EILSEQ */
    /* The 100..140 POSIX-2008 block. */
    HL_CHECK(hl_linux_errno_from_macos(100) == 98);  /* EADDRINUSE */
    HL_CHECK(hl_linux_errno_from_macos(114) == 40);  /* ELOOP */
    HL_CHECK(hl_linux_errno_from_macos(115) == 90);  /* EMSGSIZE */
    HL_CHECK(hl_linux_errno_from_macos(129) == 95);  /* ENOTSUP */
    HL_CHECK(hl_linux_errno_from_macos(130) == 95);  /* EOPNOTSUPP, same as above */
    HL_CHECK(hl_linux_errno_from_macos(132) == 75);  /* EOVERFLOW */
    HL_CHECK(hl_linux_errno_from_macos(139) == 26);  /* ETXTBSY */
    HL_CHECK(hl_linux_errno_from_macos(140) == 11);  /* EWOULDBLOCK == EAGAIN */
    /* Unassigned UCRT slots collapse to EINVAL; non-errno values pass through. */
    HL_CHECK(hl_linux_errno_from_macos(15) == 22);
    HL_CHECK(hl_linux_errno_from_macos(60) == 22);
    HL_CHECK(hl_linux_errno_from_macos(4095) == 4095);
#else
    HL_CHECK(hl_linux_errno_from_macos(11) == 35);
    HL_CHECK(hl_linux_errno_from_macos(35) == 11);
    HL_CHECK(hl_linux_errno_from_macos(62) == 40);
    HL_CHECK(hl_linux_errno_from_macos(78) == 38);
    HL_CHECK(hl_linux_errno_from_macos(84) == 75);
    HL_CHECK(hl_linux_errno_from_macos(91) == 42);
    HL_CHECK(hl_linux_errno_from_macos(93) == 61);
    HL_CHECK(hl_linux_errno_from_macos(96) == 61);
    HL_CHECK(hl_linux_errno_from_macos(102) == 95);
    HL_CHECK(hl_linux_errno_from_macos(104) == 131);
    HL_CHECK(hl_linux_errno_from_macos(105) == 130);
    HL_CHECK(hl_linux_errno_from_macos(4095) == 4095);
#endif
    HL_CHECK(hl_linux_errno_from_macos(-1) == -1);
    return EXIT_SUCCESS;
}
