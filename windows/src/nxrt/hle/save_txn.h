// The save commit on NTFS (DESIGN D13): nxnative's transactional save swaps a clone of the work directory into place with APFS's atomic RENAME_SWAP; NTFS has no
// directory swap, so the commit is a protocol a later start can finish or undo:
//   1. copy the work directory tree to <save>.new and flush every file;
//   2. write the commit marker <save>.commit (write-through, flushed): from here .new is a complete version;
//   3. rename <save> → <save>.old and <save>.new → <save> (MoveFileEx, write-through);
//   4. delete the marker: the commit is done; then delete <save>.old.
// save_txn_recover, called before a save is mounted: with the marker, .new is complete, so the commit is finished (roll forward, whatever step the process died
// in); without it, a .new is a copy that never completed and is deleted, and a missing <save> with an .old beside it is put back (roll back). Either way the save
// is the old version or the new one, never a mix. The save keeps the game's own layout (B00 / G00 / S00 under <save>).
#pragma once

int save_txn_commit(const char *work, const char *save);   // 0, or -1 with errno; on failure the save is the old version
int save_txn_recover(const char *save);                    // 0: nothing to do; 1: rolled forward; 2: rolled back (or an incomplete copy cleared); -1 with errno
int save_txn_copy_tree(const char *from, const char *to);  // the tree copy with every file flushed (streams kept: the guest names, hle_fs.c)
int save_txn_rm_tree(const char *p);
// Kill tests (tests/test_save.c): NXRT_SAVE_KILL_AT=<point>[:<n>] terminates the process at the n-th time (default 1) it reaches that point of save_txn_commit:
// copied (after each file of the copy), flushed, marker, renamed_old, renamed_new, unmarked, done
