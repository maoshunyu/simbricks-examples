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

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <simbricks/pcie/if.h>

#include "sim.h"
#include "../common/reg_defs.h"

// uncomment to enable debug prints
//#define DEBUG

const uint64_t OFF_INA = 0x0; 
const uint64_t OFF_INB = 0x4000; // 128 * 128 = 2 ^ 14
const uint64_t OFF_OUT = 0x8000; // 128 * 128 * 2

uint64_t op_latency;
uint64_t matrix_size;
uint64_t mem_size;

static uint8_t *mem;

uint8_t *matrix_a;
uint8_t *matrix_b;
uint8_t *matrix_out;

uint8_t ctrl;
uint8_t dma_ctrl;
uint8_t dma_ctrl_run;
uint8_t dma_ctrl_w;
uint64_t expected_time;

uint64_t dma_addr;
uint64_t dma_len;
uint64_t dma_off;

int InitState(void) {
  if (!(mem = calloc(1, mem_size)))
    return -1;

  ctrl = 0;
  dma_ctrl = 0;
  dma_ctrl_run = 0;
  dma_ctrl_w = 0;

  matrix_a = mem;
  matrix_b = mem + matrix_size * matrix_size;
  matrix_out = mem + matrix_size * matrix_size * 2;
  expected_time = UINT64_MAX;

  return 0;
}

void MMIORead(volatile struct SimbricksProtoPcieH2DRead *read)
{
#ifdef DEBUG
  fprintf(stderr, "MMIO Read: BAR %d offset 0x%lx len %d\n", read->bar,
    read->offset, read->len);
#endif

  // praepare read completion
  volatile union SimbricksProtoPcieD2H *msg = AllocPcieOut();
  volatile struct SimbricksProtoPcieD2HReadcomp *rc = &msg->readcomp;
  rc->req_id = read->req_id; // set req id so host can match resp to a req

  // zero it out in case of bad register
  memset((void *) rc->data, 0, read->len);

  void *src = NULL;

  // FILL ME IN
  // THIS PROBABLY NEEDS CHANGES BEYOND SWTICH CASES

  if (read->offset < 256) {
    assert(read->len <= 8);
    assert(read->offset % read->len == 0);
    switch (read->offset) {
      case REG_SIZE: src = &matrix_size; break;
      case REG_MEM_SIZE: src = &mem_size; break;
      case REG_OFF_INA: src = &OFF_INA; break;
      case REG_OFF_INB: src = &OFF_INB; break;
      case REG_OFF_OUT: src = &OFF_OUT; break;
      case REG_CTRL: src = &ctrl; break;
      case REG_DMA_CTRL: src = &dma_ctrl; break;
      case REG_DMA_ADDR: src = &dma_addr; break;
      case REG_DMA_LEN: src = &dma_len; break;
      case REG_DMA_OFF: src = &dma_off; break;
      default:
        fprintf(stderr, "MMIO Read: warning read from invalid register "
                        "0x%lx\n",
                read->offset);
    }
  } else if(read->offset < (OFF_OUT + matrix_size * matrix_size) && read->offset >= OFF_OUT) {
    src = matrix_out + (read->offset - OFF_OUT);
  } else {
    fprintf(stderr, "MMIO Read: warning invalid MMIO read 0x%lx\n",
          read->offset);
  }

  if (src)
    memcpy((void *) rc->data, src, read->len);

  // send response
  SendPcieOut(msg, SIMBRICKS_PROTO_PCIE_D2H_MSG_READCOMP);
}

void MMIOWrite(volatile struct SimbricksProtoPcieH2DWrite *write)
{
#ifdef DEBUG
  fprintf(stderr, "MMIO Write: BAR %d offset 0x%lx len %d\n", write->bar,
    write->offset, write->len);
#endif

  // FILL ME IN
  // THIS PROBABLY NEEDS CHANGES BEYOND SWTICH CASES

  void *dst = NULL;
  if (write->offset < 256) {
    assert(write->len <= 8);
    assert(write->offset % write->len == 0);
    uint64_t val = 0;
    memcpy(&val, (const void *) write->data, write->len);
    switch (write->offset) {
      case REG_SIZE:
        matrix_size = val;
        break;
      case REG_CTRL:
        if(val && !ctrl){
          ctrl = 1;
          expected_time = main_time + op_latency;
        }
        break;
      case REG_DMA_CTRL:
        dma_ctrl = val;
        dma_ctrl_run = val & REG_DMA_CTRL_RUN;
        dma_ctrl_w = val & REG_DMA_CTRL_W;
        if(dma_ctrl_run){
          if(dma_ctrl_w){
            IssueDMAWrite(dma_addr, mem + dma_off, dma_len, 0);
          } else {
            IssueDMARead(mem + dma_off, dma_addr, dma_len, 0);
          }
        }
        break;
      case REG_DMA_ADDR:
        dma_addr = val;
        break;
      case REG_DMA_LEN:
        dma_len = val;
        break;
      case REG_DMA_OFF:
        dma_off = val;
        break;
      default:
        fprintf(stderr, "MMIO Write: warning write to invalid register 0x%lx\n",
          write->offset);
    }
  } else {
    fprintf(stderr, "MMIO Write: warning invalid MMIO write 0x%lx\n",
          write->offset);
  }
}

void PollEvent(void) {
  if(main_time >= expected_time && ctrl){
    for(uint64_t i = 0; i < matrix_size; i++){
      for(uint64_t j = 0; j < matrix_size; j++){
        uint32_t sum = 0;
        for(uint64_t k = 0; k < matrix_size; k++){
          sum += matrix_a[i * matrix_size + k] * matrix_b[k * matrix_size + j];
        }
        matrix_out[i * matrix_size + j] = sum;
      }
    }
    ctrl = 0;
    expected_time = UINT64_MAX;
  }
}

uint64_t NextEvent(void) {
  if(main_time != UINT64_MAX){
    return expected_time;
  }
  return UINT64_MAX;
}

void DMACompleteEvent(uint64_t opaque) {
  dma_ctrl = 0;
  dma_ctrl_run = 0;
  dma_ctrl_w = 0;
  //expected_time = UINT64_MAX;
}