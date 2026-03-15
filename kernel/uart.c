//
// low-level driver routines for 16550a UART.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

// the UART control registers are memory-mapped
// at address UART0. this macro returns the
// address of one of the registers.
#define Reg(reg) ((volatile unsigned char *)(UART0 + reg))

// the UART control registers.
// some have different meanings for
// read vs write.
// see http://byterunner.com/16550.html
#define RHR 0                 // receive holding register (for input bytes)
#define THR 0                 // transmit holding register (for output bytes)
#define IER 1                 // interrupt enable register
#define IER_TX_ENABLE (1<<0)
#define IER_RX_ENABLE (1<<1)
#define FCR 2                 // FIFO control register
#define FCR_FIFO_ENABLE (1<<0)
#define FCR_FIFO_CLEAR (3<<1) // clear the content of the two FIFOs
#define ISR 2                 // interrupt status register
#define LCR 3                 // line control register
#define LCR_EIGHT_BITS (3<<0)
#define LCR_BAUD_LATCH (1<<7) // special mode to set baud rate
#define LSR 5                 // line status register
#define LSR_RX_READY (1<<0)   // input is waiting to be read from RHR
#define LSR_TX_IDLE (1<<5)    // THR can accept another character to send

#define ReadReg(reg) (*(Reg(reg)))
#define WriteReg(reg, v) (*(Reg(reg)) = (v))

// the transmit output buffer.
struct spinlock uart_tx_lock;
#define UART_TX_BUF_SIZE 32
char uart_tx_buf[UART_TX_BUF_SIZE];
int uart_tx_w; // write next to uart_tx_buf[uart_tx_w++]
int uart_tx_r; // read next from uart_tx_buf[uar_tx_r++]

extern volatile int panicked; // from printf.c

void uartstart();

void
uartinit(void)
{
  // disable interrupts.
  WriteReg(IER, 0x00);

  // special mode to set baud rate.
  WriteReg(LCR, LCR_BAUD_LATCH);

  // LSB for baud rate of 38.4K.
  WriteReg(0, 0x03);

  // MSB for baud rate of 38.4K.
  WriteReg(1, 0x00);

  // leave set-baud mode,
  // and set word length to 8 bits, no parity.
  WriteReg(LCR, LCR_EIGHT_BITS);

  // reset and enable FIFOs.
  WriteReg(FCR, FCR_FIFO_ENABLE | FCR_FIFO_CLEAR);

  // enable transmit and receive interrupts.
  WriteReg(IER, IER_TX_ENABLE | IER_RX_ENABLE);

  initlock(&uart_tx_lock, "uart");
}

// add a character to the output buffer and tell the
// UART to start sending if it isn't already.
// blocks if the output buffer is full.
// because it may block, it can't be called
// from interrupts; it's only suitable for use
// by write().
//  代码开头的注释写着 "because it may block, it can't be called from interrupts"（因为它可能会阻塞/睡觉，所以绝对不能在中断里被调用）。这正是进程上下文的铁律。
void
uartputc(int c)
{
  acquire(&uart_tx_lock);

  if(panicked){
    for(;;)
      ;
  }

  while(1){
    if(((uart_tx_w + 1) % UART_TX_BUF_SIZE) == uart_tx_r){
      // buffer is full.
      // wait for uartstart() to open up space in the buffer.
      // // 3. 满了！厨师没地方放菜了。
      // 核心动作：睡觉 (sleep)。交出 CPU，直到下半部取走菜并唤醒我。
      // sleep 和 wakeup 机制的设计哲学中：你睡在什么“事件/条件”上，就用代表那个“事件/条件”的变量地址作为“床位号 (Channel)”。 it sleeps on the uart_tx_r, and release the lock before it sleeps to hand out CPUs.
      // 写指针 uart_tx_w 只有厨师自己能动。厨师在睡觉，写指针绝对不会动。唯一能让 Buffer 腾出空位的人，是外卖小哥（硬件/下半部）。
      sleep(&uart_tx_r, &uart_tx_lock);
    } else {
      // 4. 没满！把菜放到写指针的位置。
      uart_tx_buf[uart_tx_w] = c;
      uart_tx_w = (uart_tx_w + 1) % UART_TX_BUF_SIZE;
      // 5. 关键：尝试把菜递给食客（喊一声搬运工）
      uartstart();
      release(&uart_tx_lock);
      return;
    }
  }
}

// alternate version of uartputc() that doesn't 
// use interrupts, for use by kernel printf() and
// to echo characters. it spins waiting for the uart's
// output register to be empty.
void
uartputc_sync(int c)
{
  push_off();

  if(panicked){
    for(;;)
      ;
  }

  // wait for Transmit Holding Empty to be set in LSR.
  while((ReadReg(LSR) & LSR_TX_IDLE) == 0)
    ;
  WriteReg(THR, c);

  pop_off();
}

// if the UART is idle, and a character is waiting
// in the transmit buffer, send it.
// caller must hold uart_tx_lock.
// IMPORTANT called from both the top- and bottom-half.
// 不生产数据，也不凭空产生数据，它只负责检查：“内存里有货吗？” + “硬件闲着吗？”，如果都满足，就搬运。
void
uartstart()
{
  while(1){
    // 1. 检查：取餐台空了吗？
    if(uart_tx_w == uart_tx_r){
      // transmit buffer is empty.
      return;
    }
    // 2. 如果 LSR_TX_IDLE 位是 0，说明硬件嘴里还在嚼上一个字符，正忙着发呢！
    if((ReadReg(LSR) & LSR_TX_IDLE) == 0){
      // the UART transmit holding register is full,
      // so we cannot give it another byte.
      // it will interrupt when it's ready for a new byte.
      // 硬件忙！搬运工绝不死等，立刻撤退。(解耦的核心体现)
      return;
    }
    // 3. 硬件闲着，且内存有货！开始搬运。
    // 从读指针的位置拿出一盘菜。
    int c = uart_tx_buf[uart_tx_r];
    uart_tx_r = (uart_tx_r + 1) % UART_TX_BUF_SIZE;
    
    // maybe uartputc() is waiting for space in the buffer.
    // any process that sleep on the read channel id should wakeup!
    // 4. 通知：如果有厨师因为取餐台满了在睡觉，现在腾出空地了，叫醒他！
    // sleep 和 wakeup 机制的设计哲学中：你睡在什么“事件/条件”上，就用代表那个“事件/条件”的变量地址作为“床位号 (Channel)”。
    // 写指针 uart_tx_w 只有厨师自己能动。厨师在睡觉，写指针绝对不会动。唯一能让 Buffer 腾出空位的人，是外卖小哥（硬件/下半部）。
    wakeup(&uart_tx_r);
    // 5. 【真正触碰硬件】
    // 把菜硬塞进硬件的嘴里 (THR 发送寄存器)。
    // 只要一执行这行，硬件的电线就开始闪烁发送信号了。
    WriteReg(THR, c);
    // 注意：这里没有 break，循环会立刻上去再检查硬件是不是闲着。
    // 必然是不闲了（因为刚塞了一个进去），所以下一圈会在检查 2 处 return。
  }
}

// read one input character from the UART.
// return -1 if none is waiting.
int
uartgetc(void)
{
  if(ReadReg(LSR) & 0x01){
    // input data is ready.
    return ReadReg(RHR);
  } else {
    return -1;
  }
}

// handle a uart interrupt, raised because input has
// arrived, or the uart is ready for more output, or
// both. called from trap.c.
// 触发者： 它是被 trap.c 里的硬件中断机制强行调用的。
// 特征： 它绝对不敢调用 sleep()。它必须以最快速度处理完硬件的“抱怨”然后滚蛋。
// 它要处理两件事：硬件说“我收到外面传来的字符了”（输入），或者硬件说“我把刚才那个字符发完了”（输出）。
// 生产者被动响应 (Bottom Half)：uartintr -> consoleintr
void
uartintr(void)
{
  // read and process incoming characters.
  while(1){
    // 去问硬件的接收寄存器 (RHR) 要字符
    int c = uartgetc();
    if(c == -1)
      break;
    // 硬件确实收到了字符！(比如你敲了键盘的 'A')
    // 把它丢给更上层的软件层 (Console) 去处理。
    // Console 会把它存进输入 Buffer，并可能唤醒正在等输入的进程。
    consoleintr(c);
  }

  // send buffered characters.
  // --- 第二部分：处理输出 (Tx) ---
  // 这部分对应输出方向的下半部。
  // 硬件发出中断，说明它“刚刚发完了上一个字符，现在闲下来了”。
  acquire(&uart_tx_lock);
  // 关键：呼叫搬运工！
  // 搬运工 uartstart 进去后，会发现硬件确实闲了，如果内存 Buffer 里还有积压的字符，
  // 搬运工就会继续往硬件里塞下一个字符。
  uartstart();
  release(&uart_tx_lock);
}
