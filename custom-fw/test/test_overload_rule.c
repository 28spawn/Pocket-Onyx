/* test/test_overload_rule.c - the OVERLOAD popup's xrun-burst rule (include/pa_overload.h): a quick scroll through presets (one overrun per swap) must not raise it, real bursts must. */
#include <stdio.h>
#include "../include/pa_overload.h"

static int fails;
#define CHECK(ok, ...) do { if (!(ok)) { fails++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    /* the bench: one overrun per swap, at any scrolling speed a hand can do (up to 20 swaps a second) */
    for (uint32_t sw = 0; sw <= 20u; sw++) CHECK(!pa_xrun_burst(sw, sw), "%u swaps with %u overruns raised the popup", sw, sw);
    for (uint32_t sw = 0; sw <= 20u; sw++) CHECK(!pa_xrun_burst(2u * sw, sw), "%u swaps with two overruns each raised the popup", sw);
    /* real bursts: no swaps */
    CHECK(!pa_xrun_burst(3u, 0u), "3 overruns without a swap are not a burst");
    CHECK(pa_xrun_burst(4u, 0u), "4 overruns without a swap are a burst (the old rule)");
    CHECK(pa_xrun_burst(40u, 0u), "40 overruns without a swap are a burst");
    /* swaps and a burst on top */
    CHECK(pa_xrun_burst(2u * 3u + 4u, 3u), "3 swaps plus 4 extra overruns are a burst");
    CHECK(!pa_xrun_burst(2u * 3u + 3u, 3u), "3 swaps plus 3 extra overruns are not");
    printf("%s\n", fails ? "OVERLOAD RULE FAILED" : "OVERLOAD RULE OK");
    return fails ? 1 : 0;
}
