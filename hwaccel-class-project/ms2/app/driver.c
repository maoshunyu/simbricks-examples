/*
 * Copyright 2023 Max Planck Institute for Software Systems, and
 * National University of Singapore
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vfio-pci.h>

#include "../common/reg_defs.h"
#include "driver.h"

/** Use this macro to safely access a register at a specific offset */
#define ACCESS_REG(r) (*(volatile uint64_t *) ((uintptr_t) regs + r))
#define ACCESS_REG_BYTE(r) (*(volatile uint64_t *) ((uintptr_t) regs + r))

static void *regs;

int accelerator_init(void) {
  struct vfio_dev dev;
  size_t reg_len;

  if (vfio_dev_open(&dev, "/dev/vfio/noiommu-0", "0000:00:00.0") != 0) {
    fprintf(stderr, "open device failed\n");
    return -1;
  }

  if(vfio_region_map(&dev, 0, &regs, &reg_len)) {
    fprintf(stderr, "mapping registers failed\n");
    return -1;
  }

  // after this registers are accessible

  // YOU MAY WANT ADDITIONAL INITIALIZATION CODE HERE
  return 0;
}

int accelerator_matrix_size(void) {
  // YOU CAN CHANGE THIS, THIS IS JUST AN EXAMPLE
  return ACCESS_REG(REG_SIZE);
}

void set_accelerator_matrix_size(uint64_t size) {
  // YOU CAN CHANGE THIS, THIS IS JUST AN EXAMPLE
  ACCESS_REG(REG_SIZE) = size;
  printf("Set matrix size to %lu\n", size);
}

void matmult_accel(const uint8_t * restrict A, const uint8_t * restrict B,
                   uint8_t * restrict out, size_t n) {
  uint64_t off_a = ACCESS_REG(REG_OFF_INA);
  uint64_t off_b = ACCESS_REG(REG_OFF_INB);
  uint64_t off_out = ACCESS_REG(REG_OFF_OUT);
  memcpy((void *) ((uintptr_t) regs + off_a), A, n * n);
  memcpy((void *) ((uintptr_t) regs + off_b), B, n * n);
  ACCESS_REG_BYTE(REG_CTRL) = 1;
  while(ACCESS_REG_BYTE(REG_CTRL) != 0)
    ;
  memcpy(out, (void *) ((uintptr_t) regs + off_out), n * n);
}
