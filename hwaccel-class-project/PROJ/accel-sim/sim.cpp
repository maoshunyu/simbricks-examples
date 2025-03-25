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

#include <cstdbool>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <simbricks/pcie/if.h>

extern "C" {
  #include "../accel-sim/sim.h"
  #include "../common/reg_defs.h"
}

#include <queue>

std::deque<work_item_t*> work_queue;
uint64_t CU_NUM;
static uint8_t mem[9][8]; // 8x8再加一个状态位
static uint8_t* states; // =mem[8]
uint8_t ready_mem;
static uint8_t issued;
static uint8_t** result; //  TODO: TP切分
static uint8_t result_t[8];// transpose result
uint64_t ready_cu; // 最多64个CU
uint8_t** dispatched; // 每个CU的内存和状态
uint8_t finished_nums; // 完成的任务数目

uint64_t OP_START;
uint64_t OP_FIND_LINE;
uint64_t OP_DISPATCH;
uint64_t OP_GET_RESULT;
uint64_t OP_DMA;
uint64_t OP_DONE;



uint64_t OFF_IN = 0x1000; 
uint64_t OFF_OUT = 0x2000;

uint64_t op_latency;
uint64_t matrix_size;
uint64_t mem_size;


uint8_t ctrl;
uint64_t expected_time;

uint64_t dma_addr_in[8];
uint64_t dma_addr_out[8];
uint64_t dma_len[8];
uint16_t dma_ctrl_in[8];
uint16_t dma_ctrl_out[8];

int InitState(void) {
  CU_NUM = 8;
  states = mem[8];
  ready_cu = UINT64_MAX;
  dispatched = new uint8_t*[CU_NUM];
  for(uint64_t i = 0; i < CU_NUM; i++){
    dispatched[i] = new uint8_t[9];
  }
  result = new uint8_t*[CU_NUM];
  for(uint64_t i = 0; i < CU_NUM; i++){
    result[i] = new uint8_t[9]; // 9代表给那一个卡
  }
  finished_nums = 0;
  ready_mem = 0;

  OP_START = 1000;
  OP_FIND_LINE = 5000;
  OP_DISPATCH = 5000;
  OP_GET_RESULT = 10000;
  OP_DMA = 1000;
  OP_DONE = 1000;

  ctrl = 0;

  expected_time = UINT64_MAX;

  return 0;
}

void MMIORead(volatile struct SimbricksProtoPcieH2DRead *read)
{
// #ifdef DEBUG
//   fprintf(stderr, "MMIO Read: BAR %d offset 0x%lx len %d\n", read->bar,
//     read->offset, read->len);
// #endif

  // praepare read completion
  volatile union SimbricksProtoPcieD2H *msg = AllocPcieOut();
  volatile struct SimbricksProtoPcieD2HReadcomp *rc = &msg->readcomp;
  rc->req_id = read->req_id; // set req id so host can match resp to a req

  // zero it out in case of bad register
  memset((void *) rc->data, 0, read->len);

  void *src = NULL;


  if (read->offset < OFF_OUT) {
    assert(read->len <= 8);
    assert(read->offset % read->len == 0);
    switch (read->offset) {
      case REG_CTRL:
        src = &ctrl;
        break;

      case REG_OFF_IN: src = &OFF_IN; break;
      case REG_OFF_OUT: src = &OFF_OUT; break;
      default:
        if(read->offset >= REG_DMA_LEN && read->offset < (REG_DMA_LEN + 32 * 8)){
          int i = (read->offset - REG_DMA_LEN) / 32;
          int j = (read->offset - REG_DMA_LEN) % 32;
          switch(j){
            case 0: src = dma_len + i; break;
            case 8: src = dma_addr_in + i; break;
            case 16: src = dma_addr_out + i; break;
            case 24: src = dma_ctrl_in + i; break;
            case 26: src = dma_ctrl_out + i; break;
            default:
              fprintf(stderr, "MMIO Read: warning read from invalid register 0x%lx\n",
                read->offset);
          }
        }else{
          fprintf(stderr, "MMIO Read: warning read from invalid register 0x%lx\n",
            read->offset);
        }
    }
  } else if(read->offset < (OFF_OUT + 64) && read->offset >= OFF_OUT) {
    src = result + (read->offset - OFF_OUT);
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

  if (write->offset < (OFF_IN + 8 * 8)) {
    assert(write->len <= 8);
    assert(write->offset % write->len == 0);
    if(write->offset == REG_CTRL){
      memcpy(&ctrl, (const void *) write->data, write->len);
      work_item_t *work = new work_item_t;
      work->type = FIND_LINE;
      work->expected_time = main_time + OP_START;
      AddWork(work);
#ifdef DEBUG
      fprintf(stderr, "MMIO Write: ctrl %d ex_time=%ld main=%ld\n", ctrl,expected_time, main_time);
#endif
    }else if(write->offset >= REG_DMA_LEN && write->offset < (REG_DMA_LEN + 32 * 8)){
      int i = (write->offset - REG_DMA_LEN) / 32;
      int j = (write->offset - REG_DMA_LEN) % 32;
      switch(j){
        case 0: memcpy(dma_len + i, (const void *) write->data, write->len); break;
        case 8: memcpy(dma_addr_in + i, (const void *) write->data, write->len); break;
        case 16: memcpy(dma_addr_out + i, (const void *) write->data, write->len); break;
        case 24: memcpy(dma_ctrl_in + i, (const void *) write->data, write->len); 
        IssueDMARead(mem[i], dma_addr_in[i], dma_len[i], READ_OPAQUE(i));break; // TODO: 搬走
        default:
          fprintf(stderr, "MMIO Write: warning invalid MMIO write 0x%lx\n", write->offset);
      }
    }else if(write->offset >= OFF_IN && write->offset < (OFF_IN + 8 * 8)){
      memcpy(mem[write->offset - OFF_IN], (const void *) write->data, write->len);
    }else {
      fprintf(stderr, "MMIO Write: warning invalid MMIO write 0x%lx\n",
          write->offset);
    }
  }
}
void PollEvent(void) {
  if(main_time >= expected_time){
    while(!work_queue.empty() & (work_queue.front()->expected_time <= main_time)){
      work_item_t *work = work_queue.front();
      work_queue.pop_front();
      ProcessWork(work);
    }
    
    if(!ctrl){
      expected_time = UINT64_MAX;
      // clean the queue
      while(!work_queue.empty()){
        work_item_t *work = work_queue.front();
        work_queue.pop_front();
        delete work;
      }
      return;
    }
  }
}

uint64_t NextEvent(void) {
  if(main_time != UINT64_MAX){
    return expected_time;
  }
  return UINT64_MAX;
}

void ProcessWork(work_item_t *work){
  work_item_t *new_work;
  switch(work->type){
    case FIND_LINE:{
      #ifdef DEBUG
      fprintf(stderr, "FIND_LINE\n");
      #endif
    ready_mem = 0;
    for (int i = 0; i < 8; i++) {
      if (states[i] == 0xFF && !(issued & (1 << i))) { // 8个bit为1意味全部写入完成
        ready_mem |= 1 << i;
      }
    }
    if(ctrl){
      new_work = new work_item_t;
      new_work->type = FIND_LINE;
      new_work->expected_time = main_time + OP_FIND_LINE;
      AddWork(new_work);
    }
    
    issued |= ready_mem; // 已经检查完毕的
    if(ready_mem){ // 只要此时仍有可以发射的
      new_work = new work_item_t;
      new_work->type = DISPATCH;
      new_work->expected_time = main_time + OP_DISPATCH;
      new_work->data = (uint64_t)ready_mem;
      AddWork(new_work);
    }
    break;
  }
    case DISPATCH:{
    uint8_t process_mem = (uint8_t)work->data;
    uint64_t dispatched_cu = 0;
    #ifdef DEBUG
    fprintf(stderr, "DISPATCH: process_mem = %b\n", process_mem);
    #endif
    for(uint64_t i = 0; i < CU_NUM;i++){
      if(process_mem == 0){// 全部任务分配完了
        break;
      }
      if(!(ready_cu & (1 << i))){ // 该CU不可用
        continue;
      }
      ready_cu &= ~(1 << i); // 该CU改为不可用
      dispatched_cu |= 1 << i; // 该CU已经分配任务
      // 找到最低位的1（不必须）
      int j = 0;
      while(!(process_mem & (1 << j))){
        j++;
      }
      process_mem &= ~(1 << j); // 该位设为0

      // 复制到CU的内存中
      for(int k = 0; k < 8; k++){
        dispatched[i][k] = mem[k][j];
      }
      dispatched[i][8] = j;// [8]状态位，表示第几行
    }
    if(dispatched_cu){ // 有任务成功分配
      new_work = new work_item_t;
      new_work->type = GET_RESULT;
      new_work->expected_time = main_time + OP_GET_RESULT;
      new_work->data = dispatched_cu;
      AddWork(new_work);
    }
    if(process_mem){ // 仍然有任务未处理
      new_work = new work_item_t;
      new_work->type = DISPATCH;
      new_work->expected_time = main_time + OP_DISPATCH;
      new_work->data = (uint64_t)process_mem;
      AddWork(new_work);
    }
    break;
  }
    case GET_RESULT:{
    uint64_t result_cu = work->data;
    #ifdef DEBUG
    fprintf(stderr, "GET_RESULT: result_cu = %b\n", result_cu);
    #endif
    for(uint64_t i = 0; i < CU_NUM; i++){
      if(result_cu == 0){
        break;
      }
      if(!(result_cu & (1 << i))){
        continue;
      }
      result_cu &= ~(1 << i);
      ready_cu |= 1 << i; // 该CU变为可用
      for(int j = 0; j < 8; j++){
        result[i][0] += dispatched[i][j]; // TODO
      }
      result[i][8] = dispatched[i][8];
      dispatched[i][8] = 0;
    }
    new_work = new work_item_t;
    new_work->type = DMA;
    new_work->expected_time = main_time + OP_DMA;
    new_work->data = work->data;
    AddWork(new_work);
    break;
  }
    case DMA:{
    uint64_t finished_cu = work->data;
    #ifdef DEBUG
    fprintf(stderr, "DMA: finished_cu = %b\n", finished_cu);
    #endif
    for(uint64_t i = 0; i < CU_NUM; i++){
      if(finished_cu & (1 << i)){ //DMA到卡i
        for(int j = 0; j < 8; j++){
          result_t[j] = result[i][0];
        }
        IssueDMAWrite(dma_addr_out[result[i][8]], result_t, 8, WRITE_OPAQUE(result[i][8])); // TODO
      }
    }
    if(!work_queue.empty()){
      expected_time = work_queue.front()->expected_time;
    }
    break;
  }
    case DONE:
    #ifdef DEBUG
    fprintf(stderr, "DONE\n");
    #endif
    ctrl = 0;
    break;

  }

  delete work;
}

void AddWork(work_item_t *work){
  // Insert work into the queue in the order of expected_time
  if (work_queue.empty()) {
    work_queue.push_back(work);
  } else {
    std::deque<work_item_t*>::iterator it = work_queue.begin();
    while (it != work_queue.end() && (*it)->expected_time < work->expected_time) {
      it++;
    }
    work_queue.insert(it, work);
  }
  expected_time = work_queue.front()->expected_time;
  #ifdef DEBUG
  fprintf(stderr, "AddWork: type = %d   work->expected_time = %ld  expected_time = %ld\n",work->type, work->expected_time,expected_time);
  #endif
}


void DMACompleteEvent(uint64_t opaque) {
  if(opaque >= 0x1000 && opaque < 0x2000){
    #ifdef DEBUG
    fprintf(stderr, "DMACompleteRead %lx\n", opaque);
    #endif
    dma_ctrl_in[opaque - 0x1000] = 0; // 读取数据完毕
    for(int i = 0; i < 8; i++){
      states[i] |= 1 << (opaque - 0x1000);
    }
  }else if(opaque >= 0x2000 && opaque < 0x3000){
    dma_ctrl_out[opaque - 0x2000] = 1; //  TODO：归零
    finished_nums++; // 完成的任务数目
    if(finished_nums == 8){
      work_item_t* new_work = new work_item_t;
      new_work->type = DONE;
      new_work->expected_time = main_time + OP_DONE;
      AddWork(new_work);
    }
    #ifdef DEBUG
    fprintf(stderr, "DMACompleteWrite %lx time = %ld\n", opaque,main_time);
    fprintf(stderr, "finished_nums = %d\n", finished_nums);
    #endif
  }
  if(!work_queue.empty())
    expected_time = work_queue.front()->expected_time;
}