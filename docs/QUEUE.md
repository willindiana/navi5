# Play Next and the queue view (v1.0)

Triangle on a focused track, album, playlist or artist sends `Command::Enqueue`
to the media worker. The item plays after the current track. Repeated Play Next
keeps insertion order: `PlayQueue` counts the entries already "played next"
(`pinned`) and inserts behind them. Each advance consumes one; Previous, a wrap
or a full reshuffle forgets the rest. Live shuffle on or off leaves pinned
entries where they are and only reorders the tail behind them.

Nothing playing (stopped, finished or failed): the item starts playing. After a
finished queue it continues from the end of that queue.

## Assembly

A track uses the row already on screen; no request. Albums and playlists go
through `Library::queue` (detail path); artists through the same bounded
assembly as artist Play all: 256 albums, 20000 tracks, 16 MiB metadata, 60 s,
all-or-nothing ([ARTIST_QUEUE.md](ARTIST_QUEUE.md)). Assembly runs on its own
`spawn_media` thread with a scratch `Library`, so the worker keeps declaring
gapless successors and serving controls meanwhile; the result returns as an
internal `EnqueueReady` command. One assembly at a time (a second is refused
with a message; single tracks never wait). Play, Stop, login and logout cancel
it. Failures and cancelled assemblies add nothing. The queue holds at most
20000 occurrences; an insert that would exceed that is refused whole.

## Gapless safety

Edits (insert, move, remove) are collected and applied to a copy of the queue.

- Immediate successor changed: the copy is committed only if
  `AudioPlayer::replace_next` accepts the new URL for the current occurrence.
  That bumps `next_revision`, so the previously prefetched producer can never be
  promoted. Refusal (seek in progress, handoff, output not ready, terminal) leaves
  the edit waiting and retries each worker tick, exactly like live shuffle.
- Immediate successor unchanged: the prefetch is kept, and the copy is committed
  only while the player reports `track == next_track == occurrence`. During a
  handoff the promoted entry is not yet "current" for the queue, and an insert
  would slip in front of it.
- Next and Previous restart the decoder anyway, so waiting edits are applied
  first: Next lands on what was just asked to play next.

Known limit: an edit made in the short window after the next decoder was
promoted but before its first audio block (or while paused exactly there) waits
until that block is submitted. The view shows "Updating queue".

## Queue view

Fullscreen player, control row, Queue. It lists the current track and everything
after it. Cross picks up an upcoming track, Up/Down (L1/R1 by eight) carries
it, Cross drops it, Circle cancels the carry; Square removes the focused track;
Circle closes. The current track cannot be picked up or removed.

Entries are identified by a process-wide `uid`, not by position or song ID, so
duplicates are distinct and a stale command matches nothing. A move is rehearsed
in the view and sent once, on drop, as "put uid after anchor uid": one worker
edit and at most one successor swap however far it was carried. Without
shuffle, the new order becomes the canonical order; with shuffle on it changes
play order only, and turning shuffle off restores album order.

The worker publishes at most 48 rows around the cursor (`QueueWindow`), rebuilt
only when the queue, the requested offset or the pending flag changes, and only
while the view is open. The view lays out nine rows. Cost per frame does not
depend on queue length.

## Tests

`queue.sh`: cursor order with and without shuffle, cap, move/remove, pinned
bookkeeping, uids. `smoke.sh` (`enqueue_worker`): real worker with a fake
player: idle start, single `replace_next` per successor change, none when
unchanged, refused swap, handoff wait, album and artist assembly off the worker,
failure and cancellation adding nothing, Next with a waiting edit, finished
queue, window publication. `music.sh`: queue view carry/remove/busy states and a
20000-entry queue, Play Next targets per screen. Not covered on the host: real
decoder timing at a gapless boundary; see
[V1_HARDWARE_CHECKLIST.md](V1_HARDWARE_CHECKLIST.md).
