#include "test.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    FILE *file = fopen("tests/e2e/lifecycle.tsv", "r");
    char line[256], artifact[24][96];
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

    file = fopen("tests/e2e/mac_gates.tsv", "r");
    HL_CHECK(file != NULL);
    char gates[24][32] = {{0}};
    count = 0;
    size_t unique_gates = 0;
    while (fgets(line, sizeof line, file)) {
        char *tab;
        size_t gate_launches = 0;
        if (line[0] == '#') continue;
        tab = strchr(line, '\t');
        HL_CHECK(tab != NULL && count < 24);
        *tab++ = 0;
        tab[strcspn(tab, "\r\n")] = 0;
        for (size_t index = 0; index < count; ++index) {
            if (strcmp(gates[index], line) != 0) continue;
            gate_launches++;
            HL_CHECK(strcmp(artifact[index], tab) != 0);
        }
        if (gate_launches == 0) unique_gates++;
        HL_CHECK(gate_launches < 4);
        HL_CHECK(strlen(line) < sizeof gates[count]);
        HL_CHECK(strlen(tab) < sizeof artifact[count]);
        strcpy(gates[count], line);
        strcpy(artifact[count], tab);
        count++;
    }
    HL_CHECK(fclose(file) == 0);
    HL_CHECK(unique_gates == 6 && count == 17);
    return 0;
}
