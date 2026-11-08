# Renderer

Nov 7, 2026. The sprite renderer after slice 3: what lives on the GPU, what a frame does,
and the rules the pieces follow.

## Principle

**The CPU never visits a sprite to draw a frame.** `renderer_apply` writes rows into a
persistent table as deltas arrive; a frame copies the rows that changed, lets compute sort
and cull the table, and records one indirect draw per run of buckets it knows to be
non-empty. A frame in which nothing moved uploads nothing and draws from the same sorted
list it would build again.

## The tables

- **Sprite rows** (`GpuSprite`, 96 bytes, device-local): slot-addressed by the render
  world's pool index, so `sprite_of_entity` is the only lookup and the CPU mirror
  (`Pool<RenderSprite>`) and the GPU row share an index. A row holds the previous and
  current placement, the tick stamp of its last `Set`, material row, uv, size, tint,
  layer and a live flag. The table grows by doubling (every row re-marked dirty, the old
  buffers retired through the garbage queue).
- **Material rows** (`GpuMaterial`, 64 bytes): tint, two float4 parameters, the bindless
  texture slot resolved when the row is written, the blend mode. The handle's index is
  the row; row 0 is the default material. A dirty bitmap over the rows drives uploads.
- **Sort scratch**: `bucket_of[row]`, `block_hist[block][bucket]`, `sort_meta` (bucket
  totals, bucket starts, the visible count), `indirect[bucket]`
  (`VkDrawIndirectCommand`), `sorted[]` (row indices in draw order).

## Ticks without a copy

The `Tick` delta increments a counter and touches no row. A `Set` on a row stamped with
an earlier tick first moves the row's current placement into its previous one, then
writes and stamps it. The vertex shader and the cull interpolate rows stamped with the
frame's tick by the view's alpha and show the rest at their current placement: a row that
was not set this tick did not move. Only rows that were set are dirty, so the upload is
proportional to what moved.

## Dirty rows

A bit per row. The frame walks the words, joins runs separated by fewer than 32 clean
rows (uploading a clean row again is cheaper than another copy region), stages each run
into the frame ring and records one `vkCmdCopyBuffer` per run. A dead slot in a run is
written as a zero row.

## The sort

A sprite's bucket is `(clamp(layer, -128, 127) + 128) * 3 + blend`: 768 buckets, layer
major, so ascending bucket order is draw order. The CPU keeps an exact count per bucket,
updated on `Set` and `Remove` (and when a material's blend changes, by walking its
sprites), so it knows the non-empty buckets without looking at rows.

Four compute passes over blocks of 1024 rows (256 threads, 4 rows each):

1. **count**: cull each live row's bounding circle against the view rect, write its
   bucket (or "culled") to `bucket_of`, build the block's histogram in shared memory and
   write it to `block_hist`.
2. **scan**: one thread per bucket runs over the blocks, turning counts into each
   block's offset within the bucket and producing the bucket's total.
3. **bucket scan**: one group scans the 768 totals into bucket starts and writes an
   indirect command per bucket: 6 vertices, the visible count, the bucket's start as
   `firstInstance`.
4. **scatter**: per block, the local histogram and its scan give each bucket's first
   local rank; a bitonic sort of the block's `(bucket << 10 | local index)` keys makes
   rows of one bucket contiguous and in row order; each row lands at
   `bucket start + block offset + rank`.

Order within a bucket is row order, every frame, so overlapping sprites on one layer
never swap, unless the bucket holds keyed sprites (slice 5). The CPU counts keyed rows per
bucket next to the plain counts; after the scatter, a fifth pass, **cs_key_sort**, runs one
group per bucket with keyed rows (the bucket in the push block's `key_bucket`): it loads
the bucket's range of `sorted[]` (up to 2,048 rows, 16 KB of shared memory), computes each
row's key (`Key`: its `order`; `Feet`: minus the interpolated y plus `order`; `Row`: 0) as
an order-preserving uint, bitonic-sorts (key, row) pairs and writes the rows back. Ties are
broken by row, so the result is as stable as the scatter's. A bucket past 2,048 visible
rows sorts its first 2,048. `--check-sort` snapshots every row's key on the CPU when it
records the check and verifies (key, row) order, allowing near ties either way. `--check-sort` reads the outputs back every 30th frame and verifies them.

## The draws

The CPU walks its bucket counts and records a `vkCmdDrawIndirect` per run: from a
non-empty bucket through the last non-empty bucket of the same blend before one of
another blend intervenes, with one pipeline bind. Buckets in between with nothing in them
draw zero instances. Without `multiDrawIndirect` it is one call per non-empty bucket.
The vertex shader reads `sorted[base + instance]`; Slang's `SV_InstanceID` excludes the
base instance, so `SV_StartInstanceLocation` adds it back.

## The frame ring

One host-visible buffer. A frame's allocations are contiguous in ring order even when
they begin before the previous frame's fence passes (texture uploads land from the asset
pump, before the frame starts); releasing a frame moves the tail to where that frame
ended. `used` counts the bytes in flight with wrap gaps, so head == tail is unambiguous.
A frame that took nothing leaves the tail alone. Growth retires the old buffer through
the garbage queue; spans already handed out keep pointing at it, which is why a span
carries its buffer.

## Textures

A texture upload creates the image at once, stages the pixels into the ring (or a one-off
staging buffer, retired with the frame, when they would not fit in half the ring) and
queues a `PendingUpload`. The next frame records the copy and its barriers ahead of
everything else. A slot's descriptor is rewritten in each frame's set once that frame's
fence has passed. There is no upload command buffer and no upload fence.

## Fences

Two frames in flight, one fence each. Everything deferred (buffers, images, views,
memory, swapchains, semaphores, pipelines) is queued on the frame being recorded and
destroyed when that frame's fence passes; fences on one queue pass in submission order,
so a frame's fence covers what earlier frames referenced too. Swapchain recreation builds
the new chain with `oldSwapchain` set and retires the old one the same way. The only
`vkDeviceWaitIdle` is in `renderer_destroy`.

## Dev facilities

`--screenshot FILE` copies one presented frame to a host buffer and writes a binary PPM;
`--check-sort` verifies the sort. Both wait on their frame's fence. The Mesa screenshot
layer crashes on the first acquire after a fenced recreation (it does not expect the old
swapchain to outlive the new one's creation); the built-in path replaces it.

## Samplers

Set 1 holds two samplers, nearest and linear (slice 5), and the material row's `filter`
picks one per draw: the vertex shader passes it flat to the fragment shader, which samples
`gSamplers[filter]`. Pixel art stays on nearest; font atlases and anything drawn at a scale
of its own use linear.
