// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

// access buf will through DDL bcache.head, not bcache.buf array itself
struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head;
} bcache;

// Although we have the buf array buf[NBUF], but still need to link all bulk element together
// in a DDL is due to Performance: The LRU (Least Recently Used) Eviction Policy.
void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");
// Create linked list of buffers
`
//   Before Insertion:
// [Head] <==> [Node_1]
// Head.next points to Node_1.
// Node_1.prev points to Head.
// After Insertion (The Goal):
// [Head] <==> [B] <==> [Node_1]

//bcache.head is the sentinel node
  bcache.head.prev = &bcache.head;
  bcache.head.next = &bcache.head;
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    // four opts need to insert node b into the head of the list, next to sentinel node
    // b->next and b->prev connect node b INTO the list
    // <--b   AND  -->b  the top two opts
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    initsleeplock(&b->lock, "buffer");
    // two bcache.head.x opts re-connect sentinel to the new node b.
    // b<--previous sentinel.next  AND  sentinel-->b    the bottom two opts
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return **locked** buffer (acquiresleep(&b->lock)).
// Since the buffer is returned locked, no other process can get past that acquiresleep(&b->lock) line in bget. Any other process that wants this block will go to sleep. Therefore, the caller has 100% exclusive use of b->data.

// NOTE we have spinlock and sleeplock here
// bcache.lock is a spinlock. It protects the buffer cache data structure itself (the linked list and the refcnt, dev, blockno fields). Spinlocks are meant to be held for a very short time. You cannot yield the CPU (sleep) while holding a spinlock.
// b->lock is a sleep-lock. It protects the actual contents (the data payload) of that specific buffer.The sleep-lock (b->lock) acts as a strict "one person in the room at a time" door.
// By convention in the xv6 kernel, no code is allowed to look at or change b->data unless it is currently holding sleep-lock b->lock.
// If b->lock were a normal spinlock, the second process would sit there spinning in a loop, wasting millions of CPU cycles waiting for the hard drive.
// By using acquiresleep(), if the lock is already held by another process, our process will go to sleep and yield the CPU to other programs until the other process finishes its disk I/O and releases the lock.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  // NOTE this needs to be held during the two loops to make sure atomic, because at most one cached buffer per disk sector
  acquire(&bcache.lock);

  // Is the block already cached?
  for(b = bcache.head.next; b != &bcache.head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      // order is IMPORTANT, Never sleep holding a spinlock!
      // acquiresleep(&b->lock), your process might have to go to sleep (context switch to another program) because another process is currently writing to that block on the hard drive.
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
    // A buffer can only be evicted/recycled if its refcnt is exactly 0
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      // Because the memory buffer contains old "garbage" data that does not belong to the newly requested blockno, we must set b->valid = 0
      // Setting valid = 0 in bget acts as a signal to later bread. It says: "I found an empty box for your block, but the box is empty. You need to actually go to the hard drive and fetch the data to fill it."
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  // Setting valid = 0 in bget acts as a signal to bread. It says: "I found an empty box for your block, but the box is empty. You need to actually go to the hard drive and fetch the data to fill it."
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  // here proves sleep-lock protect reads and writes of the block’s buffered content
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Because the caller received a locked buffer via bget, the caller is absolutely required to unlock it when they are finished reading or writing the data.
// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  // here proves sleep-lock protect reads and writes of the block’s buffered content
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  acquire(&bcache.lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    // Remove b from its current spot (2 operations)
    b->next->prev = b->prev;
    b->prev->next = b->next;
    // Insert b at the front of the list, after sentinel node, same as binit (4 operations)
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
  
  release(&bcache.lock);
}

void
bpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt++;
  release(&bcache.lock);
}

void
bunpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt--;
  release(&bcache.lock);
}


