/* This files contains one suggestion for a simple register-interface to the
   matrix multiply accelerator. The numbers are the offsets (or in one case, bit
   in the register) for the corresponding registers.

   YOU ARE WELCOME TO CHANGE THIS HOWEVER YOU LIKE!
*/


/** Control register: used to start the accelerator/wait for completion.
    read/write */
#define REG_CTRL 0x00 // RW

/**
 * |Byte 7 |Byte 6|Byte 5 |Byte 4|Byte 3|Byte 2 |Byte 1|Byte 0|
 * |卡7写入|卡6写入|卡5写入|卡4写入|卡3写入|卡2写入|卡1写入|卡0写入|
 */

// IN在0x1000+:0x48
// OUT在0x2000+:0x40
#define REG_OFF_IN 0x10 // RO
#define REG_OFF_OUT 0x20 // RO
#define REG_TP_NUM 0x30 // RW


/** Register holding requested length of the DMA operation in bytes */ // 8B
#define REG_DMA_LEN  0x40 // RW
/** Register holding *physical* address on the host. */ // 8B
#define REG_DMA_ADDR_IN 0x48 // RW
// 8B
#define REG_DMA_ADDR_OUT 0x50 // RW

// 2B
#define REG_DMA_CTRL_IN 0x58 // RW
// 2B
#define REG_DMA_CTRL_OUT 0x5A // RO



// 每个卡28B，留空到32B
// 0x40-0x60 0x60-0x80 0x80-0xA0 0xA0-0xC0 0xC0-0xE0 0xE0-0x100 0x100-0x120 0x120-0x140
// 最大到0x140