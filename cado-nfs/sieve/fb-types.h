#ifndef FB_TYPES_H
#define FB_TYPES_H

// pragma no prototypes

/* Elementary data types for the factor base */

#include <stdint.h>
#include <inttypes.h>
#include "las-config.h"

typedef uint32_t fbprime_t; /* 32 bits should be enough for everyone */
#define FBPRIME_FORMAT PRIu32
#define FBPRIME_MAX UINT32_MAX
#define FBPRIME_BITS 32
typedef fbprime_t fbroot_t;
#define FBROOT_FORMAT PRIu32
typedef fbprime_t redc_invp_t;



/* Within one factor base, there is exactly one (index, offset) tuple per
   factor base entry. */
/* Each slice in a factor base has a unique index */
typedef uint32_t slice_index_t;
/* Each factor base entry withing a slice has a unique offset */
typedef uint16_t slice_offset_t;

// FIXME: could probably go somewhere else...
// Small struct for sublattice info:
// One sieves only positions congrent to (i0,j0) mod m
struct sublat_s {
    uint32_t m=0; // 0 means no sublattices.
    uint32_t i0=0;
    uint32_t j0=0;
};
typedef struct sublat_s sublat_t;


#endif
