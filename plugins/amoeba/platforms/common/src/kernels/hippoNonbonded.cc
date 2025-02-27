// This is a modified version of the standard nonbonded kernel for computing HippoNonbondedForce.
// This is needed because of two ways in which it differs from most nonbonded interactions:
// the force between two atoms doesn't always point along the line between them, and we need
// to accumulate torques as well as forces.

#define WARPS_PER_GROUP (THREAD_BLOCK_SIZE/TILE_SIZE)

#ifndef ENABLE_SHUFFLE
typedef struct {
    real x, y, z;
    real q;
    real fx, fy, fz;
    real tx, ty, tz;
    ATOM_PARAMETER_DATA
#ifndef PARAMETER_SIZE_IS_EVEN
    real padding;
#endif
} AtomData;
#endif

#ifdef ENABLE_SHUFFLE
//support for 64 bit shuffles
static __inline__ __device__ float real_shfl(float var, int srcLane) {
    return SHFL(var, srcLane);
}

static __inline__ __device__ double real_shfl(double var, int srcLane) {
    int hi, lo;
    asm volatile("mov.b64 { %0, %1 }, %2;" : "=r"(lo), "=r"(hi) : "d"(var));
    hi = SHFL(hi, srcLane);
    lo = SHFL(lo, srcLane);
    return __hiloint2double( hi, lo );
}

static __inline__ __device__ mm_long real_shfl(mm_long var, int srcLane) {
    int hi, lo;
    asm volatile("mov.b64 { %0, %1 }, %2;" : "=r"(lo), "=r"(hi) : "l"(var));
    hi = SHFL(hi, srcLane);
    lo = SHFL(lo, srcLane);
    // unforunately there isn't an __nv_hiloint2long(hi,lo) intrinsic cast
    int2 fuse; fuse.x = lo; fuse.y = hi;
    return *reinterpret_cast<mm_long*>(&fuse);
}
#endif

KERNEL void computeNonbonded(
        GLOBAL mm_ulong* RESTRICT forceBuffers, GLOBAL mixed* RESTRICT energyBuffer, GLOBAL const real4* RESTRICT posq, GLOBAL const unsigned int* RESTRICT exclusions,
        GLOBAL const int2* RESTRICT exclusionTiles, unsigned int startTileIndex, mm_ulong numTileIndices
#ifdef USE_CUTOFF
        , GLOBAL const int* RESTRICT tiles, GLOBAL const unsigned int* RESTRICT interactionCount, real4 periodicBoxSize, real4 invPeriodicBoxSize, 
        real4 periodicBoxVecX, real4 periodicBoxVecY, real4 periodicBoxVecZ, unsigned int maxTiles, GLOBAL const real4* RESTRICT blockCenter,
        GLOBAL const real4* RESTRICT blockSize, GLOBAL const unsigned int* RESTRICT interactingAtoms
#ifdef __CUDA_ARCH__
        , unsigned int maxSinglePairs, GLOBAL const int2* RESTRICT singlePairs
#endif
#endif
        PARAMETER_ARGUMENTS) {
    const unsigned int totalWarps = (GLOBAL_SIZE)/TILE_SIZE;
    const unsigned int warp = (GLOBAL_ID)/TILE_SIZE; // global warpIndex
    const unsigned int tgx = LOCAL_ID & (TILE_SIZE-1); // index within the warp
    const unsigned int tbx = LOCAL_ID - tgx;           // block warpIndex
    const unsigned int localAtomIndex = LOCAL_ID;
    mixed energy = 0;
    // used shared memory if the device cannot shuffle
#ifndef ENABLE_SHUFFLE
    LOCAL AtomData localData[THREAD_BLOCK_SIZE];
#endif

    // First loop: process tiles that contain exclusions.

    const unsigned int firstExclusionTile = FIRST_EXCLUSION_TILE+warp*(LAST_EXCLUSION_TILE-FIRST_EXCLUSION_TILE)/totalWarps;
    const unsigned int lastExclusionTile = FIRST_EXCLUSION_TILE+(warp+1)*(LAST_EXCLUSION_TILE-FIRST_EXCLUSION_TILE)/totalWarps;
    for (int pos = firstExclusionTile; pos < lastExclusionTile; pos++) {
        const int2 tileIndices = exclusionTiles[pos];
        const unsigned int x = tileIndices.x;
        const unsigned int y = tileIndices.y;
        real3 force = make_real3(0);
        real3 torque = make_real3(0);
        unsigned int atom1 = x*TILE_SIZE + tgx;
        real4 posq1 = posq[atom1];
        LOAD_ATOM1_PARAMETERS
        unsigned int excl = exclusions[pos*TILE_SIZE+tgx];
        const bool hasExclusions = true;
        if (x == y) {
            // This tile is on the diagonal.
#ifdef ENABLE_SHUFFLE
            real4 shflPosq = posq1;
#else
            localData[LOCAL_ID].x = posq1.x;
            localData[LOCAL_ID].y = posq1.y;
            localData[LOCAL_ID].z = posq1.z;
            localData[LOCAL_ID].q = posq1.w;
            LOAD_LOCAL_PARAMETERS_FROM_1
            SYNC_WARPS;
#endif

            // we do not need to fetch parameters from global since this is a symmetric tile
            // instead we can broadcast the values using shuffle
            for (unsigned int j = 0; j < TILE_SIZE; j++) {
                int atom2 = tbx+j;
                real4 posq2;
#ifdef ENABLE_SHUFFLE
                BROADCAST_WARP_DATA
#else   
                posq2 = make_real4(localData[atom2].x, localData[atom2].y, localData[atom2].z, localData[atom2].q);
#endif
                real3 delta = make_real3(posq2.x-posq1.x, posq2.y-posq1.y, posq2.z-posq1.z);
#ifdef USE_PERIODIC
                APPLY_PERIODIC_TO_DELTA(delta)
#endif
                real r2 = delta.x*delta.x + delta.y*delta.y + delta.z*delta.z;
                real rInv = RSQRT(r2);
                real r = r2*rInv;
                LOAD_ATOM2_PARAMETERS
                atom2 = y*TILE_SIZE+j;
                real3 tempForce = make_real3(0);
                real3 tempTorque1 = make_real3(0);
                real3 tempTorque2 = make_real3(0);
                bool isExcluded = (atom1 >= NUM_ATOMS || atom2 >= NUM_ATOMS || !(excl & 0x1));
                real tempEnergy = 0.0f;
                const real interactionScale = 0.5f;
                COMPUTE_INTERACTION
                energy += 0.5f*tempEnergy;
                force += tempForce;
                torque += tempTorque1;
                excl >>= 1;
                SYNC_WARPS;
            }
        }
        else {
            // This is an off-diagonal tile.
            unsigned int j = y*TILE_SIZE + tgx;
            real4 shflPosq = posq[j];
#ifdef ENABLE_SHUFFLE
            real3 shflForce = make_real3(0);
            real3 shflTorque = make_real3(0);
#else
            localData[LOCAL_ID].x = shflPosq.x;
            localData[LOCAL_ID].y = shflPosq.y;
            localData[LOCAL_ID].z = shflPosq.z;
            localData[LOCAL_ID].q = shflPosq.w;
            localData[LOCAL_ID].fx = 0.0f;
            localData[LOCAL_ID].fy = 0.0f;
            localData[LOCAL_ID].fz = 0.0f;
            localData[LOCAL_ID].tx = 0.0f;
            localData[LOCAL_ID].ty = 0.0f;
            localData[LOCAL_ID].tz = 0.0f;
#endif
            DECLARE_LOCAL_PARAMETERS
            LOAD_LOCAL_PARAMETERS_FROM_GLOBAL
            SYNC_WARPS;
            excl = (excl >> tgx) | (excl << (TILE_SIZE - tgx));
            unsigned int tj = tgx;
            for (j = 0; j < TILE_SIZE; j++) {
                int atom2 = tbx+tj;
#ifdef ENABLE_SHUFFLE
                real4 posq2 = shflPosq;
#else
                real4 posq2 = make_real4(localData[atom2].x, localData[atom2].y, localData[atom2].z, localData[atom2].q);
#endif
                real3 delta = make_real3(posq2.x-posq1.x, posq2.y-posq1.y, posq2.z-posq1.z);
#ifdef USE_PERIODIC
                APPLY_PERIODIC_TO_DELTA(delta)
#endif
                real r2 = delta.x*delta.x + delta.y*delta.y + delta.z*delta.z;
                real rInv = RSQRT(r2);
                real r = r2*rInv;
                LOAD_ATOM2_PARAMETERS
                atom2 = y*TILE_SIZE+tj;
                real3 tempForce = make_real3(0);
                real3 tempTorque1 = make_real3(0);
                real3 tempTorque2 = make_real3(0);
                bool isExcluded = (atom1 >= NUM_ATOMS || atom2 >= NUM_ATOMS || !(excl & 0x1));
                real tempEnergy = 0.0f;
                const real interactionScale = 1.0f;
                COMPUTE_INTERACTION
                energy += tempEnergy;
                force += tempForce;
                torque += tempTorque1;
#ifdef ENABLE_SHUFFLE
                shflForce -= tempForce;
                shflTorque += tempTorque2;
                SHUFFLE_WARP_DATA
                shflTorque.x = real_shfl(shflTorque.x, tgx+1);
                shflTorque.y = real_shfl(shflTorque.y, tgx+1);
                shflTorque.z = real_shfl(shflTorque.z, tgx+1);
#else
                localData[tbx+tj].fx -= tempForce.x;
                localData[tbx+tj].fy -= tempForce.y;
                localData[tbx+tj].fz -= tempForce.z;
                localData[tbx+tj].tx += tempTorque2.x;
                localData[tbx+tj].ty += tempTorque2.y;
                localData[tbx+tj].tz += tempTorque2.z;
#endif
                excl >>= 1;
                // cycles the indices
                // 0 1 2 3 4 5 6 7 -> 1 2 3 4 5 6 7 0
                tj = (tj + 1) & (TILE_SIZE - 1);
                SYNC_WARPS;
            }
            const unsigned int offset = y*TILE_SIZE + tgx;
            // write results for off diagonal tiles
#ifdef ENABLE_SHUFFLE
            ATOMIC_ADD(&forceBuffers[offset], (mm_ulong) realToFixedPoint(shflForce.x));
            ATOMIC_ADD(&forceBuffers[offset+PADDED_NUM_ATOMS], (mm_ulong) realToFixedPoint(shflForce.y));
            ATOMIC_ADD(&forceBuffers[offset+2*PADDED_NUM_ATOMS], (mm_ulong) realToFixedPoint(shflForce.z));
            ATOMIC_ADD(&torqueBuffers[offset], (mm_ulong) realToFixedPoint(shflTorque.x));
            ATOMIC_ADD(&torqueBuffers[offset+PADDED_NUM_ATOMS], (mm_ulong) realToFixedPoint(shflTorque.y));
            ATOMIC_ADD(&torqueBuffers[offset+2*PADDED_NUM_ATOMS], (mm_ulong) realToFixedPoint(shflTorque.z));
#else
            ATOMIC_ADD(&forceBuffers[offset], (mm_ulong) realToFixedPoint(localData[LOCAL_ID].fx));
            ATOMIC_ADD(&forceBuffers[offset+PADDED_NUM_ATOMS], (mm_ulong) realToFixedPoint(localData[LOCAL_ID].fy));
            ATOMIC_ADD(&forceBuffers[offset+2*PADDED_NUM_ATOMS], (mm_ulong) realToFixedPoint(localData[LOCAL_ID].fz));
            ATOMIC_ADD(&torqueBuffers[offset], (mm_ulong) realToFixedPoint(localData[LOCAL_ID].tx));
            ATOMIC_ADD(&torqueBuffers[offset+PADDED_NUM_ATOMS], (mm_ulong) realToFixedPoint(localData[LOCAL_ID].ty));
            ATOMIC_ADD(&torqueBuffers[offset+2*PADDED_NUM_ATOMS], (mm_ulong) realToFixedPoint(localData[LOCAL_ID].tz));
#endif
        }
        // Write results for on and off diagonal tiles

        const unsigned int offset = x*TILE_SIZE + tgx;
        ATOMIC_ADD(&forceBuffers[offset], (mm_ulong) realToFixedPoint(force.x));
        ATOMIC_ADD(&forceBuffers[offset+PADDED_NUM_ATOMS], (mm_ulong) realToFixedPoint(force.y));
        ATOMIC_ADD(&forceBuffers[offset+2*PADDED_NUM_ATOMS], (mm_ulong) realToFixedPoint(force.z));
        ATOMIC_ADD(&torqueBuffers[offset], (mm_ulong) realToFixedPoint(torque.x));
        ATOMIC_ADD(&torqueBuffers[offset+PADDED_NUM_ATOMS], (mm_ulong) realToFixedPoint(torque.y));
        ATOMIC_ADD(&torqueBuffers[offset+2*PADDED_NUM_ATOMS], (mm_ulong) realToFixedPoint(torque.z));
    }

    // Second loop: tiles without exclusions, either from the neighbor list (with cutoff) or just enumerating all
    // of them (no cutoff).

#ifdef USE_CUTOFF
    const unsigned int numTiles = interactionCount[0];
    if (numTiles > maxTiles)
        return; // There wasn't enough memory for the neighbor list.
    int pos = (int) (numTiles > maxTiles ? startTileIndex+warp*(mm_long)numTileIndices/totalWarps : warp*(mm_long)numTiles/totalWarps);
    int end = (int) (numTiles > maxTiles ? startTileIndex+(warp+1)*(mm_long)numTileIndices/totalWarps : (warp+1)*(mm_long)numTiles/totalWarps);
#else
    const unsigned int numTiles = numTileIndices;
    int pos = (int) (startTileIndex+warp*(mm_long)numTiles/totalWarps);
    int end = (int) (startTileIndex+(warp+1)*(mm_long)numTiles/totalWarps);
#endif
    int skipBase = 0;
    int currentSkipIndex = tbx;
    // atomIndices can probably be shuffled as well
    // but it probably wouldn't make things any faster
    LOCAL int atomIndices[THREAD_BLOCK_SIZE];
    LOCAL volatile int skipTiles[THREAD_BLOCK_SIZE];
    skipTiles[LOCAL_ID] = -1;
    
    while (pos < end) {
        const bool hasExclusions = false;
        real3 force = make_real3(0);
        real3 torque = make_real3(0);
        bool includeTile = true;

        // Extract the coordinates of this tile.
        int x, y;
        bool singlePeriodicCopy = false;
#ifdef USE_CUTOFF
        x = tiles[pos];
        real4 blockSizeX = blockSize[x];
        singlePeriodicCopy = (0.5f*periodicBoxSize.x-blockSizeX.x >= MAX_CUTOFF &&
                              0.5f*periodicBoxSize.y-blockSizeX.y >= MAX_CUTOFF &&
                              0.5f*periodicBoxSize.z-blockSizeX.z >= MAX_CUTOFF);
#else
        y = (int) floor(NUM_BLOCKS+0.5f-SQRT((NUM_BLOCKS+0.5f)*(NUM_BLOCKS+0.5f)-2*pos));
        x = (pos-y*NUM_BLOCKS+y*(y+1)/2);
        if (x < y || x >= NUM_BLOCKS) { // Occasionally happens due to roundoff error.
            y += (x < y ? -1 : 1);
            x = (pos-y*NUM_BLOCKS+y*(y+1)/2);
        }

        // Skip over tiles that have exclusions, since they were already processed.

        SYNC_WARPS;
        while (skipTiles[tbx+TILE_SIZE-1] < pos) {
            SYNC_WARPS;
            if (skipBase+tgx < NUM_TILES_WITH_EXCLUSIONS) {
                int2 tile = exclusionTiles[skipBase+tgx];
                skipTiles[LOCAL_ID] = tile.x + tile.y*NUM_BLOCKS - tile.y*(tile.y+1)/2;
            }
            else
                skipTiles[LOCAL_ID] = end;
            skipBase += TILE_SIZE;            
            currentSkipIndex = tbx;
            SYNC_WARPS;
        }
        while (skipTiles[currentSkipIndex] < pos)
            currentSkipIndex++;
        includeTile = (skipTiles[currentSkipIndex] != pos);
#endif
        if (includeTile) {
            unsigned int atom1 = x*TILE_SIZE + tgx;
            // Load atom data for this tile.
            real4 posq1 = posq[atom1];
            LOAD_ATOM1_PARAMETERS
#ifdef USE_CUTOFF
            unsigned int j = interactingAtoms[pos*TILE_SIZE+tgx];
#else
            unsigned int j = y*TILE_SIZE + tgx;
#endif
            atomIndices[LOCAL_ID] = j;
#ifdef ENABLE_SHUFFLE
            DECLARE_LOCAL_PARAMETERS
            real4 shflPosq;
            real3 shflForce = make_real3(0);
            real3 shflTorque = make_real3(0);
#endif
            if (j < PADDED_NUM_ATOMS) {
                // Load position of atom j from from global memory
#ifdef ENABLE_SHUFFLE
                shflPosq = posq[j];
#else
                localData[LOCAL_ID].x = posq[j].x;
                localData[LOCAL_ID].y = posq[j].y;
                localData[LOCAL_ID].z = posq[j].z;
                localData[LOCAL_ID].q = posq[j].w;
                localData[LOCAL_ID].fx = 0.0f;
                localData[LOCAL_ID].fy = 0.0f;
                localData[LOCAL_ID].fz = 0.0f;
                localData[LOCAL_ID].tx = 0.0f;
                localData[LOCAL_ID].ty = 0.0f;
                localData[LOCAL_ID].tz = 0.0f;
#endif                
                LOAD_LOCAL_PARAMETERS_FROM_GLOBAL
            }
            else {
#ifdef ENABLE_SHUFFLE
                shflPosq = make_real4(0, 0, 0, 0);
#else
                localData[LOCAL_ID].x = 0;
                localData[LOCAL_ID].y = 0;
                localData[LOCAL_ID].z = 0;
#endif
            }
            SYNC_WARPS;
#ifdef USE_PERIODIC
            if (singlePeriodicCopy) {
                // The box is small enough that we can just translate all the atoms into a single periodic
                // box, then skip having to apply periodic boundary conditions later.
                real4 blockCenterX = blockCenter[x];
                APPLY_PERIODIC_TO_POS_WITH_CENTER(posq1, blockCenterX)
#ifdef ENABLE_SHUFFLE
                APPLY_PERIODIC_TO_POS_WITH_CENTER(shflPosq, blockCenterX)
#else
                APPLY_PERIODIC_TO_POS_WITH_CENTER(localData[LOCAL_ID], blockCenterX)
#endif
                SYNC_WARPS;
                unsigned int tj = tgx;
                for (j = 0; j < TILE_SIZE; j++) {
                    int atom2 = tbx+tj;
#ifdef ENABLE_SHUFFLE
                    real4 posq2 = shflPosq; 
#else
                    real4 posq2 = make_real4(localData[atom2].x, localData[atom2].y, localData[atom2].z, localData[atom2].q);
#endif
                    real3 delta = make_real3(posq2.x-posq1.x, posq2.y-posq1.y, posq2.z-posq1.z);
                    real r2 = delta.x*delta.x + delta.y*delta.y + delta.z*delta.z;
                    real rInv = RSQRT(r2);
                    real r = r2*rInv;
                    LOAD_ATOM2_PARAMETERS
                    atom2 = atomIndices[tbx+tj];
                    real3 tempForce = make_real3(0);
                    real3 tempTorque1 = make_real3(0);
                    real3 tempTorque2 = make_real3(0);
                    bool isExcluded = (atom1 >= NUM_ATOMS || atom2 >= NUM_ATOMS);
                    real tempEnergy = 0.0f;
                    const real interactionScale = 1.0f;
                    COMPUTE_INTERACTION
                    energy += tempEnergy;
                    force += tempForce;
                    torque += tempTorque1;
#ifdef ENABLE_SHUFFLE
                    shflForce -= tempForce;
                    shflTorque += tempTorque2;
                    SHUFFLE_WARP_DATA
                    shflTorque.x = real_shfl(shflTorque.x, tgx+1);
                    shflTorque.y = real_shfl(shflTorque.y, tgx+1);
                    shflTorque.z = real_shfl(shflTorque.z, tgx+1);
#else
                    localData[tbx+tj].fx -= tempForce.x;
                    localData[tbx+tj].fy -= tempForce.y;
                    localData[tbx+tj].fz -= tempForce.z;
                    localData[tbx+tj].tx += tempTorque2.x;
                    localData[tbx+tj].ty += tempTorque2.y;
                    localData[tbx+tj].tz += tempTorque2.z;
#endif
                    tj = (tj + 1) & (TILE_SIZE - 1);
                    SYNC_WARPS;
                }
            }
            else
#endif
            {
                // We need to apply periodic boundary conditions separately for each interaction.
                unsigned int tj = tgx;
                for (j = 0; j < TILE_SIZE; j++) {
                    int atom2 = tbx+tj;
#ifdef ENABLE_SHUFFLE
                    real4 posq2 = shflPosq;
#else
                    real4 posq2 = make_real4(localData[atom2].x, localData[atom2].y, localData[atom2].z, localData[atom2].q);
#endif
                    real3 delta = make_real3(posq2.x-posq1.x, posq2.y-posq1.y, posq2.z-posq1.z);
#ifdef USE_PERIODIC
                    APPLY_PERIODIC_TO_DELTA(delta)
#endif
                    real r2 = delta.x*delta.x + delta.y*delta.y + delta.z*delta.z;
                    real rInv = RSQRT(r2);
                    real r = r2*rInv;
                    LOAD_ATOM2_PARAMETERS
                    atom2 = atomIndices[tbx+tj];
                    real3 tempForce = make_real3(0);
                    real3 tempTorque1 = make_real3(0);
                    real3 tempTorque2 = make_real3(0);
                    bool isExcluded = (atom1 >= NUM_ATOMS || atom2 >= NUM_ATOMS);
                    real tempEnergy = 0.0f;
                    const real interactionScale = 1.0f;
                    COMPUTE_INTERACTION
                    energy += tempEnergy;
                    force += tempForce;
                    torque += tempTorque1;
#ifdef ENABLE_SHUFFLE
                    shflForce -= tempForce;
                    shflTorque += tempTorque2;
                    SHUFFLE_WARP_DATA
                    shflTorque.x = real_shfl(shflTorque.x, tgx+1);
                    shflTorque.y = real_shfl(shflTorque.y, tgx+1);
                    shflTorque.z = real_shfl(shflTorque.z, tgx+1);
#else
                    localData[tbx+tj].fx -= tempForce.x;
                    localData[tbx+tj].fy -= tempForce.y;
                    localData[tbx+tj].fz -= tempForce.z;
                    localData[tbx+tj].tx += tempTorque2.x;
                    localData[tbx+tj].ty += tempTorque2.y;
                    localData[tbx+tj].tz += tempTorque2.z;
#endif
                    tj = (tj + 1) & (TILE_SIZE - 1);
                    SYNC_WARPS;
                }
            }

            // Write results.

            ATOMIC_ADD(&forceBuffers[atom1], (mm_ulong) realToFixedPoint(force.x));
            ATOMIC_ADD(&forceBuffers[atom1+PADDED_NUM_ATOMS], (mm_ulong) realToFixedPoint(force.y));
            ATOMIC_ADD(&forceBuffers[atom1+2*PADDED_NUM_ATOMS], (mm_ulong) realToFixedPoint(force.z));
            ATOMIC_ADD(&torqueBuffers[atom1], (mm_ulong) realToFixedPoint(torque.x));
            ATOMIC_ADD(&torqueBuffers[atom1+PADDED_NUM_ATOMS], (mm_ulong) realToFixedPoint(torque.y));
            ATOMIC_ADD(&torqueBuffers[atom1+2*PADDED_NUM_ATOMS], (mm_ulong) realToFixedPoint(torque.z));
#ifdef USE_CUTOFF
            unsigned int atom2 = atomIndices[LOCAL_ID];
#else
            unsigned int atom2 = y*TILE_SIZE + tgx;
#endif
            if (atom2 < PADDED_NUM_ATOMS) {
#ifdef ENABLE_SHUFFLE
                ATOMIC_ADD(&forceBuffers[atom2], (mm_ulong) realToFixedPoint(shflForce.x));
                ATOMIC_ADD(&forceBuffers[atom2+PADDED_NUM_ATOMS], (mm_ulong) realToFixedPoint(shflForce.y));
                ATOMIC_ADD(&forceBuffers[atom2+2*PADDED_NUM_ATOMS], (mm_ulong) realToFixedPoint(shflForce.z));
                ATOMIC_ADD(&torqueBuffers[atom2], (mm_ulong) realToFixedPoint(shflTorque.x));
                ATOMIC_ADD(&torqueBuffers[atom2+PADDED_NUM_ATOMS], (mm_ulong) realToFixedPoint(shflTorque.y));
                ATOMIC_ADD(&torqueBuffers[atom2+2*PADDED_NUM_ATOMS], (mm_ulong) realToFixedPoint(shflTorque.z));
#else
                ATOMIC_ADD(&forceBuffers[atom2], (mm_ulong) realToFixedPoint(localData[LOCAL_ID].fx));
                ATOMIC_ADD(&forceBuffers[atom2+PADDED_NUM_ATOMS], (mm_ulong) realToFixedPoint(localData[LOCAL_ID].fy));
                ATOMIC_ADD(&forceBuffers[atom2+2*PADDED_NUM_ATOMS], (mm_ulong) realToFixedPoint(localData[LOCAL_ID].fz));
                ATOMIC_ADD(&torqueBuffers[atom2], (mm_ulong) realToFixedPoint(localData[LOCAL_ID].tx));
                ATOMIC_ADD(&torqueBuffers[atom2+PADDED_NUM_ATOMS], (mm_ulong) realToFixedPoint(localData[LOCAL_ID].ty));
                ATOMIC_ADD(&torqueBuffers[atom2+2*PADDED_NUM_ATOMS], (mm_ulong) realToFixedPoint(localData[LOCAL_ID].tz));
#endif
            }
        }
        pos++;
    }
#ifdef INCLUDE_ENERGY
    energyBuffer[GLOBAL_ID] += energy;
#endif
}