#pragma once
// Allocation counter for real-time safety tests (area 01 R19). alloc_counter.cpp replaces the
// global allocation functions of the engine test binary; while a thread has countAllocs set,
// every allocation it makes is counted in allocs.
#include <atomic>

namespace amq_test {
extern thread_local bool countAllocs;
extern std::atomic<long> allocs;

struct CountAllocsScope {
    CountAllocsScope() { countAllocs = true; }
    ~CountAllocsScope() { countAllocs = false; }
};
}  // namespace amq_test
