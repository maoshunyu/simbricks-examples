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
#include <stdbool.h>
#include <stdlib.h>

#include <vfio-pci.h>
#include <dma-alloc.h>

#include "../common/reg_defs.h"
#include "driver.h"
#include <string.h>

/** Use this macro to safely access a register at a specific offset */
#define ACCESS_REG(r) (*(volatile uint64_t *) ((uintptr_t) regs + r))
#define ACCESS_REG_BYTE(r) (*(volatile uint8_t *) ((uintptr_t) regs + r))

static void *regs;

static bool use_dma;
static size_t dma_mem_size = 16 * 1024;
static uint8_t *dma_mem;
static uintptr_t dma_mem_phys;

static uint8_t *dma_out;
static uintptr_t dma_out_phys;
static size_t dma_out_size = 8 * 32;

int accelerator_init(bool dma) {
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

  use_dma = dma;
  if (dma) {
    if (dma_alloc_init()) {
      fprintf(stderr, "DMA INIT failed\n");
      return -1;
    }

    if (!(dma_mem = dma_alloc_alloc(dma_mem_size, &dma_mem_phys))) {
      fprintf(stderr, "Allocating DMA memory failed\n");
      return -1;
    }
    if (!(dma_out = dma_alloc_alloc(dma_out_size, &dma_out_phys))) {
      fprintf(stderr, "Allocating DMA DB memory failed\n");
      return -1;
    }


    if (vfio_busmaster_enable(&dev)) {
      fprintf(stderr, "Enabling busmastering failed\n");
      return -1;
    }
  }

  // FILL ME IN

  return 0;
}

#include "utils.h"

void accel(const uint8_t * restrict A,
                   uint8_t * restrict out, size_t n) {

  memcpy(dma_mem, A, n);


  /////////////////////
  for(int i = 0;i < 8;i++){
    int off =  i * 32;
    ACCESS_REG(REG_DMA_ADDR_IN + off) = dma_mem_phys;
    ACCESS_REG(REG_DMA_ADDR_OUT + off) = dma_out_phys + i * 32;// 32Byte
    ACCESS_REG(REG_DMA_LEN + off) = n;
    ACCESS_REG_BYTE(REG_DMA_CTRL_IN + off) = 1;
    while (ACCESS_REG_BYTE(REG_DMA_CTRL_IN + off))
      ;
  }
  /////////////////////


  uint64_t total_cycles = 0;
  uint64_t start = rdtsc();


  ACCESS_REG_BYTE(REG_CTRL) = 1;
  while(ACCESS_REG_BYTE(REG_CTRL))
    ;

  /////////////////////
  for(int i = 0;i < 8;i++){
    int off =  i * 32;
    while (!ACCESS_REG_BYTE(REG_DMA_CTRL_OUT + off))
      ;
  }
  /////////////////////


  total_cycles += rdtsc() - start;
  

  printf("Cycles per operation: %ld\n", total_cycles);
  memcpy(out, dma_out, n*32);// TODO


  // TODO 删掉
  for(int i=0;i<8;i++){
    fprintf(stderr, "out[%d] = %d\n", i, *(out + i*32));
  }
}
