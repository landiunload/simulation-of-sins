#include "sim/beings.h"
#include "sim/biology.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "biology test failed: %s\n", message);
        exit(1);
    }
}

static void TestNeedScale(void)
{
    /* Pinned: needs and health are stored and normalised against this scale, so
     * changing the value would shift every saved being. */
    Expect(SOS_NEED_MAX == UINT32_C(0x40000000), "the need scale is 2^30");
    Expect(2u * (uint64_t)SOS_NEED_MAX <= UINT32_MAX, "two needs cannot overflow 32 bits");
}

static void TestSexes(void)
{
    Expect(SOS_SEX_FEMALE == 0, "female is the first sex");
    Expect(SOS_SEX_MALE == 1, "male is the second sex");
    Expect(SOS_SEX_FEMALE != SOS_SEX_MALE, "the two sexes differ");
}

int main(void)
{
    TestNeedScale();
    TestSexes();
    printf("biology tests passed\n");
    return 0;
}
