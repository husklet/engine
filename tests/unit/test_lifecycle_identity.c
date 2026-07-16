#include "test.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    FILE *file = fopen("tests/e2e/lifecycle.tsv", "r");
    char line[256], artifact[16][96];
    size_t count = 0;
    HL_CHECK(file != NULL);
    while (fgets(line, sizeof line, file)) {
        char *first, *second;
        if (line[0] == '#') continue;
        first = strchr(line, '\t');
        second = first ? strchr(first + 1, '\t') : NULL;
        HL_CHECK(second != NULL && count < 16);
        second++;
        second[strcspn(second, "\r\n")] = 0;
        for (size_t index = 0; index < count; ++index) HL_CHECK(strcmp(artifact[index], second) != 0);
        HL_CHECK(strlen(second) < sizeof artifact[count]);
        strcpy(artifact[count++], second);
    }
    HL_CHECK(fclose(file) == 0);
    HL_CHECK(count == 8);
    return 0;
}
