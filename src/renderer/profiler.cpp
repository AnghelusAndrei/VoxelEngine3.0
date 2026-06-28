#include "profiler.hpp"

// =============================================================================
// FrameData — allocate / free all query objects in one call.
// =============================================================================

void Profiler::FrameData::deleteAll() {
    total.del();
    for (auto& kv : passes) kv.second.del();
    passes.clear();
    cpuTimes.clear();
}

// =============================================================================
// Profiler
// =============================================================================

Profiler::Profiler() {
}

Profiler::~Profiler() {
    for (int k = 0; k < RING; k++)
        frames_[k].deleteAll();
}

// -----------------------------------------------------------------------------
// Frame boundary
// -----------------------------------------------------------------------------

void Profiler::beginFrame(double cpuNow_ms) {
    // Store the true frame duration (begin-to-begin) into the slot that just finished.
    // This captures swap/vsync time that occurs between run() returning and the next run().
    if (lastBeginMs_ >= 0.0) {
        int prevSlot = (writeIdx_ + RING - 1) % RING;
        frames_[prevSlot].frameDuration = cpuNow_ms - lastBeginMs_;
    }
    lastBeginMs_ = cpuNow_ms;
    cur().total.stampStart();
}

void Profiler::endFrame() {
    cur().total.stampEnd();
    cur().initialized  = true;

    // Read back the frame that is 3 slots behind the current write position.
    // That frame's GPU work was submitted ~3 frames ago — safe to wait on.
    int readIdx = (writeIdx_ + RING - 3) % RING;
    if (frames_[readIdx].initialized)
        doReadback(readIdx);

    writeIdx_ = (writeIdx_ + 1) % RING;
}

// -----------------------------------------------------------------------------
// Readback — called once per frame when a 3-frames-old slot is ready.
// -----------------------------------------------------------------------------

void Profiler::doReadback(int readIdx) {
    FrameData& f = frames_[readIdx];

    results_.CPU_ms              = f.frameDuration;
    results_.GPU_ms = f.total.readMs();

    results_.passes.clear();
    for (auto& kv : f.passes)
        results_.passes[kv.first] = kv.second.readMs();

    results_.cpu = f.cpuTimes;

    hasResults_ = true;
}

const ProfilerResults* Profiler::getResults() const {
    return hasResults_ ? &results_ : nullptr;
}

// -----------------------------------------------------------------------------
// Stage stamps
// -----------------------------------------------------------------------------

void Profiler::start(const std::string& id) { cur().passes[id].stampStart(); }
void Profiler::end  (const std::string& id) { cur().passes[id].stampEnd();   }

void Profiler::setCPU(const std::string& id, double ms) { cur().cpuTimes[id] = ms; }