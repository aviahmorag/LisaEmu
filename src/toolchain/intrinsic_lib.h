#ifndef INTRINSIC_LIB_H
#define INTRINSIC_LIB_H

#include <stdint.h>

/* Generate a minimal INTRINSIC.LIB in Lisa OBJ format.
 * Returns a malloc'd buffer; caller frees. *out_size set to byte count. */
uint8_t *build_intrinsic_lib(uint32_t *out_size);

#endif
