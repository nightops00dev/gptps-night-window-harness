/* erp_redrive_job.c - vendor job whose licence can be "renewed" overnight.
 * Exit 3 while licence.renewed is absent next to this exe (dead-letter),
 * exit 0 once the morning operator drops that file (re-drive succeeds).
 * The marker lives beside the executable, not in the current directory, so
 * parallel runs never share it. Evidence line on stderr either way.
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

int main(void)
{
    char path[1024];
    char *slash;
    FILE *f;
    if (!GetModuleFileNameA(NULL, path, (DWORD)sizeof path)) return 2;
    slash = strrchr(path, '\\');
    if (slash) slash[1] = '\0';
    strncat(path, "licence.renewed", sizeof path - strlen(path) - 1);
    f = fopen(path, "rb");
    if (f) {
        fclose(f);
        fprintf(stderr, "TAIL-EVIDENCE exit=0 marker=renewed\n");
        return 0;
    }
    fprintf(stderr, "TAIL-EVIDENCE exit=3 marker=licence-missing\n");
    return 3;
}
