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

const uint64_t OFF_INA = 0x100000; // 1024 * 1024 = 2 ^ 20
const uint64_t OFF_INB = 0x200000;
const uint64_t OFF_OUT = 0x300000;

uint8_t *matrix_a;
uint8_t *matrix_b;
uint8_t *matrix_out;

uint8_t ctrl;
uint64_t expected_time;

// uncomment to enable debug prints
// #define DEBUG

// these are initialized from the main function based on command line parameters
uint64_t op_latency;
uint64_t matrix_size;


int InitState(void) {
  ctrl = 0;
  matrix_a = malloc(matrix_size * matrix_size);
  matrix_b = malloc(matrix_size * matrix_size);
  matrix_out = malloc(matrix_size * matrix_size);
  expected_time = UINT64_MAX;
  return 0;
}

void MMIORead(volatile struct SimbricksProtoPcieH2DRead *read)
{
#ifdef DEBUG
  fprintf(stderr, "MMIO Read: BAR %d offset 0x%lx len %d\n", read->bar,
    read->offset, read->len);
#endif

  // prepare read completion
  volatile union SimbricksProtoPcieD2H *msg = AllocPcieOut();
  volatile struct SimbricksProtoPcieD2HReadcomp *rc = &msg->readcomp;
  rc->req_id = read->req_id; // set req id so host can match resp to a req

  // zero it out in case of bad register
  memset((void *) rc->data, 0, read->len);

  void *src = NULL;

  // Evry Read is 8 bytes max
  if (read->offset < 64) {
    // design choice: All our actual registers need to be accessed with 64-bit
    // aligned reads
    assert(read->len <= 8);
    assert(read->offset % read->len == 0);

    switch (read->offset) {
      case REG_SIZE: src = &matrix_size; break;
      case REG_OFF_INA: src = &OFF_INA; break;
      case REG_OFF_INB: src = &OFF_INB; break;
      case REG_OFF_OUT: src = &OFF_OUT; break;
      case REG_CTRL: src = &ctrl; break;
      default:
        fprintf(stderr, "MMIO Read: warning read from invalid register "
                        "0x%lx\n",
                read->offset);
    }
  } else if(read->offset < (OFF_OUT + matrix_size * matrix_size) && read->offset >= OFF_OUT) {
    src = matrix_out + (read->offset - OFF_OUT);
  } 
  else {
    fprintf(stderr, "MMIO Read: warning invalid MMIO read 0x%lx\n",
          read->offset);
  }

  // copy data into response message
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


  // Evry Write is 8 bytes max
  if (write->offset < 64) {
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
      default:
        fprintf(stderr, "MMIO Write: warning write to invalid register "
                        "0x%lx = 0x%lx\n",
                write->offset, val);
    }
  } else if(write->offset < (OFF_INA + matrix_size * matrix_size) && write->offset >= OFF_INA) {
    memcpy(matrix_a + (write->offset - OFF_INA), (const void *) write->data, write->len);
  } else if(write->offset < (OFF_INB + matrix_size * matrix_size) && write->offset >= OFF_INB) {
    memcpy(matrix_b + (write->offset - OFF_INB), (const void *) write->data, write->len);
  }
  else {
    fprintf(stderr, "MMIO Write: warning invalid MMIO write 0x%lx\n",
          write->offset);
  }

  // note that writes need no completion
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
