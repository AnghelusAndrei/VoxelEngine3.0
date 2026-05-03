#pragma once
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <cstdint>

// =============================================================================
// Profiler — GPU timestamp query management + per-frame results readback.
//
// The Profiler owns all GL query objects, manages ring-buffer allocation, and
// exposes clean per-stage timings via ProfilerResults. The renderer calls the
// named stamp methods (primaryStart / primaryEnd, etc.) around each dispatch;
// the Profiler handles the 3-frame-delayed readback internally.
//
// Usage pattern in the renderer:
//
//   profiler.beginFrame(glfwGetTime() * 1000.0);
//   profiler.primaryStart();
//   glDispatchCompute(...);
//   profiler.primaryEnd();
//   ...
//   profiler.endFrame(glfwGetTime() * 1000.0);
//
//   const ProfilerResults* r = profiler.getResults(); // null until frame 3
// =============================================================================

// -----------------------------------------------------------------------------
// ProfilerResults — pure data, no GL types. Safe to read from the UI thread.
// All times are in milliseconds.
// -----------------------------------------------------------------------------

struct BounceResult {
    double trace_ms = 0.0;
    double shade_ms = 0.0;
};

struct ChunkResult {
    double       emit_ms    = 0.0;
    double       bounces_ms = 0.0;
    int          numBounces = 0;
    BounceResult bounces[8];
};

struct ProfilerResults {
    // Wall-clock frame time (CPU) and total GPU span (first→last command).
    double CPU_ms           = 0.0;
    double GPU_ms           = 0.0;

    // Per-stage GPU times.
    double primary_ms       = 0.0;
    double normal_ms        = 0.0;
    double schedule_ms      = 0.0;
    double rank_ms          = 0.0;
    double threshold_ms     = 0.0;   // single-workgroup histogram scan (post-rank)
    double chunkLoop_ms     = 0.0;
    double resolve_ms       = 0.0;
    double finalBlit_ms     = 0.0;

    // CPU stall times — glGetBufferSubData sync points inside the frame.
    // Note: there is no longer a "threshold readback" — thresholds were
    // CPU-readback in the previous design, now derived entirely on-GPU by
    // threshold.comp. The two remaining stalls below are for candidateCount
    // (after schedule) and scheduledCount (after rank), both still required
    // for chunk-loop sizing.
    double scheduleReadback_ms = 0.0;
    double rankReadback_ms     = 0.0;

    // Per-chunk breakdown. numChunks <= MAX_CHUNKS.
    int         numChunks = 0;
    ChunkResult chunks[30];
};

// -----------------------------------------------------------------------------
// Profiler
// -----------------------------------------------------------------------------
class Profiler {
public:
    static constexpr int RING        = 5;   // ring depth; must be > readback lag (3)
    static constexpr int MAX_CHUNKS  = 30;
    static constexpr int MAX_BOUNCES = 8;

    Profiler();
    ~Profiler();

    // ---- Frame boundary (call at the very start / end of each run()) --------
    void beginFrame(double cpuNow_ms);
    void endFrame  (double cpuNow_ms);

    // ---- Stage stamps — call immediately before / after each dispatch -------
    void primaryStart();    void primaryEnd();
    void normalStart();     void normalEnd();
    void scheduleStart();   void scheduleEnd();
    void rankStart();       void rankEnd();
    void thresholdStart();  void thresholdEnd();
    void chunkLoopStart();  void chunkLoopEnd();
    void svgfStart();       void svgfEnd();
    void resolveStart();    void resolveEnd();
    void finalBlitStart();  void finalBlitEnd();

    // ---- Chunk / bounce stamps ----------------------------------------------
    void setNumChunks(int n);
    void emitStart   (int chunk);        void emitEnd   (int chunk);
    void bouncesStart(int chunk);        void bouncesEnd(int chunk, int numBounces);
    void traceStart  (int chunk, int b); void traceEnd  (int chunk, int b);
    void shadeStart  (int chunk, int b); void shadeEnd  (int chunk, int b);

    // ---- CPU stall times (measured on host around glGetBufferSubData) -------
    void setScheduleReadback(double ms);
    void setRankReadback    (double ms);

    // ---- Results ------------------------------------------------------------
    // Returns nullptr until the first readback is available (after frame 3).
    const ProfilerResults* getResults() const;

private:
    // -------------------------------------------------------------------------
    // QueryPair — owns one start/end GL timestamp query pair.
    // -------------------------------------------------------------------------
    struct QueryPair {
        GLuint start = 0, end = 0;

        void gen() {
            glGenQueries(1, &start);
            glGenQueries(1, &end);
        }
        void del() {
            glDeleteQueries(1, &start);
            glDeleteQueries(1, &end);
        }
        void stampStart() const { glQueryCounter(start, GL_TIMESTAMP); }
        void stampEnd()   const { glQueryCounter(end,   GL_TIMESTAMP); }

        // Blocks until the GPU result is available; returns elapsed ms.
        double readMs() const {
            GLuint64 s = 0, e = 0;
            glGetQueryObjectui64v(start, GL_QUERY_RESULT, &s);
            glGetQueryObjectui64v(end,   GL_QUERY_RESULT, &e);
            return (e > s) ? (double)(e - s) / 1.0e6 : 0.0;
        }
    };

    struct BounceQueries { QueryPair trace, shade; };

    struct ChunkQueries {
        QueryPair    emit, bounces;
        int          numBounces = 0;
        BounceQueries bounceData[MAX_BOUNCES];
    };

    // -------------------------------------------------------------------------
    // FrameData — one ring slot. Holds all query objects for a single frame
    // plus the associated CPU timestamps and readback stall times.
    // -------------------------------------------------------------------------
    struct FrameData {
        bool   initialized          = false;
        double cpuStart             = 0.0;
        double cpuEnd               = 0.0;
        double scheduleReadback_ms  = 0.0;
        double rankReadback_ms      = 0.0;
        int    numChunks            = 0;

        QueryPair    total, primary, normal, schedule, rank, threshold,
                     chunkLoop, svgf, resolve, finalBlit;
        ChunkQueries chunks[MAX_CHUNKS];

        void genAll();
        void deleteAll();
    };

    FrameData      frames_[RING];
    ProfilerResults results_;
    bool           hasResults_ = false;
    int            writeIdx_   = 0;

    FrameData& cur() { return frames_[writeIdx_]; }
    void doReadback(int readIdx);
};
