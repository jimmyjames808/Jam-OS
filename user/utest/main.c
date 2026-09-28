/* utest: the M5 test program, run under init. Phase 2 fills it with the
 * milestone checks (M5-PLAN.md, "Phase 2"); for now it only proves it
 * builds, packs into bootfs and parses as an ELF. */
#include <os.h>

int main(int argc, char **argv)
{
    (void)argc, (void)argv;
    printf("utest: stub\n");
    return 0;
}
