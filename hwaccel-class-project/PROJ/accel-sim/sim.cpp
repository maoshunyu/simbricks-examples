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

#define MAX_GPUS 32

std::deque<work_item_t *> work_queue;
uint64_t CU_NUM;
static uint8_t mem[MAX_GPUS][MAX_GPUS];
static uint32_t states[MAX_GPUS]; // 32 = MAX_GPUS // TODO: 所有uint32都要改掉
uint32_t ready_mem;
uint32_t issued;
static uint8_t **result;
static uint8_t result_t[MAX_GPUS]; // transpose result
uint64_t ready_cu;                 // 最多64个CU
uint8_t **dispatched;              // 每个CU的内存和状态
uint8_t finished_nums;             // 完成的任务数目
uint8_t tp_num;
uint8_t ep_num;
uint8_t gpus_num;

// 多层交换机相关变量
bool multi_layer;        // 是否启用多层模式
uint8_t **layer2_data;   // 第二层交换机数据
uint8_t layer2_finished; // 第二层完成的组数

uint64_t OP_START;
uint64_t OP_FIND_LINE;
uint64_t OP_DISPATCH;
uint64_t OP_GET_RESULT;
uint64_t OP_DMA;
uint64_t OP_DONE;

// 第二层交换机操作延迟
uint64_t OP_LAYER2_ALLTOALL;
uint64_t OP_LAYER2_BROADCAST;

uint64_t OFF_IN = 0x1000;
uint64_t OFF_OUT = 0x2000;

uint64_t op_latency;
uint64_t matrix_size;
uint64_t mem_size;

uint8_t ctrl;
uint64_t expected_time;

uint64_t dma_addr_in[MAX_GPUS]; // 扩展到支持32个GPU
uint64_t dma_addr_out[MAX_GPUS];
uint64_t dma_len[MAX_GPUS];
uint16_t dma_ctrl_in[MAX_GPUS];
uint16_t dma_ctrl_out[MAX_GPUS];

int InitState(void) {
  CU_NUM = 32;
  ready_cu = UINT64_MAX;
  dispatched = new uint8_t *[CU_NUM];
  for (uint64_t i = 0; i < CU_NUM; i++) {
    dispatched[i] = new uint8_t[MAX_GPUS + 1];
  }
  result = new uint8_t *[CU_NUM];
  for (uint64_t i = 0; i < CU_NUM; i++) {
    result[i] = new uint8_t[MAX_GPUS + 1]; // 9代表给那一个卡
  }
  finished_nums = 0;
  ready_mem = 0;

  multi_layer = false;
  layer2_finished = 0;

  // 初始化第二层交换机数据结构
  layer2_data = new uint8_t *[MAX_GPUS]; // 最多支持32个GPU
  for (uint64_t i = 0; i < MAX_GPUS; i++) {
    layer2_data[i] = new uint8_t[MAX_GPUS]; // 每个GPU的数据
    memset(layer2_data[i], 0, MAX_GPUS);
  }

  // 1cycle = 5000 = 5ns
  OP_START = 5000;        // Size/BW + 5ns  8MB/64GBps = 125us
  OP_FIND_LINE = 40000;   // 8 Cycles
  OP_DISPATCH = 20000;    // 4 Cycles
  OP_GET_RESULT = 200000; // 13 * 3 Cycles
  OP_DMA = 5000;          // Size/BW + 5ns
  OP_DONE = 1000;

  OP_LAYER2_ALLTOALL = 100000; // 执行alltoall延迟
  OP_LAYER2_BROADCAST = 80000; // 广播结果延迟

  tp_num = 1;
  ep_num = MAX_GPUS;
  gpus_num = tp_num * ep_num;

  ctrl = 0;

  expected_time = UINT64_MAX;

  return 0;
}

void MMIORead(volatile struct SimbricksProtoPcieH2DRead *read) {
  // #ifdef DEBUG
  //   fprintf(stderr, "MMIO Read: BAR %d offset 0x%lx len %d\n", read->bar,
  //     read->offset, read->len);
  // #endif

  // praepare read completion
  volatile union SimbricksProtoPcieD2H *msg = AllocPcieOut();
  volatile struct SimbricksProtoPcieD2HReadcomp *rc = &msg->readcomp;
  rc->req_id = read->req_id; // set req id so host can match resp to a req

  // zero it out in case of bad register
  memset((void *)rc->data, 0, read->len);

  void *src = NULL;

  if (read->offset < OFF_OUT) {
    assert(read->len <= 8);
    assert(read->offset % read->len == 0);
    switch (read->offset) {
    case REG_CTRL:
      src = &ctrl;
      break;
    case REG_TP_NUM:
      src = &tp_num;
      break;
    case REG_EP_NUM:
      src = &ep_num;
      break;
    case REG_MULTI_LAYER:
      src = &multi_layer;
      break;
    case REG_OFF_IN:
      src = &OFF_IN;
      break;
    case REG_OFF_OUT:
      src = &OFF_OUT;
      break;
    default:
      if (read->offset >= REG_DMA_LEN &&
          read->offset < (REG_DMA_LEN + MAX_GPUS * 32)) {
        int i = (read->offset - REG_DMA_LEN) / 32;
        int j = (read->offset - REG_DMA_LEN) % 32;
        switch (j) {
        case 0:
          src = dma_len + i;
          break;
        case 8:
          src = dma_addr_in + i;
          break;
        case 16:
          src = dma_addr_out + i;
          break;
        case 24:
          src = dma_ctrl_in + i;
          break;
        case 26:
          src = dma_ctrl_out + i;
          break;
        default:
          fprintf(stderr,
                  "MMIO Read: warning read from invalid register 0x%lx\n",
                  read->offset);
        }
      } else {
        fprintf(stderr, "MMIO Read: warning read from invalid register 0x%lx\n",
                read->offset);
      }
    }
  } else if (read->offset < (OFF_OUT + MAX_GPUS * 8) &&
             read->offset >= OFF_OUT) {
    src = result + (read->offset - OFF_OUT);
  } else {
    fprintf(stderr, "MMIO Read: warning invalid MMIO read 0x%lx\n",
            read->offset);
  }

  if (src)
    memcpy((void *)rc->data, src, read->len);

  // send response
  SendPcieOut(msg, SIMBRICKS_PROTO_PCIE_D2H_MSG_READCOMP);
}

void MMIOWrite(volatile struct SimbricksProtoPcieH2DWrite *write) {
#ifdef DEBUG
  fprintf(stderr, "MMIO Write: BAR %d offset 0x%lx len %d\n", write->bar,
          write->offset, write->len);
#endif

  if (write->offset < OFF_IN) {
    assert(write->len <= 8);
    assert(write->offset % write->len == 0);
    switch (write->offset) {
    case REG_CTRL: {
      memcpy(&ctrl, (const void *)write->data, write->len);
      work_item_t *work = new work_item_t;
      work->type = FIND_LINE;
      work->expected_time = main_time + OP_START;
      AddWork(work);
#ifdef DEBUG
      fprintf(stderr, "MMIO Write: ctrl %d ex_time=%ld main=%ld\n", ctrl,
              expected_time, main_time);
#endif
      break;
    }
    case REG_TP_NUM: {
      // 设置TP组数量
      memcpy(&tp_num, (const void *)write->data, write->len);
      gpus_num = tp_num * ep_num; // 更新总GPU数量
#ifdef DEBUG
      fprintf(stderr, "MMIO Write: tp_num set to %d\n", tp_num);
#endif
      break;
    }
    case REG_EP_NUM: {
      // 设置总GPU数量
      memcpy(&ep_num, (const void *)write->data, write->len);
      gpus_num = tp_num * ep_num; // 更新总GPU数量
#ifdef DEBUG
      fprintf(stderr, "MMIO Write: ep_num set to %d\n", ep_num);
#endif
      break;
    }
    case REG_MULTI_LAYER: {
      // 设置是否启用多层模式
      memcpy(&multi_layer, (const void *)write->data, write->len);
#ifdef DEBUG
      fprintf(stderr, "MMIO Write: multi_layer set to %d\n", multi_layer);
#endif
      break;
    }
    default:
      if (write->offset >= REG_DMA_LEN &&
          write->offset < (REG_DMA_LEN + MAX_GPUS * 32)) {
        int i = (write->offset - REG_DMA_LEN) / 32;
        int j = (write->offset - REG_DMA_LEN) % 32;
        switch (j) {
        case 0:
          memcpy(dma_len + i, (const void *)write->data, write->len);
          break;
        case 8:
          memcpy(dma_addr_in + i, (const void *)write->data, write->len);
          break;
        case 16:
          memcpy(dma_addr_out + i, (const void *)write->data, write->len);
          break;
        case 24:
          memcpy(dma_ctrl_in + i, (const void *)write->data, write->len);
          IssueDMARead(mem[i], dma_addr_in[i], dma_len[i], READ_OPAQUE(i));
          break;
        default:
          fprintf(stderr, "MMIO Write: warning invalid MMIO write 0x%lx\n",
                  write->offset);
        }
      } else {
        fprintf(stderr, "MMIO Write: warning invalid MMIO write 0x%lx\n",
                write->offset);
      }
    }
  } else if (write->offset >= OFF_IN &&
             write->offset < (OFF_IN + MAX_GPUS * 8)) {
    memcpy(mem[write->offset - OFF_IN], (const void *)write->data, write->len);
  } else {
    fprintf(stderr, "MMIO Write: warning invalid MMIO write 0x%lx\n",
            write->offset);
  }
}

void PollEvent(void) {
  if (main_time >= expected_time) {
    while (!work_queue.empty() &
           (work_queue.front()->expected_time <= main_time)) {
      work_item_t *work = work_queue.front();
      work_queue.pop_front();
      ProcessWork(work);
    }

    if (!ctrl) {
      expected_time = UINT64_MAX;
      clean_states();
      return;
    }
  }
}

uint64_t NextEvent(void) {
  if (main_time != UINT64_MAX) {
    return expected_time;
  }
  return UINT64_MAX;
}

void ProcessWork(work_item_t *work) {
  work_item_t *new_work;

  // 处理多层交换机相关的工作
  if (work->type == LAYER2_ALLTOALL || work->type == LAYER2_BROADCAST) {
    ProcessLayer2Operation(work);
    return;
  }

  switch (work->type) {
  case FIND_LINE: {
#ifdef DEBUG
    fprintf(stderr, "FIND_LINE\n");
#endif
    ready_mem = 0;
    uint32_t temp = gpus_num == MAX_GPUS
                        ? 0xFFFFFFFF
                        : ((uint32_t)(1 << gpus_num) - 1); // TODO
    for (int i = 0; i < gpus_num; i++) {
      if ((states[i] == temp) &&
          !(issued & (1 << i))) { // 8个bit为1意味全部写入完成
        ready_mem |= 1 << i;
      }
    }
    if (ctrl) {
      new_work = new work_item_t;
      new_work->type = FIND_LINE;
      new_work->expected_time = main_time + OP_FIND_LINE;
      AddWork(new_work);
    }

    issued |= ready_mem; // 已经检查完毕的
    if (ready_mem) {     // 只要此时仍有可以发射的
      new_work = new work_item_t;
      new_work->type = DISPATCH;
      new_work->expected_time = main_time + OP_DISPATCH;
      new_work->data = (uint64_t)ready_mem;
      AddWork(new_work);
    }
    break;
  }
  case DISPATCH: {
    uint32_t process_mem = (uint32_t)work->data;
    uint64_t dispatched_cu = 0;
#ifdef DEBUG
    fprintf(stderr, "DISPATCH: process_mem = %x\n", process_mem);
#endif
    for (uint64_t i = 0; i < CU_NUM; i++) {
      if (process_mem == 0) { // 全部任务分配完了
        break;
      }
      if (!(ready_cu & (1 << i))) { // 该CU不可用
        continue;
      }
      ready_cu &= ~(1 << i);   // 该CU改为不可用
      dispatched_cu |= 1 << i; // 该CU已经分配任务
      // 找到最低位的1（不必须）
      int j = 0;
      while (!(process_mem & (1 << j))) {
        j++;
      }
      process_mem &= ~(1 << j); // 该位设为0

      // 复制到CU的内存中
      for (int k = 0; k < gpus_num; k++) {
        dispatched[i][k] = mem[k][j];
      }
      dispatched[i][MAX_GPUS] = j; // [8]状态位，表示第几行
    }
    if (dispatched_cu) { // 有任务成功分配
      new_work = new work_item_t;
      new_work->type = GET_RESULT;
      new_work->expected_time = main_time + OP_GET_RESULT;
      new_work->data = dispatched_cu;
      AddWork(new_work);
    }
    if (process_mem) { // 仍然有任务未处理
      new_work = new work_item_t;
      new_work->type = DISPATCH;
      new_work->expected_time = main_time + OP_DISPATCH;
      new_work->data = (uint64_t)process_mem;
      AddWork(new_work);
    }
    break;
  }
  case GET_RESULT: {
    uint64_t result_cu = work->data;
#ifdef DEBUG
    fprintf(stderr, "GET_RESULT: result_cu = %lx\n", result_cu);
#endif
    for (uint64_t i = 0; i < CU_NUM; i++) {
      if (result_cu == 0) {
        break;
      }
      if (!(result_cu & (1 << i))) {
        continue;
      }
      result_cu &= ~(1 << i);
      ready_cu |= 1 << i; // 该CU变为可用

      uint8_t gpu_idx = dispatched[i][MAX_GPUS]; // 获取GPU索引
      result[i][MAX_GPUS] = gpu_idx;
      dispatched[i][MAX_GPUS] = 0;

      for (int j = 0; j < ep_num; j++) {
        for (int k = 0; k < tp_num; k++) {
          result[i][j] += dispatched[i][j * tp_num + k];
        }
        if (multi_layer)
          layer2_data[gpu_idx][j] = result[i][j];
      }
    }

    new_work = new work_item_t;
    new_work->type = DMA;
    new_work->expected_time = main_time + OP_DMA;
    new_work->data = work->data;
    AddWork(new_work);

    break;
  }
  case DMA: {
    uint64_t finished_cu = work->data;
#ifdef DEBUG
    fprintf(stderr, "DMA: finished_cu = %lx\n", finished_cu);
#endif

    if (multi_layer) {
      // 在多层模式下，完成第一层后要触发第二层操作
      for (uint64_t i = 0; i < CU_NUM; i++) {
        if (finished_cu & (1 << i)) {
          // 将结果标记为已完成，但不直接写回，等待第二层操作完成后统一写回
          finished_nums++; // 增加完成的任务数
        }
      }

      // 如果所有第一层任务都完成，触发第二层操作
      if (finished_nums >= ep_num) {
        work_item_t *layer2_work = new work_item_t;
        layer2_work->type = LAYER2_ALLTOALL;
        layer2_work->expected_time = main_time + OP_LAYER2_ALLTOALL;
        AddWork(layer2_work);
      }
    } else {
      // 单层模式直接写回结果
      for (uint64_t i = 0; i < CU_NUM; i++) {
        if (finished_cu & (1 << i)) {
          for (int j = 0; j < ep_num; j++) {
            result_t[j] = result[i][j];
          }
          IssueDMAWrite(dma_addr_out[result[i][MAX_GPUS]], result_t, ep_num,
                        WRITE_OPAQUE(result[i][MAX_GPUS]));
          for (int j = 0; j < ep_num; j++) {
            result[i][j] = 0;
          }
        }
      }
    }

    if (!work_queue.empty()) {
      expected_time = work_queue.front()->expected_time;
    }
    break;
  }
  case DONE:
#ifdef DEBUG
    fprintf(stderr, "DONE main=%ld\n", main_time);
#endif
    ctrl = 0;
    break;

  default:
    fprintf(stderr, "Unknown work type: %d\n", work->type);
    break;
  }

  delete work;
}

// 处理第二层交换机操作
void ProcessLayer2Operation(work_item_t *work) {
  work_item_t *new_work;

  switch (work->type) {
  case LAYER2_ALLTOALL: {
#ifdef DEBUG
    fprintf(stderr, "LAYER2_ALLTOALL\n");
#endif
    // 这里我们用一个简单的转置操作来模拟alltoall
    uint8_t temp_data[MAX_GPUS][MAX_GPUS];

    // 首先保存当前数据副本
    for (int i = 0; i < ep_num; i++) {
      for (int j = 0; j < ep_num; j++) {
        temp_data[i][j] = layer2_data[i][j];
      }
    }

    // 执行alltoall
    for (int i = 0; i < ep_num; i++) {
      for (int j = 0; j < ep_num; j++) {
        // 每个GPU i 的第j个元素应该发给GPU j
        if (j < ep_num) {
          layer2_data[j][i] = temp_data[i][j];
        }
      }
    }

    // 触发下一阶段：广播结果
    new_work = new work_item_t;
    new_work->type = LAYER2_BROADCAST;
    new_work->expected_time = main_time + OP_LAYER2_BROADCAST;
    AddWork(new_work);
    break;
  }

  case LAYER2_BROADCAST: {
#ifdef DEBUG
    fprintf(stderr, "LAYER2_BROADCAST\n");
#endif

    // 广播第二层交换机结果：将结果写回到主机内存
    for (int i = 0; i < ep_num; i++) {
      uint8_t out_data[MAX_GPUS];
      for (int j = 0; j < ep_num; j++) {
        out_data[j] = layer2_data[i][j];
      }
      // 写回结果
      IssueDMAWrite(dma_addr_out[i], out_data, ep_num, WRITE_LAYER2_OPAQUE(i));
    }

    // 触发完成事件
    new_work = new work_item_t;
    new_work->type = DONE;
    new_work->expected_time = main_time + OP_DONE;
    AddWork(new_work);
    break;
  }

  default:
    fprintf(stderr, "Unknown layer2 work type: %d\n", work->type);
    break;
  }

  delete work;
}

void AddWork(work_item_t *work) {
  // Insert work into the queue in the order of expected_time
  if (work_queue.empty()) {
    work_queue.push_back(work);
  } else {
    std::deque<work_item_t *>::iterator it = work_queue.begin();
    while (it != work_queue.end() &&
           (*it)->expected_time < work->expected_time) {
      it++;
    }
    work_queue.insert(it, work);
  }
  expected_time = work_queue.front()->expected_time;
#ifdef DEBUG
  fprintf(
      stderr,
      "AddWork: type = %d   work->expected_time = %ld  expected_time = %ld\n",
      work->type, work->expected_time, expected_time);
#endif
}

void DMACompleteEvent(uint64_t opaque) {
  if (opaque >= 0x1000 && opaque < 0x2000) {
#ifdef DEBUG
    fprintf(stderr, "DMACompleteRead %lx\n", opaque);
#endif
    dma_ctrl_in[opaque - 0x1000] = 0; // 读取数据完毕
    for (int i = 0; i < gpus_num; i++) {
      states[i] |= (uint32_t)(1 << (opaque - 0x1000));
    }
  } else if (opaque >= 0x2000 && opaque < 0x3000) {
    dma_ctrl_out[opaque - 0x2000] = 1;
    if (!multi_layer) {
      // 在单层模式下，直接增加完成的任务数
      finished_nums++;
      if (finished_nums == ep_num) {
        work_item_t *new_work = new work_item_t;
        new_work->type = DONE;
        new_work->expected_time = main_time + OP_DONE;
        AddWork(new_work);
      }
    }
#ifdef DEBUG
    fprintf(stderr, "DMACompleteWrite %lx time = %ld\n", opaque, main_time);
    fprintf(stderr, "finished_nums = %d\n", finished_nums);
#endif
  } else if (opaque >= 0x4000 && opaque < 0x5000) {
    // 处理第二层交换机写回完成事件
    uint32_t gpu_idx = opaque - 0x4000;
    dma_ctrl_out[gpu_idx] = 1;
    layer2_finished++;

#ifdef DEBUG
    fprintf(stderr, "Layer2 DMACompleteWrite for GPU %d, time = %ld\n", gpu_idx,
            main_time);
    fprintf(stderr, "layer2_finished = %d\n", layer2_finished);
#endif

    // 如果所有GPU都已完成，触发DONE事件
    if (layer2_finished == ep_num) {
      work_item_t *new_work = new work_item_t;
      new_work->type = DONE;
      new_work->expected_time = main_time + OP_DONE;
      AddWork(new_work);
    }
  }

  if (!work_queue.empty())
    expected_time = work_queue.front()->expected_time;
}

void clean_states() {
  for (int i = 0; i < MAX_GPUS; i++) {
    states[i] = 0;
  }
  ready_cu = UINT64_MAX;
  finished_nums = 0;
  issued = 0;
  ready_mem = 0;
  tp_num = 1;

  // 清理多层交换机状态
  multi_layer = false;
  layer2_finished = 0;

  for (uint64_t i = 0; i < CU_NUM; i++) {
    delete[] dispatched[i];
  }
  delete[] dispatched;
  for (uint64_t i = 0; i < CU_NUM; i++) {
    delete[] result[i];
  }
  delete[] result;
  for (uint64_t i = 0; i < MAX_GPUS; i++) {
    delete[] layer2_data[i];
  }
  delete[] layer2_data;

  while (!work_queue.empty()) {
    work_item_t *work = work_queue.front();
    work_queue.pop_front();
    delete work;
  }
#ifdef DEBUG
  fprintf(stderr, "clean_states\n");
#endif
}