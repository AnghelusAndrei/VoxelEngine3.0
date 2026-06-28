#pragma once
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <cstdint>
#include <string>
#include <unordered_map>

// =============================================================================
// Profiler — GPU timestamp query management + per-frame results readback.
//
// GPU passes are identified by arbitrary string IDs.  Query objects are
// allocated lazily on first use per ring slot, so no registration step is
// needed and unused passes cost nothing.  The ring-buffer handles 3-frame-
// delayed readback internally.
//
// Usage:
//
//   profiler.beginFrame(glfwGetTime() * 1000.0);
//   profiler.start("primary");
//   glDispatchCompute(...);
//   profiler.end("primary");
//   ...
//   profiler.endFrame(glfwGetTime() * 1000.0);
//
//   const ProfilerResults* r = profiler.getResults(); // null until frame 3
//   if (r) r->passes.at("primary");   // GPU ms
//          r->cpu.at("schedReadback"); // CPU stall ms
// =============================================================================

// -----------------------------------------------------------------------------
// ProfilerResults — pure data, no GL types. Safe to read from the UI thread.
// All times are in milliseconds.
// -----------------------------------------------------------------------------

struct ProfilerResults {
    // Wall-clock frame time (CPU) and total GPU span (first→last command).
    double CPU_ms           = 0.0;
    double GPU_ms           = 0.0;

    // Per-stage GPU times.
    std::unordered_map<std::string, double> passes;  // per-pass GPU times
    std::unordered_map<std::string, double> cpu;     // CPU stall times
};

// -----------------------------------------------------------------------------
// Profiler
// -----------------------------------------------------------------------------
class Profiler {
public:
    static constexpr int RING        = 5;   // ring depth; must be > readback lag (3)

    Profiler();
    ~Profiler();

    // ---- Frame boundary (call at the very start / end of each run()) --------
    // beginFrame captures the true frame duration as the delta from the previous
    // beginFrame call, so it includes swap/vsync time that happens between run()s.
    void beginFrame(double cpuNow_ms);
    void endFrame  ();

    // ---- Named GPU pass stamps — call before / after each dispatch ----------
    // Queries are lazily allocated per ring slot on the first start() call.
    void start(const std::string& id);
    void end  (const std::string& id);

    // ---- Results ------------------------------------------------------------
    // Returns nullptr until the first readback is available (after frame 3).
    const ProfilerResults* getResults() const;

    // ---- CPU timing ---------------------------------------------------------
    void setCPU(const std::string& id, double ms);

private:
    // -------------------------------------------------------------------------
    // QueryPair — owns one start/end GL timestamp query pair.
    // -------------------------------------------------------------------------
    struct QueryPair {
        GLuint startQ = 0, endQ = 0;
        bool   ready  = false;

        void ensureGen() {
            if (!ready) {
                glGenQueries(1, &startQ);
                glGenQueries(1, &endQ);
                ready = true;
            }
        }
        void del() {
            if (!ready) return;
            glDeleteQueries(1, &startQ);
            glDeleteQueries(1, &endQ);
            ready = false;
        }
        void stampStart() { ensureGen(); glQueryCounter(startQ, GL_TIMESTAMP); }
        void stampEnd()   { ensureGen(); glQueryCounter(endQ,   GL_TIMESTAMP); }

        double readMs() const {
            if (!ready) return 0.0;
            GLuint64 s = 0, e = 0;
            glGetQueryObjectui64v(startQ, GL_QUERY_RESULT, &s);
            glGetQueryObjectui64v(endQ,   GL_QUERY_RESULT, &e);
            return (e > s) ? (double)(e - s) / 1.0e6 : 0.0;
        }
    };

    // -------------------------------------------------------------------------
    // FrameData — one ring slot. Holds all query objects for a single frame
    // plus the associated CPU timestamps and readback stall times.
    // -------------------------------------------------------------------------
    struct FrameData {
        bool   initialized          = false;
        double frameDuration        = 0.0;  // begin-to-begin wall clock (set by next frame)

        QueryPair total;
        std::unordered_map<std::string, QueryPair> passes;
        std::unordered_map<std::string, double>    cpuTimes;

        void deleteAll();
    };

    FrameData       frames_[RING];
    ProfilerResults results_;
    bool            hasResults_   = false;
    int             writeIdx_     = 0;
    double          lastBeginMs_  = -1.0;

    FrameData& cur() { return frames_[writeIdx_]; }
    void doReadback(int readIdx);
};
