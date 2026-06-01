#include <check.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>

#include "C/filters/edge_detect.h"
#include "C/filters/invert.h"
#include "C/filters/threshold.h"

typedef uint8_t* (*filter_test_func)(uint8_t*, int, int);

START_TEST(test_all_plugins_safe) {
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

    struct {
        filter_test_func func;
        const char *name;
    } plugins[] = {
        { edge_detect, "edge_detect" },
        { invert,      "invert"      },
        { threshold,   "threshold"   }
    };
    int num_plugins = sizeof(plugins) / sizeof(plugins[0]);

    // Run every test payload against every single filter plugin
    for (int p = 0; p < num_plugins; p++) {
        for (int i = 0; i < num_payloads; i++) {
            uint32_t w = payloads[i].w;
            uint32_t h = payloads[i].h;

            size_t safe_size = (size_t)w * (size_t)h;
            uint8_t *input = NULL;

            if (safe_size > 0 && safe_size <= (1024 * 1024 * 16)) {
                input = calloc(safe_size, 1);
            }

            // Execute for target plugin
            uint8_t *result = plugins[p].func(input, w, h);

            if ((uint64_t)w * (uint64_t)h > (uint64_t)UINT32_MAX) {
                char error_msg[128];
                sprintf(error_msg, "%s must reject overflowing dimensions safely", plugins[p].name);
                ck_assert_msg(result == NULL, "%s", error_msg);
            }

            free(result);
            free(input);
        }
 
        // Verifying explicit NULL buffer safety rules
        ck_assert_ptr_eq(plugins[p].func(NULL, 64, 48), NULL);
        ck_assert_ptr_eq(plugins[p].func(NULL, 0, 0), NULL);
    }
}
END_TEST;

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_all_plugins_safe);
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
