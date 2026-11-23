/* erp_redrive_job.c - vendor job whose licence can be "renewed" overnight.
 * Exit 3 while licence.renewed is absent (dead-letter), exit 0 once the
 * morning operator drops that file (re-drive succeeds). Evidence line on
 * stderr either way.
 */
#include <stdio.h>

int main(void)
{
    FILE *f = fopen("licence.renewed", "rb");
    if (f) {
        fclose(f);
        fprintf(stderr, "TAIL-EVIDENCE exit=0 marker=renewed\n");
        return 0;
    }
    fprintf(stderr, "TAIL-EVIDENCE exit=3 marker=licence-missing\n");
    return 3;
}
