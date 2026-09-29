# Memory regressions

The guest heap and the memory bridge, checked against the bugs they had. Like
`tests/kernel_bridge`, it needs no title and no game files: it runs the real
memory layout on the small synthetic XBE from `tools/conformance` and calls the
kernel through the thunk dispatcher.

```
cmake -S tests/memory_regressions -B build/memory-regressions -A x64
cmake --build build/memory-regressions --config Release
ctest --test-dir build/memory-regressions -C Release --output-on-failure
```

Two tests, one process each, because the switches are read once and cached:

| Test | What it checks |
|---|---|
| `memory_regressions_default` | No switch set. Pins what every title gets today: reuse hands a freed block over whole. A change to the default heap fails here. |
| `memory_regressions_heap_reclaim` | `RECOMP_HEAP_RECLAIM=1`. The checks below. |

## `RECOMP_HEAP_RECLAIM`

Each check fails without the change and passes with it.

| Check | Without it |
|---|---|
| A 16-byte request after a 2 MB free takes 16 bytes | The whole 2 MB block, so a few thousand small requests drain the heap. |
| The rest of that block is still available | Not reachable: it went with the first request. |
| Freeing three neighbours in the order first, second, third merges all three | The third never joins: the merge of the first two left an empty slot that the neighbour search stopped at. |
| `NtFreeVirtualMemory(MEM_RELEASE)` on memory the heap supplied returns it | `STATUS_UNSUCCESSFUL`. The 32-bit guest slot went to the host `VirtualFree` as a pointer, so nothing came back. |

## What it does not cover

Titles. It shows the allocator does what it should when the switch is set and
does what it did before when it is not; it does not show any particular title
runs better with it. Which titles free enough for the switch to matter, and
whether a title's addresses matter to it, is a per-title question.

The block table's lock is not exercised: nothing here allocates from two
threads.
