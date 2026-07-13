#ifndef HL_TEST_H
#define HL_TEST_H

#include <stdio.h>
#include <stdlib.h>

#define HL_CHECK(expression)                                                                                           \
    do {                                                                                                               \
        if (!(expression)) {                                                                                           \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expression);                             \
            return EXIT_FAILURE;                                                                                       \
        }                                                                                                              \
    } while (0)

#endif
