/* EXP-013: lock-external ownership transitions, shared by the driver and host tests. */
#pragma once
#include <stdint.h>

struct NVBackingKey { uint64_t ownerGeneration, allocationGeneration; };
enum class NVOwnerPhase { Uninitialized, Active, Draining, Dead };
enum class NVBackingPhase { Resident, Parked, Retiring, Released };
enum class NVLeaseUse { CpuAccess, GpuMapping, Present };

static inline bool nvBackingKeyEqual(NVBackingKey a, NVBackingKey b)
{ return a.ownerGeneration == b.ownerGeneration && a.allocationGeneration == b.allocationGeneration; }
static inline bool nvNextGeneration(uint64_t *counter, uint64_t *out)
{
    if (!counter || !out || *counter == UINT64_MAX) return false;
    const uint64_t next = *counter + 1;
    *counter = next;
    *out = next;
    return true;
}
struct NVOwnerState {
    NVOwnerPhase phase = NVOwnerPhase::Uninitialized;
    uint64_t generation = 0, nextAllocation = 0;
    uint32_t operations = 0, allocations = 0, reservations = 0, borrowers = 0;
    bool activate(uint64_t value) {
        if (!value || phase != NVOwnerPhase::Uninitialized) return false;
        generation = value; phase = NVOwnerPhase::Active; return true;
    }
    bool enter() {
        if (phase != NVOwnerPhase::Active || operations == UINT32_MAX) return false;
        ++operations; return true;
    }
    void leave() { if (operations) --operations; }
    void drain() { if (phase == NVOwnerPhase::Active) phase = NVOwnerPhase::Draining; }
    bool drained() const {
        return phase == NVOwnerPhase::Draining && !operations && !allocations && !reservations && !borrowers;
    }
};
struct NVBackingState {
    NVBackingPhase phase = NVBackingPhase::Resident;
    NVBackingKey key = {};
    uint32_t leases[3] = {};
    bool acquire(NVOwnerPhase owner, NVLeaseUse use) {
        const unsigned i = (unsigned)use;
        if (i >= 3 || owner != NVOwnerPhase::Active ||
            (phase != NVBackingPhase::Resident && phase != NVBackingPhase::Parked) || leases[i] == UINT32_MAX) return false;
        ++leases[i]; return true;
    }
    void release(NVLeaseUse use) { const unsigned i = (unsigned)use; if (i < 3 && leases[i]) --leases[i]; }
    bool idle() const { return !(leases[0] | leases[1] | leases[2]); }
    bool park() { if (phase != NVBackingPhase::Resident) return false; phase = NVBackingPhase::Parked; return true; }
    bool unpark() { if (phase != NVBackingPhase::Parked) return false; phase = NVBackingPhase::Resident; return true; }
    void retire() { if (phase != NVBackingPhase::Released) phase = NVBackingPhase::Retiring; }
    bool releasable() const { return phase == NVBackingPhase::Retiring && idle(); }
};
struct NVTransactionState {
    const void *transaction = nullptr;
    uint64_t id = 0, sequence = 0, epoch = 0;
    NVBackingKey key = {};
    int result = 0;
    bool completed = false, queued = false;
    void begin(const void *t, uint64_t tid, uint64_t seq, uint64_t e) {
        transaction = t; id = tid; sequence = seq; epoch = e; key = {}; result = 0; completed = false; queued = false;
    }
    bool finish(const void *t, uint64_t seq, uint64_t e, NVBackingKey k, int status, bool image) {
        if (transaction != t || sequence != seq || epoch != e) return false;
        key = k; result = status; completed = !status; return image && completed;
    }
    bool matches(const void *t, uint64_t tid) const { return completed && transaction == t && id == tid; }
};
