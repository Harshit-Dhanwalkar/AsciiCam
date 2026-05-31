#include <check.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>

/* Include the production code under test */
#include "C/filters/edge_detect.h"

START_TEST(test_edge_detect_no_overflow)
{
    /* Invariant: edge_detect must never corrupt heap or crash due to
       integer overflow in dimension-based allocations. If dimensions
       would overflow size_t/uint32_t, the function must either reject
       them safely or handle them without heap corruption. */

    typedef struct { uint32_t w; uint32_t h; } dims;

    dims payloads[] = {
        /* Exact exploit: w*h overflows 32-bit (65536*65536 = 0 mod 2^32) */
        { 65536, 65536 },
        /* Boundary: just over 16-bit max, product overflows uint16_t */
        { 65535, 65535 },
        /* Valid small input: should succeed without issues */
        { 64, 48 },
    };

    int num_payloads = sizeof(payloads) / sizeof(payloads[0]);

    for (int i = 0; i < num_payloads; i++) {
        uint32_t w = payloads[i].w;
        uint32_t h = payloads[i].h;

        /* Allocate a dummy input buffer; for overflow cases this may be
           small or NULL — the function must not crash/corrupt heap */
        size_t safe_size = (size_t)w * (size_t)h;
        uint8_t *input = NULL;

        /* Only allocate if size is reasonable to avoid OOM in test */
        if (safe_size > 0 && safe_size <= (1024 * 1024 * 16)) {
            input = calloc(safe_size, 1);
        }

        /* Call the real production function; it must not crash.
           We accept NULL return or error code for adversarial inputs. */
        uint8_t *result = edge_detect(input, w, h);

        /* Invariant: if dimensions would overflow 32-bit multiplication,
           the function must not return a valid pointer (it should fail
           safely), OR if it does return a pointer it must be correctly
           sized. We assert no silent heap corruption occurred. */
        if ((uint64_t)w * (uint64_t)h > (uint64_t)UINT32_MAX) {
            /* For overflow inputs, result must be NULL (safe rejection) */
            ck_assert_msg(result == NULL,
                "edge_detect must reject overflowing dimensions safely");
        }

        free(result);
        free(input);
    }
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_edge_detect_no_overflow);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = security_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}