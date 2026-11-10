// Korvid renderer-vulkan: the sort. A counting sort of the live, visible sprite rows into
// buckets of (layer, pipeline slot), stable by row within a bucket, in four passes:
//   cs_count        per block of 1024 rows: cull, bucket, block histogram
//   cs_scan         per bucket: running sum over blocks (block offsets), bucket totals
//   cs_bucket_scan  one group: exclusive scan of the totals (bucket starts), indirect draws
//   cs_scatter      per block: local histogram and scan, a bitonic sort of (bucket, row)
//                   keys so equal buckets come out in row order, then the final index
// The vertex shader reads gSorted[instance]; the CPU draws the buckets it knows to be
// non-empty with the counts and offsets written here.
#include "sprite_common.slangh"

[[vk::binding(0, 0)]] StructuredBuffer<GpuSprite>   gSprites;
[[vk::binding(1, 0)]] StructuredBuffer<GpuMaterial> gMaterials;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint>      gSorted;    // row indices in draw order
[[vk::binding(3, 0)]] RWStructuredBuffer<uint>      gBucketOf;  // per row: bucket or kCulled
[[vk::binding(4, 0)]] RWStructuredBuffer<uint>      gBlockHist; // [block][bucket]: counts, then offsets
[[vk::binding(5, 0)]] RWStructuredBuffer<uint>      gSortMeta;  // [0, 768) totals, [768, 1536) starts, [1536] visible
[[vk::binding(6, 0)]] RWStructuredBuffer<uint4>     gIndirect;  // VkDrawIndirectCommand per bucket

static const uint kStarts = kBucketCount;
static const uint kVisible = 2 * kBucketCount;

groupshared uint sHist[kBucketCount];
groupshared uint sScan[kGroup];
groupshared uint sKeys[kBlockRows];

// The bucket of a row, or kCulled when dead, out of range, or outside the view.
uint bucket_of_row(uint i) {
    if (i >= gPush.row_count) return kCulled;
    GpuSprite s = gSprites[i];
    if ((s.flags & kSpriteAlive) == 0) return kCulled;
    float2 pos; float rot; float2 scale;
    row_placement(s, pos, rot, scale);
    float radius = 0.5 * length(abs(scale) * s.size); // the quad's bounding circle
    float4 c = row_cull(s);
    if (pos.x + radius < c.x || pos.x - radius > c.x + c.z ||
        pos.y + radius < c.y || pos.y - radius > c.y + c.w) return kCulled;
    int  layer = clamp(s.layer, -128, 127);
    uint slot  = min(gMaterials[s.material].slot, kSlotCount - 1);
    return uint(layer + int(kLayerMin)) * kSlotCount + slot;
}

// Exclusive scan of sHist in place. Thread t owns entries kPerThread * t onward.
void scan_hist(uint t) {
    uint a[kPerThread];
    uint sum = 0;
    for (uint j = 0; j < kPerThread; ++j) { a[j] = sHist[kPerThread * t + j]; sum += a[j]; }
    sScan[t] = sum;
    GroupMemoryBarrierWithGroupSync();
    for (uint off = 1; off < kGroup; off <<= 1) {
        uint v = t >= off ? sScan[t - off] : 0;
        GroupMemoryBarrierWithGroupSync();
        sScan[t] += v;
        GroupMemoryBarrierWithGroupSync();
    }
    uint run = sScan[t] - sum;
    for (uint j = 0; j < kPerThread; ++j) { sHist[kPerThread * t + j] = run; run += a[j]; }
    GroupMemoryBarrierWithGroupSync();
}

[shader("compute")]
[numthreads(kGroup, 1, 1)]
void cs_count(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    uint t = tid.x, block = gid.x;
    for (uint b = t; b < kBucketCount; b += kGroup) sHist[b] = 0;
    GroupMemoryBarrierWithGroupSync();
    for (uint k = 0; k < kBlockRows / kGroup; ++k) {
        uint local = t * (kBlockRows / kGroup) + k;
        uint i     = block * kBlockRows + local;
        uint bk    = bucket_of_row(i);
        gBucketOf[i] = bk;
        if (bk != kCulled) InterlockedAdd(sHist[bk], 1);
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint b = t; b < kBucketCount; b += kGroup) gBlockHist[block * kBucketCount + b] = sHist[b];
}

[shader("compute")]
[numthreads(kGroup, 1, 1)]
void cs_scan(uint3 dtid : SV_DispatchThreadID) {
    uint b = dtid.x;
    if (b >= kBucketCount) return;
    uint blocks = (gPush.row_count + kBlockRows - 1) / kBlockRows;
    uint run = 0;
    for (uint blk = 0; blk < blocks; ++blk) {
        uint idx = blk * kBucketCount + b;
        uint c   = gBlockHist[idx];
        gBlockHist[idx] = run;
        run += c;
    }
    gSortMeta[b] = run;
}

[shader("compute")]
[numthreads(kGroup, 1, 1)]
void cs_bucket_scan(uint3 tid : SV_GroupThreadID) {
    uint t = tid.x;
    uint counts[kPerThread];
    for (uint j = 0; j < kPerThread; ++j) { counts[j] = gSortMeta[kPerThread * t + j]; sHist[kPerThread * t + j] = counts[j]; }
    GroupMemoryBarrierWithGroupSync();
    scan_hist(t);
    for (uint j = 0; j < kPerThread; ++j) {
        uint b     = kPerThread * t + j;
        uint start = sHist[b];
        gSortMeta[kStarts + b] = start;
        gIndirect[b] = uint4(6, counts[j], 0, start); // vertexCount, instanceCount, firstVertex, firstInstance
    }
    if (t == kGroup - 1) gSortMeta[kVisible] = sHist[kBucketCount - 1] + counts[kPerThread - 1];
}

[shader("compute")]
[numthreads(kGroup, 1, 1)]
void cs_scatter(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    uint t = tid.x, block = gid.x;
    for (uint b = t; b < kBucketCount; b += kGroup) sHist[b] = 0;
    GroupMemoryBarrierWithGroupSync();
    for (uint k = 0; k < kBlockRows / kGroup; ++k) {
        uint local = t * (kBlockRows / kGroup) + k;
        uint i     = block * kBlockRows + local;
        uint bk    = i < gPush.row_count ? gBucketOf[i] : kCulled;
        if (bk != kCulled) InterlockedAdd(sHist[bk], 1);
        sKeys[local] = bk != kCulled ? (bk << 10) | local : kCulled; // culled rows sort last
    }
    GroupMemoryBarrierWithGroupSync();
    scan_hist(t); // sHist[b] is now the first local rank of bucket b within the block

    // Bitonic sort of the block's keys: equal buckets end up contiguous and in row order.
    for (uint k = 2; k <= kBlockRows; k <<= 1) {
        for (uint j = k >> 1; j > 0; j >>= 1) {
            for (uint e = t; e < kBlockRows; e += kGroup) {
                uint partner = e ^ j;
                if (partner > e) {
                    uint a = sKeys[e], b = sKeys[partner];
                    bool ascending = (e & k) == 0;
                    if ((a > b) == ascending) { sKeys[e] = b; sKeys[partner] = a; }
                }
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }

    for (uint p = t; p < kBlockRows; p += kGroup) {
        uint key = sKeys[p];
        if (key == kCulled) continue;
        uint bk    = key >> 10;
        uint local = key & (kBlockRows - 1);
        uint rank  = p - sHist[bk];
        uint dst   = gSortMeta[kStarts + bk] + gBlockHist[block * kBucketCount + bk] + rank;
        gSorted[dst] = block * kBlockRows + local;
    }
}

// ---- the key sort: within a bucket, by each row's key, then by row ------------------------------

groupshared uint sKeyK[kKeySortMax];
groupshared uint sKeyR[kKeySortMax];

// A float as a uint that sorts the same way.
uint orderable(float f) {
    uint u = asuint(f);
    return (u & 0x80000000u) != 0 ? ~u : (u | 0x80000000u);
}

// Row: key 0. Key: `order`. Feet: lower on screen draws later, so the key is minus the
// feet's y at the row's interpolation point (the same one the vertex shader draws).
uint key_of_row(uint row) {
    GpuSprite s = gSprites[row];
    uint mode = (s.flags >> kSortShift) & 3;
    float k = 0.0;
    if (mode == 1) k = s.order;
    else if (mode == 2) {
        float2 pos; float rot; float2 scale;
        row_placement(s, pos, rot, scale);
        k = -(pos.y + s.order);
    }
    return orderable(k);
}

// One group per keyed bucket (gPush.key_bucket): the bucket's range of gSorted, already in
// row order from the scatter, sorted by (key, row). A bucket with more than kKeySortMax
// visible rows sorts its first kKeySortMax and leaves the rest in row order.
[shader("compute")]
[numthreads(kGroup, 1, 1)]
void cs_key_sort(uint3 tid : SV_GroupThreadID) {
    uint t = tid.x;
    uint b = gPush.key_bucket;
    uint start = gSortMeta[kStarts + b];
    uint n = min(gSortMeta[b], kKeySortMax);
    uint p2 = 1;
    while (p2 < n) p2 <<= 1;
    for (uint i = t; i < p2; i += kGroup) {
        if (i < n) { uint row = gSorted[start + i]; sKeyR[i] = row; sKeyK[i] = key_of_row(row); }
        else { sKeyR[i] = 0xFFFFFFFFu; sKeyK[i] = 0xFFFFFFFFu; } // padding sorts last
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint k = 2; k <= p2; k <<= 1) {
        for (uint j = k >> 1; j > 0; j >>= 1) {
            for (uint e = t; e < p2; e += kGroup) {
                uint partner = e ^ j;
                if (partner > e) {
                    uint ka = sKeyK[e], kb = sKeyK[partner], ra = sKeyR[e], rb = sKeyR[partner];
                    bool greater = ka > kb || (ka == kb && ra > rb);
                    bool ascending = (e & k) == 0;
                    if (greater == ascending) { sKeyK[e] = kb; sKeyK[partner] = ka; sKeyR[e] = rb; sKeyR[partner] = ra; }
                }
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }
    for (uint i = t; i < n; i += kGroup) gSorted[start + i] = sKeyR[i];
}
