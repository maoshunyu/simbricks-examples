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
#include <pthread.h>
#define GNU_SOURCE

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



static pthread_barrier_t barrier;

// 线程参数结构体
typedef struct {
    int thread_id;
    const uint8_t *A;
    uint8_t *out;
    size_t n;
} thread_arg_t;

// 线程处理函数
void *thread_handler(void *arg) {
    thread_arg_t *t_arg = (thread_arg_t *)arg;
    int i = t_arg->thread_id;
    size_t n = t_arg->n;
    
    // 计算偏移量
    int off = i * 32;
    
    // 设置 DMA 地址和长度
    ACCESS_REG(REG_DMA_ADDR_IN + off) = dma_mem_phys;
    ACCESS_REG(REG_DMA_ADDR_OUT + off) = dma_out_phys + i * 32; // 32Byte
    ACCESS_REG(REG_DMA_LEN + off) = n;
    

    ACCESS_REG_BYTE(REG_DMA_CTRL_IN + off) = 1;
    while (ACCESS_REG_BYTE(REG_DMA_CTRL_IN + off))
        ;
    
    // // 第一个同步点：等待所有线程完成 DMA 输入设置
    // pthread_barrier_wait(&barrier);
    
    // 只让线程 0 触发处理并等待
    if (i == 0) {
        ACCESS_REG_BYTE(REG_CTRL) = 1;
        while (ACCESS_REG_BYTE(REG_CTRL))
            ;
    }
    
    // 第二个同步点：等待处理完成
    pthread_barrier_wait(&barrier);
    
    // 等待 DMA 输出完成
    while (!ACCESS_REG_BYTE(REG_DMA_CTRL_OUT + off))
        ;

    // 复制结果到对应的输出位置
    memcpy(t_arg->out + off, dma_out + off, n);
    
    return NULL;
}

void accel(const uint8_t * restrict A,
                      uint8_t * restrict out, size_t n) {
    // 复制输入数据到 DMA 内存
    memcpy(dma_mem, A, n);
    ACCESS_REG(REG_TP_NUM) = 2;
    
    pthread_t threads[8];
    thread_arg_t thread_args[8];
    
    // 初始化屏障，需要同步 8 个线程
    pthread_barrier_init(&barrier, NULL, 8);
    
    uint64_t total_cycles = 0;
    uint64_t start = rdtsc();
    
    // 创建线程
    for (int i = 0; i < 8; i++) {
        thread_args[i].thread_id = i;
        thread_args[i].A = A;
        thread_args[i].out = out;
        thread_args[i].n = n;
        
        if (pthread_create(&threads[i], NULL, thread_handler, &thread_args[i]) != 0) {
            fprintf(stderr, "Error creating thread %d\n", i);
            // 处理错误
            return;
        }
    }
    
    // 等待所有线程完成
    for (int i = 0; i < 8; i++) {
        pthread_join(threads[i], NULL);
    }
    
    total_cycles += rdtsc() - start;
    printf("Cycles per operation: %ld\n", total_cycles);
    
    // 销毁屏障
    pthread_barrier_destroy(&barrier);
    
    // 8x8输入
    // 8x4输出
    for (int i = 0; i < 8/2; i++) {
        fprintf(stderr, "out[%d] = %d\n", i, *(out + i * 32));
    }
}