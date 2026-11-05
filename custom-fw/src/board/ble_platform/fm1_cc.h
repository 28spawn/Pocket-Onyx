/* Compiler macros the imported Optimist BLE headers (third_party/optimist_ble/hal) expect from their platform.
 * This is OUR file: the glue between that stack and this firmware. See BLE-PORT-PLAN.md. */
#pragma once
#include <stdint.h>

#define FM1_INLINE static inline __attribute__((always_inline))
