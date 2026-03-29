// ### 1. The Four States of a Buffer

// | State | `valid` | `disk` | What it means |
// | :--- | :---: | :---: | :--- |
// | **1. Empty / Reused** | `0` | `0` | This buffer was just assigned a new block number, but the data hasn't been read from the disk yet. It contains garbage. |
// | **2. Fetching (Read)** | `0` | `1` | The kernel has asked the disk to read data into this buffer. The CPU is sleeping, waiting for the disk to finish. |
// | **3. Clean / Up-to-date**| `1` | `0` | The buffer contains perfectly valid data that matches the disk. No disk action is needed right now. |
// | **4. Dirty / Saving (Write)**| `1` | `1` | The buffer contains perfectly valid data (modified by a user), but the kernel is currently asking the disk to save it back to the physical platter. |
struct buf {
  int valid;   // indicates that the buffer contains a copy of the block
  int disk;    // Is the disk currently working on/"holding" the buffer?
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt; // A buffer can only be evicted/recycled if its refcnt is exactly 0
  struct buf *prev; // LRU cache list
  struct buf *next;
  uchar data[BSIZE];
};

