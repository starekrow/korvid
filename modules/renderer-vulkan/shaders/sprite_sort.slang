// Korvid renderer-vulkan: the sort. A counting sort of the live, visible sprite rows into
// buckets of (layer, blend), stable by row within a bucket, in four passes:
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
    float4 c = gPush.cull;
    if (pos.x + radius < c.x || pos.x - radius > c.x + c.z ||
        pos.y + radius < c.y || pos.y - radius > c.y + c.w) return kCulled;
    int  layer = clamp(s.layer, -128, 127);
    uint blend = min(gMaterials[s.material].blend, 2u);
    return uint(layer + int(kLayerMin)) * 3 + blend;
}

// Exclusive scan of sHist in place. Thread t owns entries 3t, 3t+1, 3t+2.
void scan_hist(uint t) {
    uint a0 = sHist[3 * t], a1 = sHist[3 * t + 1], a2 = sHist[3 * t + 2];
    uint sum = a0 + a1 + a2;
    sScan[t] = sum;
    GroupMemoryBarrierWithGroupSync();
    for (uint off = 1; off < kGroup; off <<= 1) {
        uint v = t >= off ? sScan[t - off] : 0;
        GroupMemoryBarrierWithGroupSync();
        sScan[t] += v;
        GroupMemoryBarrierWithGroupSync();
    }
    uint excl = sScan[t] - sum;
    sHist[3 * t]     = excl;
    sHist[3 * t + 1] = excl + a0;
    sHist[3 * t + 2] = excl + a0 + a1;
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
    for (uint j = 0; j < 3; ++j) sHist[3 * t + j] = gSortMeta[3 * t + j];
    GroupMemoryBarrierWithGroupSync();
    uint c0 = sHist[3 * t], c1 = sHist[3 * t + 1], c2 = sHist[3 * t + 2];
    scan_hist(t);
    uint counts[3] = {c0, c1, c2};
    for (uint j = 0; j < 3; ++j) {
        uint b     = 3 * t + j;
        uint start = sHist[b];
        gSortMeta[kStarts + b] = start;
        gIndirect[b] = uint4(6, counts[j], 0, start); // vertexCount, instanceCount, firstVertex, firstInstance
    }
    if (t == kGroup - 1) gSortMeta[kVisible] = sHist[kBucketCount - 1] + c2;
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
