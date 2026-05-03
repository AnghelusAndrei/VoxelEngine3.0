#include "profiler.hpp"

// =============================================================================
// FrameData — allocate / free all query objects in one call.
// =============================================================================

void Profiler::FrameData::genAll() {
    total.gen(); primary.gen(); normal.gen(); schedule.gen();
    rank.gen(); threshold.gen(); chunkLoop.gen();
    resolve.gen(); finalBlit.gen();

    for (int c = 0; c < Profiler::MAX_CHUNKS; c++) {
        chunks[c].emit.gen();
        chunks[c].bounces.gen();
        for (int b = 0; b < Profiler::MAX_BOUNCES; b++) {
            chunks[c].bounceData[b].trace.gen();
            chunks[c].bounceData[b].shade.gen();
        }
    }
}

void Profiler::FrameData::deleteAll() {
    total.del(); primary.del(); normal.del(); schedule.del();
    rank.del(); threshold.del(); chunkLoop.del();
    resolve.del(); finalBlit.del();

    for (int c = 0; c < Profiler::MAX_CHUNKS; c++) {
        chunks[c].emit.del();
        chunks[c].bounces.del();
        for (int b = 0; b < Profiler::MAX_BOUNCES; b++) {
            chunks[c].bounceData[b].trace.del();
            chunks[c].bounceData[b].shade.del();
        }
    }
}

// =============================================================================
// Profiler
// =============================================================================

Profiler::Profiler() {
    for (int k = 0; k < RING; k++)
        frames_[k].genAll();
}

Profiler::~Profiler() {
    for (int k = 0; k < RING; k++)
        frames_[k].deleteAll();
}

// -----------------------------------------------------------------------------
// Frame boundary
// -----------------------------------------------------------------------------

void Profiler::beginFrame(double cpuNow_ms) {
    cur().cpuStart = cpuNow_ms;
    cur().total.stampStart();
}

void Profiler::endFrame(double cpuNow_ms) {
    cur().total.stampEnd();
    cur().cpuEnd       = cpuNow_ms;
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

    results_.CPU_ms              = f.cpuEnd - f.cpuStart;
    results_.GPU_ms              = f.total.readMs();
    results_.primary_ms          = f.primary.readMs();
    results_.normal_ms           = f.normal.readMs();
    results_.schedule_ms         = f.schedule.readMs();
    results_.scheduleReadback_ms = f.scheduleReadback_ms;
    results_.rank_ms             = f.rank.readMs();
    results_.rankReadback_ms     = f.rankReadback_ms;
    results_.threshold_ms        = f.threshold.readMs();
    results_.chunkLoop_ms        = f.chunkLoop.readMs();
    results_.resolve_ms          = f.resolve.readMs();
    results_.finalBlit_ms        = f.finalBlit.readMs();

    results_.numChunks = f.numChunks;
    for (int c = 0; c < f.numChunks && c < MAX_CHUNKS; c++) {
        ChunkResult&  cr = results_.chunks[c];
        ChunkQueries& cq = f.chunks[c];

        cr.emit_ms    = cq.emit.readMs();
        cr.bounces_ms = cq.bounces.readMs();
        cr.numBounces = cq.numBounces;

        for (int b = 0; b < cq.numBounces && b < MAX_BOUNCES; b++) {
            cr.bounces[b].trace_ms = cq.bounceData[b].trace.readMs();
            cr.bounces[b].shade_ms = cq.bounceData[b].shade.readMs();
        }
    }

    hasResults_ = true;
}

const ProfilerResults* Profiler::getResults() const {
    return hasResults_ ? &results_ : nullptr;
}

// -----------------------------------------------------------------------------
// Per-frame metadata
// -----------------------------------------------------------------------------

void Profiler::setScheduleReadback(double ms) { cur().scheduleReadback_ms = ms; }
void Profiler::setRankReadback    (double ms) { cur().rankReadback_ms     = ms; }
void Profiler::setNumChunks       (int n)     { cur().numChunks           = n;  }

// -----------------------------------------------------------------------------
// Stage stamps
// -----------------------------------------------------------------------------

void Profiler::primaryStart()   { cur().primary.stampStart();   }
void Profiler::primaryEnd()     { cur().primary.stampEnd();     }
void Profiler::normalStart()    { cur().normal.stampStart();    }
void Profiler::normalEnd()      { cur().normal.stampEnd();      }
void Profiler::scheduleStart()  { cur().schedule.stampStart();  }
void Profiler::scheduleEnd()    { cur().schedule.stampEnd();    }
void Profiler::rankStart()      { cur().rank.stampStart();      }
void Profiler::rankEnd()        { cur().rank.stampEnd();        }
void Profiler::thresholdStart() { cur().threshold.stampStart(); }
void Profiler::thresholdEnd()   { cur().threshold.stampEnd();   }
void Profiler::chunkLoopStart() { cur().chunkLoop.stampStart(); }
void Profiler::chunkLoopEnd()   { cur().chunkLoop.stampEnd();   }
void Profiler::resolveStart()   { cur().resolve.stampStart();   }
void Profiler::resolveEnd()     { cur().resolve.stampEnd();     }
void Profiler::finalBlitStart() { cur().finalBlit.stampStart(); }
void Profiler::finalBlitEnd()   { cur().finalBlit.stampEnd();   }

// -----------------------------------------------------------------------------
// Chunk / bounce stamps
// -----------------------------------------------------------------------------

void Profiler::emitStart(int c) {
    if (c >= 0 && c < MAX_CHUNKS) cur().chunks[c].emit.stampStart();
}
void Profiler::emitEnd(int c) {
    if (c >= 0 && c < MAX_CHUNKS) cur().chunks[c].emit.stampEnd();
}
void Profiler::bouncesStart(int c) {
    if (c >= 0 && c < MAX_CHUNKS) cur().chunks[c].bounces.stampStart();
}
void Profiler::bouncesEnd(int c, int numBounces) {
    if (c >= 0 && c < MAX_CHUNKS) {
        cur().chunks[c].bounces.stampEnd();
        cur().chunks[c].numBounces = numBounces;
    }
}
void Profiler::traceStart(int c, int b) {
    if (c >= 0 && c < MAX_CHUNKS && b >= 0 && b < MAX_BOUNCES)
        cur().chunks[c].bounceData[b].trace.stampStart();
}
void Profiler::traceEnd(int c, int b) {
    if (c >= 0 && c < MAX_CHUNKS && b >= 0 && b < MAX_BOUNCES)
        cur().chunks[c].bounceData[b].trace.stampEnd();
}
void Profiler::shadeStart(int c, int b) {
    if (c >= 0 && c < MAX_CHUNKS && b >= 0 && b < MAX_BOUNCES)
        cur().chunks[c].bounceData[b].shade.stampStart();
}
void Profiler::shadeEnd(int c, int b) {
    if (c >= 0 && c < MAX_CHUNKS && b >= 0 && b < MAX_BOUNCES)
        cur().chunks[c].bounceData[b].shade.stampEnd();
}
