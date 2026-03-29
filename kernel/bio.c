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

#define NBUCKET 13 // a prime number to better avoid hash collision
#define BUCKET_HASH(blockno) (blockno % NBUCKET)

// access buf will through DDL bcache.head, not bcache.buf array itself
// NOTE: lab the eviction_lock is to protect the whole bache bucket struct that only atomic evicting at a time
// the bucket_lock is to protect each bucket ref_cnt, dev, blockno fields, whenever we change those fields, we need to acquire bucket_lock
// Although eviction_lock is broader, but it can not protect ref_cnt, e.g. process A hold eviction_lock during eviction can ref_cnt++, but process holds bucket_lock[i] can ref_cnt++ at the same time!
struct {
  // global eviction_lock for the whole bcache structure
  struct spinlock eviction_lock;
  struct buf buf[NBUF];

  // lab we need to add bucket array for LRU, and bucket lock array
  // IMPORTANT bucket[i] is just a * pointer to the first node. The actual objects still live in buf[].
  // With the old design, brelse needed to move nodes to the front of the list constantly (LRU ordering). That's where sentinel made insertion elegant.
  // With the new design:
  // LRU is tracked by timestamp number, not by position in list, so the only operation we need to do is to insert new node to the head of the bucket list and update timestamp
  // The only time a node moves is during eviction in bget
  // So the complexity of sentinel buys you nothing here. NULL-terminated is simpler.
  struct buf* bucket[NBUCKET];
  struct spinlock bucket_lock[NBUCKET];

  // lab Remove the list of all buffers (bcache.head etc.) and instead time-stamp buffers using the time of their last use (i.e., using ticks in kernel/trap.c).
  // // Linked list of all buffers, through prev/next.
  // // Sorted by how recently the buffer was used.
  // // head.next is most recent, head.prev is least.
  // struct buf head;
} bcache;

// lab lock
void
binit(void)
{
  // we need to init both eviction and bucket locks
  struct buf *b;
  initlock(&bcache.eviction_lock, "bcache");
  for(int i = 0; i < NBUCKET; i++){
    initlock(&bcache.bucket_lock[i], "bcache_bucket");
    // init the bucket
    bcache.bucket[i] = 0;
  }
  // now we need to init all buf in the buf array into bucket
  // At boot time, all buffers have blockno = 0, dev = 0, refcnt = 0, it deosnt matter which bucket it belongs to, so just put in bucket 0!!
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    // INSERT AT HEAD!
    // after buf[29]: bucket[0] -> buf[29] -> buf[28] -> ... -> buf[0] -> NULL
    b->next = bcache.bucket[0];
    bcache.bucket[0] = b;
    initsleeplock(&b->lock, "buffer");
  }
}

/* LAB comment out
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
*/

static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  // PART 1
  // Calculate the bucket index for blockno
  int idx = BUCKET_HASH(blockno);
  // Lock that bucket
  acquire(&bcache.bucket_lock[idx]);
  for(b = bcache.bucket[idx]; b != 0; b = b->next){
    // Scan the chain looking for dev == dev && blockno == blockno
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      // order is IMPORTANT, Never sleep holding a spinlock!
      // acquiresleep(&b->lock), your process might have to go to sleep (context switch to another program) because another process is currently writing to that block on the hard drive.
      // If found, increment refcnt, release bucket lock, acquire sleeplock, return
      release(&bcache.bucket_lock[idx]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // at this stage, we dont have a cache hit
  // PART 2
  // Need to evict a buffer from the cache to make space for the new block
  // Eviction = stealing a free buffer (refcnt == 0) to use for the new block.
  // It does NOT mean the cache is full. It just means the block wasn't found in cache, so you need a free buffer to load it into.

  // BUG: the order is important, if we 1st acquire, then release, we will have the issue, process A holds bucket lock and wait for eviction lock. process B has eviction lock, and lru scan needs bucket lock, deadlock!!
  release(&bcache.bucket_lock[idx]);
  // Acquire eviction_lock to prevent two process entering miss for the same blockno
  acquire(&bcache.eviction_lock);
  // re-check bucket[idx] — only one process can get acquire eviction_lock to evict and reassign. This re-check make sure,
  // even if two process entering miss for the same blockno, the 2nd will not pass if re-check
  // since we have eviction_lock, no bucket_lock for now
  for(b = bcache.bucket[idx]; b != 0; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      // b->refcnt++ must be protected!
      acquire(&bcache.bucket_lock[idx]);
      b->refcnt++;
      release(&bcache.bucket_lock[idx]);
      release(&bcache.eviction_lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // scan ALL buckets for LRU (refcnt==0, smallest timestamp)
  struct buf *lru = 0;      // best candidate so far
  uint min_time = -1;       // smallest timestamp so far (since unsigned int, -1 means 4294967295)
  int lru_bucket = -1;      // which bucket lru lives in
  for (int i = 0; i < NBUCKET; i++) {
    // before acquiring, we are either holding nothing, or only holding locks of
    // buckets that are *on the left side* of the current bucket
    // so no circular wait can ever happen here. (safe from deadlock)
    acquire(&bcache.bucket_lock[i]);
    for(b = bcache.bucket[i]; b != 0; b = b->next){
      // find a free buffer (refcnt == 0)
      if(b->refcnt == 0 && b->timestamp < min_time){
        min_time = b->timestamp;
        lru = b;
        lru_bucket = i;
      }
    }
    release(&bcache.bucket_lock[i]);
    // we finish scan bucket[i] may or may not find 
  }

  // if lru is still 0, it means no buffer is available (all refcnt > 0)
  if(lru == 0)
    panic("bget: no buffers");

  // second pass: find predecessor of lru in bucket[lru_bucket] so that we can remove lru buf from the bucket, before insert into the correct bucket idx
  // one tricky case before we remove is that what if lru_bucket == idx, in this case, we dont need to remove at all!
  if(lru_bucket == idx){
      acquire(&bcache.bucket_lock[idx]);
      // no remove/reinsert needed, lru already in correct bucket
  } else {
      acquire(&bcache.bucket_lock[lru_bucket]);
      // find prev and remove lru from bucket[lru_bucket]
      struct buf *prev = 0;
      for(b = bcache.bucket[lru_bucket]; b != lru; b = b->next)
          prev = b;
      if(prev == 0)
          bcache.bucket[lru_bucket] = lru->next;  // lru was head
      else
          prev->next = lru->next;                  // lru was middle/tail
      release(&bcache.bucket_lock[lru_bucket]);
      acquire(&bcache.bucket_lock[idx]);
      // insert lru into bucket[idx] head
      lru->next = bcache.bucket[idx];
      bcache.bucket[idx] = lru;
  }
  // set fields
  lru->dev = dev;
  lru->blockno = blockno;
  lru->valid = 0; // fresh field need data from disk
  lru->refcnt = 1; // increment refcnt
  release(&bcache.bucket_lock[idx]);
  release(&bcache.eviction_lock);
  acquiresleep(&lru->lock);
  return lru;
  
}

/* LAB COMMENT OUT
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
*/

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

// Hash to get index
// Release sleeplock
// Acquire bucket lock
// refcnt--
// If refcnt == 0, set b->timestamp = ticks ← this is all you need!
// Release bucket lock
void
brelse(struct buf *b)
{
  int idx = BUCKET_HASH(b->blockno);
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);
  acquire(&bcache.bucket_lock[idx]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // this is about to be evict soon, update the ticks when current time when refcnt == 0
    b->timestamp = ticks;
  }
  release(&bcache.bucket_lock[idx]);
}

void bpin(struct buf *b) {
  int idx = BUCKET_HASH(b->blockno);
  acquire(&bcache.bucket_lock[idx]);
  b->refcnt++;
  release(&bcache.bucket_lock[idx]);
}

void bunpin(struct buf *b) {
  int idx = BUCKET_HASH(b->blockno);
  acquire(&bcache.bucket_lock[idx]);
  b->refcnt--;
  release(&bcache.bucket_lock[idx]);
}

/* LAB COMMENT
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
*/

