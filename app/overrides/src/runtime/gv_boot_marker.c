// GameVault diagnostics: write /data/gamevault/boot.txt as early as this
// process can, before main() and before the toolkit opens the display, so a
// launch that ends at once still shows how far it got.
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void gv_boot_mark(const char *line)
{
    mkdir("/data/gamevault", 0777);
    int fd = open("/data/gamevault/boot.txt", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
        fd = open("/data/boot-gamevault.txt", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
        return;
    write(fd, line, strlen(line));
    write(fd, "\n", 1);
    close(fd);
}

__attribute__((constructor(101))) static void gv_boot_first(void)
{
    gv_boot_mark("boot: process started (constructor)");
}
