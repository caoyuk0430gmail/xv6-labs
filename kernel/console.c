//
// Console input and output, to the uart.
// Reads are line at a time.
// Implements special input characters:
//   newline -- end of line
//   control-h -- backspace
//   control-u -- kill line
//   control-d -- end of file
//   control-p -- print process list
//

#include <stdarg.h>

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

#define BACKSPACE 0x100
#define C(x)  ((x)-'@')  // Control-x

//
// send one character to the uart.
// called by printf, and to echo input characters,
// but not from write().
//
void
consputc(int c)
{
  if(c == BACKSPACE){
    // if the user typed backspace, overwrite with a space.
    uartputc_sync('\b'); uartputc_sync(' '); uartputc_sync('\b');
  } else {
    uartputc_sync(c);
  }
}

struct {
  struct spinlock lock;
  
  // input
#define INPUT_BUF 128
  char buf[INPUT_BUF];
  uint r;  // Read index
  uint w;  // Write index
  uint e;  // Edit index
} cons;

//
// user write()s to the console go here.
//
int
consolewrite(int user_src, uint64 src, int n)
{
  int i;

  acquire(&cons.lock);
  for(i = 0; i < n; i++){
    char c;
    if(either_copyin(&c, user_src, src+i, 1) == -1)
      break;
    uartputc(c);
  }
  release(&cons.lock);

  return i;
}

// 消费者主动索取 (Top Half)：consoleread Shell 停在那里等你敲命令，其实它正在循环调用 read(0, ...)。这个系统调用最终会进入到 console.c 的 consoleread 函数。
// user read()s from the console go here.
// copy (up to) a whole input line to dst.
// user_dist indicates whether dst is a user
// or kernel address.
// return val: consoleread 把拿到的字符串拷贝给用户空间的 Shell。Shell 拿到完整的命令，开始解析执行（比如执行 ls）。
int
consoleread(int user_dst, uint64 dst, int n)
{
  uint target;
  int c;
  char cbuf;

  target = n;
  acquire(&cons.lock);
  while(n > 0){
    // wait until interrupt handler has put some
    // input into cons.buffer.
    // 【核心解耦点 1：检查是否有数据】
    // 读指针 (r) 等于 写指针 (w)，说明取餐台 (cons.buf) 是空的。
    // 注意：这里用的是 w (Write index，表示已经确认写入的完整行)，不是 e (Edit index)。
    while(cons.r == cons.w){
      if(myproc()->killed){
        release(&cons.lock);
        return -1;
      }
      // 【核心解耦点 2：不死等，主动睡觉】
      // Shell 发现没敲击任何键盘。它绝对不在这里空转！
      // 它选择睡在 &cons.r 这个床位号上，把 CPU 让给其他进程。
      // 潜台词：“等有人（下半部）往 cons.buf 里塞了数据，再叫醒我。”
      sleep(&cons.r, &cons.lock);
    }
     // ... 如果醒了，或者本来就有数据，就从 cons.buf[cons.r] 读走数据拷贝给用户。
    // 从 r 指针处拿走 'A'，r 前进一格
    c = cons.buf[cons.r++ % INPUT_BUF];

    if(c == C('D')){  // end-of-file
      if(n < target){
        // Save ^D for next time, to make sure
        // caller gets a 0-byte result.
        cons.r--;
      }
      break;
    }

    // copy the input byte to the user-space buffer.
    cbuf = c;
    if(either_copyout(user_dst, dst, &cbuf, 1) == -1)
      break;

    dst++;
    --n;

    if(c == '\n'){
      // a whole line has arrived, return to
      // the user-level read().
      break;
    }
  }
  release(&cons.lock);

  return target - n;
}

// 唤醒consumer, the shell. 大取餐台的生产者
// the console input interrupt handler.
// uartintr() calls this for input character.
// do erase/kill processing, append to cons.buf,
// wake up consoleread() if a whole line has arrived.
//
void
consoleintr(int c)
{
  acquire(&cons.lock);

  switch(c){
  case C('P'):  // Print process list.
    procdump();
    break;
  case C('U'):  // Kill line.
    while(cons.e != cons.w &&
          cons.buf[(cons.e-1) % INPUT_BUF] != '\n'){
      cons.e--;
      consputc(BACKSPACE);
    }
    break;
  case C('H'): // Backspace
  case '\x7f':
    if(cons.e != cons.w){
      cons.e--;
      consputc(BACKSPACE);
    }
    break;
  default:
    if(c != 0 && cons.e-cons.r < INPUT_BUF){
      c = (c == '\r') ? '\n' : c;

      // echo back to the user.
      // 1. 回显 (Echo)：你敲了 'A'，屏幕上得立刻显示 'A' 呀！
      // 直接调用底层直接发送函数（不经过缓冲），让你立刻看到你敲的字。
      consputc(c);

      // store for consumption by consoleread().
      // 2. 【核心生产动作：存入 Buffer】
      // 把 'A' 存入编辑指针 (e) 的位置，编辑指针前进一格。
      // 为什么用 e？因为 xv6 允许你敲退格键修改还没按回车的输入。
      cons.buf[cons.e++ % INPUT_BUF] = c;

      // 3. 【核心唤醒动作：判断是否可以叫醒消费者】
      // xv6 的控制台是 "行缓冲 (Line-Buffered)" 的。
      // 只有当你按了回车 ('\n')，或者按了 EOF (Ctrl+D)，或者 Buffer 彻底满了，
      // 它才认为“这一批外卖”打包好了。
      if(c == '\n' || c == C('D') || cons.e == cons.r+INPUT_BUF){
        // wake up consoleread() if a whole line (or end-of-file)
        // has arrived.
         // 确认打包完成：把写指针 (w) 挪到编辑指针 (e) 的位置。
        // 这意味着 cons.w 变了！回忆 consoleread 中的 while(cons.r == cons.w)。
        // 这一步打破了那个死循环的条件！
        cons.w = cons.e;
        // 既然 w 变了，说明有完整的一行数据可以读了。
        // 去叫醒那个因为没数据而睡在 &cons.r 上的进程 (即 Shell)！
        wakeup(&cons.r);
      }
    }
    break;
  }
  
  release(&cons.lock);
}

void
consoleinit(void)
{
  initlock(&cons.lock, "cons");

  uartinit();

  // connect read and write system calls
  // to consoleread and consolewrite.
  devsw[CONSOLE].read = consoleread;
  devsw[CONSOLE].write = consolewrite;
}
