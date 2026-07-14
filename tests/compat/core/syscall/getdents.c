// Directory enumeration: create a dir with known entries, then opendir/readdir and collect the
// names (skipping . and ..), counting and checksumming them order-independently. Exercises the
// getdents/readdir path + stat-by-dirent. Portable -> all engines, golden-checked.
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

struct linux_dirent64 {
    uint64_t ino;
    int64_t off;
    unsigned short reclen;
    unsigned char type;
    char name[];
};

int main(void) {
    const char *dir = "/tmp/hl_getdents_dir";
    mkdir(dir, 0755);
    const char *names[] = {"alpha", "bravo", "charlie", "delta"};
    char p[256];
    for (int i = 0; i < 4; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, names[i]);
        FILE *f = fopen(p, "w");
        if (f) fclose(f);
    }
    DIR *d = opendir(dir);
    int count = 0;
    long namechk = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        count++;
        for (const char *c = e->d_name; *c; c++) namechk += (unsigned char)*c; // order-independent
    }
    closedir(d);
    int raw = open(dir, O_RDONLY | O_DIRECTORY);
    char buf[48];
    int raw_count = 0, layout = 1, typed = 1, eof = 0, rewind_ok = 0;
    long n;
    while ((n = syscall(SYS_getdents64, raw, buf, sizeof buf)) > 0) {
        long at = 0;
        while (at < n) {
            struct linux_dirent64 *entry = (struct linux_dirent64 *)(buf + at);
            size_t names = strnlen(entry->name, entry->reclen > 19 ? entry->reclen - 19 : 0);
            if (entry->reclen < 24 || (entry->reclen & 7) != 0 || at + entry->reclen > n || names == 0)
                layout = 0;
            if (strcmp(entry->name, ".") && strcmp(entry->name, "..") && entry->type != DT_REG) typed = 0;
            raw_count++;
            at += entry->reclen;
        }
    }
    eof = n == 0 && syscall(SYS_getdents64, raw, buf, sizeof buf) == 0;
    rewind_ok = lseek(raw, 0, SEEK_SET) == 0 && syscall(SYS_getdents64, raw, buf, sizeof buf) > 0;
    close(raw);
    for (int i = 0; i < 4; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, names[i]);
        unlink(p);
    }
    rmdir(dir);
    printf("getdents count=%d namechk=%ld raw=%d layout=%d type=%d eof=%d rewind=%d\n",
           count, namechk, raw_count, layout, typed, eof, rewind_ok);
    return 0;
}
