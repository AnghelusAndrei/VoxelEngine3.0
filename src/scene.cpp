#include "scene.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#include <stb_image.h>

namespace {

// ---------------------------------------------------------------------------
// Small helpers (kept hand-rolled: no <filesystem> dependency, trivially portable)
// ---------------------------------------------------------------------------

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    const size_t b = s.find_last_not_of(" \t\r\n");
    return (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1);
}

std::string dirOf(const std::string& p) {
    const size_t s = p.find_last_of("/\\");
    return (s == std::string::npos) ? std::string(".") : p.substr(0, s);
}

std::string extOf(const std::string& p) {          // lowercased, includes the dot
    const size_t s = p.find_last_of("/\\");
    const std::string f = (s == std::string::npos) ? p : p.substr(s + 1);
    const size_t d = f.find_last_of('.');
    if (d == std::string::npos || d == 0) return std::string();
    std::string e = f.substr(d);
    for (size_t i = 0; i < e.size(); i++) e[i] = (char)std::tolower((unsigned char)e[i]);
    return e;
}

float maxComp(const glm::vec3& v) { return std::max(v.x, std::max(v.y, v.z)); }

// Ceiling on a .vox authored grid (1 byte/cell) — a sanity guard against a
// malformed header claiming an absurd size. MESHES no longer allocate a grid at
// all (they rasterize straight into the octree at place()), so this bounds only
// the .vox path, where real files are small.
constexpr uint64_t MAX_GRID_CELLS = 1ull << 31;

// ---------------------------------------------------------------------------
// Akenine-Möller triangle / axis-aligned-box overlap (SAT).
// Box given as centre c and half-extents h; vertices in the same space.
// ---------------------------------------------------------------------------

bool triBoxOverlap(const glm::vec3& c, const glm::vec3& h,
                   glm::vec3 v0, glm::vec3 v1, glm::vec3 v2)
{
    v0 -= c; v1 -= c; v2 -= c;

    // 1) box face axes — the triangle's AABB vs the box
    for (int i = 0; i < 3; i++) {
        const float mn = std::min(v0[i], std::min(v1[i], v2[i]));
        const float mx = std::max(v0[i], std::max(v1[i], v2[i]));
        if (mn > h[i] || mx < -h[i]) return false;
    }

    const glm::vec3 e[3] = { v1 - v0, v2 - v1, v0 - v2 };

    // 2) triangle plane vs box
    const glm::vec3 n = glm::cross(e[0], e[1]);
    const float d = glm::dot(n, v0);
    const float r = h.x * std::fabs(n.x) + h.y * std::fabs(n.y) + h.z * std::fabs(n.z);
    if (d > r || d < -r) return false;

    // 3) the 9 cross axes a = unit_k × e_i
    const glm::vec3 v[3] = { v0, v1, v2 };
    for (int ei = 0; ei < 3; ei++)
    for (int k = 0; k < 3; k++) {
        const int i1 = (k + 1) % 3, i2 = (k + 2) % 3;
        glm::vec3 a(0.0f);
        a[i1] = -e[ei][i2];
        a[i2] =  e[ei][i1];
        const float p0 = glm::dot(a, v[0]);
        const float p1 = glm::dot(a, v[1]);
        const float p2 = glm::dot(a, v[2]);
        const float mn = std::min(p0, std::min(p1, p2));
        const float mx = std::max(p0, std::max(p1, p2));
        const float rr = h[i1] * std::fabs(a[i1]) + h[i2] * std::fabs(a[i2]);
        if (mn > rr || mx < -rr) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------

// Voxel-scale sampling does not need source resolution, and Sponza's 54 maps at
// full size would be ~700 MB in a 32-bit process. Box-downsample to this on the
// long axis as they are decoded; peak cost is one full-size decode at a time.
constexpr int TEX_MAX_EDGE = 256;

std::string fixSlashes(std::string p) {
    for (char& c : p) if (c == '\\') c = '/';
    return p;
}

scene::Texture decodeTexture(const std::string& path) {
    scene::Texture t;
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!px) {
        printf("[scene] texture not found: %s\n", path.c_str());
        return t;
    }
    const int scale = std::max(1, (std::max(w, h) + TEX_MAX_EDGE - 1) / TEX_MAX_EDGE);
    t.w = std::max(1, w / scale);
    t.h = std::max(1, h / scale);
    t.rgba.assign((size_t)t.w * t.h * 4, 0);

    for (int y = 0; y < t.h; y++)
    for (int x = 0; x < t.w; x++) {
        uint32_t acc[4] = {0, 0, 0, 0};
        int count = 0;
        for (int sy = y * scale; sy < std::min((y + 1) * scale, h); sy++)
        for (int sx = x * scale; sx < std::min((x + 1) * scale, w); sx++) {
            const unsigned char* s = px + ((size_t)sy * w + sx) * 4;
            for (int c = 0; c < 4; c++) acc[c] += s[c];
            count++;
        }
        unsigned char* d = &t.rgba[((size_t)y * t.w + x) * 4];
        for (int c = 0; c < 4; c++) d[c] = (unsigned char)(count ? acc[c] / count : 0);
    }
    stbi_image_free(px);
    return t;
}

// Dominant colours of a texture, as up to `k` representatives.
//
// Uniform 5x5x5 bucketing then "keep the most populous buckets, each as the mean
// of its members" - deterministic, single pass, no iteration to tune. k-means
// would place representatives better but needs seeding and convergence policy for
// a result nobody inspects: these are voxel albedos, not a display palette.
// Fully transparent texels are skipped so a cutout's background never votes.
std::vector<glm::vec3> dominantColors(const scene::Texture& t, int k) {
    constexpr int B = 5;                       // buckets per channel
    std::array<glm::vec3, B*B*B> sum{};
    std::array<uint32_t,  B*B*B> hits{};

    for (size_t i = 0; i < t.rgba.size(); i += 4) {
        if (t.rgba[i + 3] < 128) continue;
        const glm::vec3 c(t.rgba[i] / 255.0f, t.rgba[i+1] / 255.0f, t.rgba[i+2] / 255.0f);
        const int bx = std::min(B - 1, (int)(c.r * B));
        const int by = std::min(B - 1, (int)(c.g * B));
        const int bz = std::min(B - 1, (int)(c.b * B));
        const int b  = (bx * B + by) * B + bz;
        sum[(size_t)b] += c;
        hits[(size_t)b]++;
    }

    std::vector<int> order;
    for (int b = 0; b < B*B*B; b++) if (hits[(size_t)b]) order.push_back(b);
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return hits[(size_t)a] > hits[(size_t)b]; });
    if ((int)order.size() > k) order.resize((size_t)k);

    std::vector<glm::vec3> out;
    out.reserve(order.size());
    for (int b : order) out.push_back(sum[(size_t)b] / (float)hits[(size_t)b]);
    if (out.empty()) out.push_back(glm::vec3(0.5f));      // fully transparent map
    return out;
}

// ---------------------------------------------------------------------------
// OBJ / MTL
// ---------------------------------------------------------------------------

struct MtlRec {
    std::string name;
    std::string mapKd;   // diffuse map - where a real .obj actually keeps its colour
    std::string mapD;    // alpha mask; cutout foliage is a rectangle without it
    glm::vec3 kd = glm::vec3(0.8f);
    glm::vec3 ks = glm::vec3(0.0f);
    glm::vec3 ke = glm::vec3(0.0f);
    float ns = 0.0f;    // Phong exponent
    float pr = -1.0f;   // PBR roughness extension (overrides Ns if present)
    float pm = -1.0f;   // PBR metallic extension
};

Material toMaterial(const MtlRec& r) {
    Material m{};
    m.color         = glm::vec4(r.kd, 0.0f);
    m.specularColor = glm::vec4(1.0f);
    const float ksMax = maxComp(r.ks);
    if (ksMax > 0.0f) m.specularColor = glm::vec4(r.ks / ksMax, 1.0f);
    // Phong exponent -> GGX roughness: sqrt(2/(Ns+2)); Ns=0 (default) -> 1.
    m.roughness = (r.pr >= 0.0f)
        ? glm::clamp(r.pr, 0.05f, 1.0f)
        : glm::clamp(std::sqrt(2.0f / (r.ns + 2.0f)), 0.05f, 1.0f);
    m.specular = glm::clamp(ksMax, 0.0f, 1.0f);
    m.metallic = (r.pm >= 0.0f) ? glm::clamp(r.pm, 0.0f, 1.0f) : 0.0f;
    const float keMax = maxComp(r.ke);
    if (keMax > 0.0f) {
        // Ke magnitude maps directly to emissiveIntensity, its hue to color —
        // author bright lights as e.g. "Ke 30 30 30" in the .mtl.
        m.emissive          = 1;
        m.emissiveIntensity = keMax;
        m.color             = glm::vec4(r.ke / keMax, 0.0f);
    }
    return m;
}

void parseMTL(const std::string& mtlPath, std::vector<MtlRec>& out) {
    std::ifstream f(mtlPath.c_str());
    if (!f) {
        printf("[scene] mtl not found: %s (using defaults)\n", mtlPath.c_str());
        return;
    }
    std::string line;
    MtlRec* cur = nullptr;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string tag; ss >> tag;
        if (tag == "newmtl") {
            std::string rest; std::getline(ss, rest);
            out.push_back(MtlRec{});
            cur = &out.back();
            cur->name = trim(rest);
        } else if (!cur) {
            continue;
        } else if (tag == "Kd") { ss >> cur->kd.x >> cur->kd.y >> cur->kd.z; }
          else if (tag == "Ks") { ss >> cur->ks.x >> cur->ks.y >> cur->ks.z; }
          else if (tag == "Ke") { ss >> cur->ke.x >> cur->ke.y >> cur->ke.z; }
          else if (tag == "Ns") { ss >> cur->ns; }
          else if (tag == "Pr") { ss >> cur->pr; }
          else if (tag == "Pm") { ss >> cur->pm; }
          else if (tag == "map_Kd") { std::string r; std::getline(ss, r); cur->mapKd = trim(r); }
          else if (tag == "map_d")  { std::string r; std::getline(ss, r); cur->mapD  = trim(r); }
    }
}

using scene::Tri;   // vertices in lattice coords after load(); defined in scene.hpp

// Exterior flood fill (6-connected) over empty cells, seeded from the grid
// boundary; anything unreached is interior and takes the majority surface
// material. Robust for surfaces closed at voxel resolution; an open mesh simply
// floods through and stays hollow.
void fillInterior(scene::Model& m) {
    const glm::ivec3 d = m.dim;
    const size_t vol = m.cells.size();
    auto idx = [&](int x, int y, int z) -> size_t {
        return (size_t)x + (size_t)d.x * ((size_t)y + (size_t)d.y * (size_t)z);
    };

    std::vector<bool> outside(vol, false);
    std::vector<size_t> stack;
    auto seed = [&](int x, int y, int z) {
        const size_t i = idx(x, y, z);
        if (!m.cells[i] && !outside[i]) { outside[i] = true; stack.push_back(i); }
    };
    for (int z = 0; z < d.z; z++)
    for (int y = 0; y < d.y; y++) { seed(0, y, z); seed(d.x - 1, y, z); }
    for (int z = 0; z < d.z; z++)
    for (int x = 0; x < d.x; x++) { seed(x, 0, z); seed(x, d.y - 1, z); }
    for (int y = 0; y < d.y; y++)
    for (int x = 0; x < d.x; x++) { seed(x, y, 0); seed(x, y, d.z - 1); }

    while (!stack.empty()) {
        const size_t i = stack.back(); stack.pop_back();
        const int x = (int)(i % (size_t)d.x);
        const int y = (int)((i / (size_t)d.x) % (size_t)d.y);
        const int z = (int)(i / ((size_t)d.x * (size_t)d.y));
        const int nx[6] = { x-1, x+1, x,   x,   x,   x   };
        const int ny[6] = { y,   y,   y,   y,   y-1, y+1 };
        const int nz[6] = { z,   z,   z-1, z+1, z,   z   };
        for (int k = 0; k < 6; k++) {
            if (nx[k] < 0 || nx[k] >= d.x ||
                ny[k] < 0 || ny[k] >= d.y ||
                nz[k] < 0 || nz[k] >= d.z) continue;
            const size_t ni = idx(nx[k], ny[k], nz[k]);
            if (!m.cells[ni] && !outside[ni]) { outside[ni] = true; stack.push_back(ni); }
        }
    }

    size_t counts[256] = {};
    for (size_t i = 0; i < vol; i++) counts[m.cells[i]]++;
    uint8_t fillMat = 1;
    for (int c = 2; c < 256; c++) if (counts[c] > counts[fillMat]) fillMat = (uint8_t)c;

    for (size_t i = 0; i < vol; i++)
        if (!m.cells[i] && !outside[i]) m.cells[i] = fillMat;
}

// Guarantee that no run of occupied cells is shorter than `minRun` along any
// axis — i.e. thicken what is TOO THIN and leave everything else exactly alone.
//
// Blanket dilation was tried and rejected: growing every surface by 2 turns a
// 9M-voxel building into ~45M voxels, and the resulting octree does not fit
// (this is a 32-bit toolchain, ~2 GB for the whole process). It is also wrong in
// kind — walls and columns are already hundreds of voxels thick and need
// nothing; only curtains, leaves and sheet details are the problem.
//
// Scanning by axis makes "too thin" trivial to see: a 1-voxel plate has runs of
// length 1 along its perpendicular and long runs in-plane, so only the
// perpendicular grows. Three 1D sweeps, one scratch LINE each (kilobytes).
void thicken(scene::Model& m, int minRun) {
    if (minRun <= 1) return;
    const glm::ivec3 d = m.dim;
    const size_t sx = 1, sy = (size_t)d.x, sz = (size_t)d.x * (size_t)d.y;
    std::vector<uint8_t> line((size_t)std::max(d.x, std::max(d.y, d.z)));

    auto sweep = [&](int n, size_t stride, size_t base) {
        for (int i = 0; i < n; i++) line[(size_t)i] = m.cells[base + (size_t)i * stride];
        int i = 0;
        while (i < n) {
            if (!line[(size_t)i]) { i++; continue; }
            int j = i;
            while (j < n && line[(size_t)j]) j++;             // maximal run [i, j)
            const int len = j - i;
            if (len < minRun) {
                const int need = minRun - len;
                const int lo = std::max(i - need / 2, 0);     // extend symmetrically
                const int hi = std::min(j + need - need / 2, n);
                const uint8_t mat = line[(size_t)i];
                for (int k = lo; k < hi; k++)                 // never overwrite a
                    if (!line[(size_t)k])                     // neighbouring run's
                        m.cells[base + (size_t)k * stride] = mat;   // own material
            }
            i = j;
        }
    };

    for (int z = 0; z < d.z; z++)
    for (int y = 0; y < d.y; y++) sweep(d.x, sx, (size_t)y * sy + (size_t)z * sz);
    for (int z = 0; z < d.z; z++)
    for (int x = 0; x < d.x; x++) sweep(d.y, sy, (size_t)x * sx + (size_t)z * sz);
    for (int y = 0; y < d.y; y++)
    for (int x = 0; x < d.x; x++) sweep(d.z, sz, (size_t)x * sx + (size_t)y * sy);
}

// `pad` shifts the model that many voxels off its own AABB min, so `dim` includes
// margin for the normal-extrusion at place() (a surface on the AABB face would
// otherwise extrude straight out of `dim`). Centring uses `dim`, so this keeps a
// thickened model centred; the octree's own bounds catch anything past the world.
scene::Model loadOBJ(const std::string& path, int resolution, int pad,
                     int colorsPerMaterial) {
    scene::Model m;
    m.source = path;

    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) { m.error = "cannot open " + path; return m; }
    // Slurp and scan the buffer directly. A real benchmark scene is 20+ MB of
    // text; a per-line std::istringstream costs SECONDS on it, which is most of
    // the engine's startup. Same grammar, ~10x less time.
    const std::string buf((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());

    std::vector<glm::vec3> positions;
    std::vector<glm::vec2> uvs;
    std::vector<Tri>       tris;
    std::vector<MtlRec>    srcMats;     // one per .mtl entry; expanded into `palette` below
    std::unordered_map<std::string, uint32_t> matIndex;
    uint32_t current   = UINT32_MAX;      // no material seen yet
    bool     capWarned = false;

    // Tri::mat is a uint8_t, so a mesh is capped at 256 SOURCE materials. That is a
    // different budget from the palette, which textures expand well past it.
    auto addSourceMat = [&](const MtlRec& r) -> uint32_t {
        if (srcMats.size() >= 256) {
            if (!capWarned) {
                printf("[scene] %s: over 256 source materials - extras reuse the first\n",
                       path.c_str());
                capWarned = true;
            }
            return 0;
        }
        srcMats.push_back(r);
        return (uint32_t)srcMats.size() - 1;
    };

    // Every scan is bounded by the line end, so a truncated line can never pull
    // a number off the next one (strtof/strtol treat '\n' as skippable space).
    auto isSpace = [](char c) { return c == ' ' || c == '\t'; };
    auto nextFloat = [&](const char*& c, const char* stop, float& out) {
        while (c < stop && isSpace(*c)) c++;
        if (c >= stop) return false;
        char* e = nullptr;
        out = std::strtof(c, &e);
        if (e == c) return false;
        c = e;
        return true;
    };
    auto tagIs = [&](const char* q, const char* stop, const char* tag) {
        const size_t n = std::strlen(tag);
        return (size_t)(stop - q) > n && std::memcmp(q, tag, n) == 0 && isSpace(q[n]);
    };

    std::vector<uint32_t> idx, tdx;                  // reused across faces (position / uv)
    const char* p   = buf.c_str();
    const char* end = p + buf.size();
    while (p < end) {
        const char* nl = (const char*)std::memchr(p, '\n', (size_t)(end - p));
        const char* eol = nl ? nl : end;
        const char* lineEnd = eol;
        if (lineEnd > p && lineEnd[-1] == '\r') lineEnd--;   // CRLF

        const char* q = p;
        while (q < lineEnd && isSpace(*q)) q++;

        if (tagIs(q, lineEnd, "v")) {
            const char* c = q + 1;
            glm::vec3 v(0.0f);
            if (nextFloat(c, lineEnd, v.x) && nextFloat(c, lineEnd, v.y)
                                           && nextFloat(c, lineEnd, v.z))
                positions.push_back(v);
        } else if (tagIs(q, lineEnd, "vt")) {
            const char* c = q + 2;
            glm::vec2 t(0.0f);
            if (!(nextFloat(c, lineEnd, t.x) && nextFloat(c, lineEnd, t.y))) t = glm::vec2(0.0f);
            uvs.push_back(t);
        } else if (tagIs(q, lineEnd, "f")) {
            if (current == UINT32_MAX) {             // faces before any usemtl
                MtlRec def; def.name = "default";
                current = addSourceMat(def);
                matIndex[def.name] = current;
            }
            idx.clear();
            tdx.clear();
            const char* c = q + 1;
            while (c < lineEnd) {
                while (c < lineEnd && isSpace(*c)) c++;
                if (c >= lineEnd) break;
                char* e = nullptr;
                long v = std::strtol(c, &e, 10);   // "i", "i/ti", "i/ti/ni" or "i//ni"
                const bool got = (e != c);
                c = got ? e : c;
                long t = LONG_MIN;                 // no vt on this vertex
                if (got && c < lineEnd && *c == '/') {
                    c++;
                    if (c < lineEnd && *c != '/') { t = std::strtol(c, &e, 10); c = e; }
                }
                while (c < lineEnd && !isSpace(*c)) c++;        // skip a trailing /ni
                if (!got) continue;
                if (v < 0) v += (long)positions.size(); else v -= 1;
                if (v < 0 || v >= (long)positions.size()) continue;
                if      (t == LONG_MIN) t = -1;
                else if (t < 0)         t += (long)uvs.size();  // negative index is relative
                else                    t -= 1;
                idx.push_back((uint32_t)v);
                tdx.push_back((t >= 0 && t < (long)uvs.size()) ? (uint32_t)t : UINT32_MAX);
            }
            auto uvAt = [&](size_t i) {
                return (tdx[i] == UINT32_MAX) ? glm::vec2(0.0f) : uvs[tdx[i]];
            };
            for (size_t i = 2; i < idx.size(); i++)             // fan triangulation
                tris.push_back({ { positions[idx[0]], positions[idx[i-1]], positions[idx[i]] },
                                 { uvAt(0),           uvAt(i-1),           uvAt(i)           },
                                 (uint8_t)current });
        } else if (tagIs(q, lineEnd, "mtllib")) {
            std::vector<MtlRec> recs;
            parseMTL(dirOf(path) + "/" + trim(std::string(q + 6, lineEnd)), recs);
            for (const MtlRec& r : recs)
                if (!matIndex.count(r.name))
                    matIndex[r.name] = addSourceMat(r);
        } else if (tagIs(q, lineEnd, "usemtl")) {
            const std::string name = trim(std::string(q + 6, lineEnd));
            auto it = matIndex.find(name);
            if (it == matIndex.end()) {              // undeclared — grey placeholder
                MtlRec ph; ph.name = name; ph.kd = glm::vec3(0.6f);
                matIndex[name] = addSourceMat(ph);
                it = matIndex.find(name);
            }
            current = it->second;
        }

        if (!nl) break;
        p = nl + 1;
    }

    if (tris.empty()) { m.error = "no faces in " + path; return m; }
    if (srcMats.empty()) { m.error = "no materials in " + path; return m; }

    // ---- expand source materials into the palette -----------------------------
    // A source material with a diffuse map becomes a RUN of palette entries, one per
    // dominant colour of that map; place() picks among the run per voxel from the
    // sampled UV. Without a map the run is one entry and behaves exactly as before.
    //
    // Doing this at load keeps place()'s invariant intact: the whole palette is known,
    // and therefore bindable, before a single voxel is written.
    {
        const std::string dir = dirOf(path);
        std::unordered_map<std::string, int> texOf;      // path -> m.textures index
        auto texture = [&](const std::string& rel) -> int {
            if (rel.empty()) return -1;
            const std::string full = dir + "/" + fixSlashes(rel);
            auto it = texOf.find(full);
            if (it != texOf.end()) return it->second;
            scene::Texture t = decodeTexture(full);
            if (!t.ok()) { texOf[full] = -1; return -1; }
            m.textures.push_back(std::move(t));
            const int id = (int)m.textures.size() - 1;
            texOf[full] = id;
            return id;
        };

        // Share the palette budget evenly, so one huge texture cannot starve the rest.
        const int perMat = std::max(1, std::min(colorsPerMaterial,
                              (int)(scene::MAX_PALETTE / std::max<size_t>(1, srcMats.size()))));

        for (const MtlRec& r : srcMats) {
            const Material base = toMaterial(r);
            const int tex  = texture(r.mapKd);
            const int mask = texture(r.mapD);
            m.srcSlot.push_back((int)m.palette.size());
            m.srcTex.push_back(tex);
            m.srcMask.push_back(mask);

            if (tex < 0) {                                // untextured: Kd as authored
                m.palette.push_back(base);
                m.paletteNames.push_back(r.name);
                m.srcCount.push_back(1);
                continue;
            }
            const std::vector<glm::vec3> cols = dominantColors(m.textures[(size_t)tex], perMat);
            for (size_t k = 0; k < cols.size(); k++) {
                Material mat = base;
                // Kd MODULATES the map in the .obj model, and is 1,1,1 in every real
                // asset - so this is the texture colour, tinted if the author meant it.
                mat.color = glm::vec4(cols[k] * glm::vec3(base.color), 0.0f);
                m.palette.push_back(mat);
                m.paletteNames.push_back(cols.size() == 1 ? r.name
                                         : r.name + "#" + std::to_string((int)k));
            }
            m.srcCount.push_back((int)cols.size());
        }
        printf("[scene] %s: %zu source materials -> %zu palette entries, %zu textures\n",
               path.c_str(), srcMats.size(), m.palette.size(), m.textures.size());
    }

    // Model AABB -> lattice scale. A mesh has no intrinsic voxel size, so the
    // longest axis is made to span `resolution` cells.
    glm::vec3 lo( FLT_MAX), hi(-FLT_MAX);
    for (const Tri& t : tris)
        for (int i = 0; i < 3; i++) { lo = glm::min(lo, t.v[i]); hi = glm::max(hi, t.v[i]); }

    const int   res    = (resolution > 0) ? resolution : scene::DEFAULT_RESOLUTION;
    const float maxExt = maxComp(hi - lo);
    if (!(maxExt > 0.0f)) { m.error = "degenerate (zero-size) mesh in " + path; return m; }
    const float s = (float)res / maxExt;

    m.dim = glm::max(glm::ivec3(1), glm::ivec3(glm::ceil((hi - lo) * s))) + 2 * pad;

    // Bake the model->lattice transform into the triangle vertices and hand the
    // triangles to place(), which rasterizes them straight into the octree. No
    // dense grid is allocated — that is the whole point of this change.
    const glm::vec3 pv((float)pad);
    for (Tri& t : tris)
        for (int i = 0; i < 3; i++) t.v[i] = (t.v[i] - lo) * s + pv;
    m.tris = std::move(tris);
    return m;
}

// ---------------------------------------------------------------------------
// VOX (MagicaVoxel)
// ---------------------------------------------------------------------------

struct VoxReader {
    const std::vector<uint8_t>& b;
    size_t p  = 0;
    bool   ok = true;

    bool has(size_t n) const { return ok && p + n <= b.size(); }
    uint8_t u8() { if (!has(1)) { ok = false; return 0; } return b[p++]; }
    uint32_t u32() {
        if (!has(4)) { ok = false; return 0; }
        const uint32_t v = (uint32_t)b[p] | ((uint32_t)b[p+1] << 8)
                         | ((uint32_t)b[p+2] << 16) | ((uint32_t)b[p+3] << 24);
        p += 4;
        return v;
    }
    std::string str() {
        const uint32_t n = u32();
        if (!has(n)) { ok = false; return {}; }
        std::string s((const char*)&b[p], n);
        p += n;
        return s;
    }
};

struct Rgba { uint8_t r, g, b, a; };

// MagicaVoxel's default palette, reconstructed from its documented structure:
// the 6-level RGB cube (255,204,153,102,51,0; blue fastest, black skipped),
// then 0x11-step pure R, G, B ramps, then the matching grey ramp. Index 0 unused.
std::array<Rgba, 256> defaultVoxPalette() {
    std::array<Rgba, 256> p{};
    int i = 1;
    const int L[6] = { 255, 204, 153, 102, 51, 0 };
    for (int r = 0; r < 6; r++)
    for (int g = 0; g < 6; g++)
    for (int b = 0; b < 6; b++) {
        if (L[r] == 0 && L[g] == 0 && L[b] == 0) continue;
        p[i++] = Rgba{ (uint8_t)L[r], (uint8_t)L[g], (uint8_t)L[b], 255 };
    }
    const int R[10] = { 238, 221, 187, 170, 136, 119, 85, 68, 34, 17 };
    for (int k = 0; k < 10; k++) p[i++] = Rgba{ (uint8_t)R[k], 0, 0, 255 };
    for (int k = 0; k < 10; k++) p[i++] = Rgba{ 0, (uint8_t)R[k], 0, 255 };
    for (int k = 0; k < 10; k++) p[i++] = Rgba{ 0, 0, (uint8_t)R[k], 255 };
    for (int k = 0; k < 10; k++) p[i++] = Rgba{ (uint8_t)R[k], (uint8_t)R[k], (uint8_t)R[k], 255 };
    return p;
}

struct VoxMat {
    std::string type;      // "_diffuse", "_emit", "_metal", "_glass", ...
    float weight = 1.0f;
    float rough  = -1.0f;
    float flux   = 0.0f;
};

scene::Model loadVOX(const std::string& path, int pad) {
    scene::Model m;
    m.source = path;

    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) { m.error = "cannot open " + path; return m; }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());

    VoxReader r{ bytes };
    if (!r.has(8) || std::memcmp(bytes.data(), "VOX ", 4) != 0) {
        m.error = "not a VOX file: " + path;
        return m;
    }
    r.p = 4;
    (void)r.u32();                                   // version

    if (!r.has(12) || std::memcmp(&bytes[r.p], "MAIN", 4) != 0) {
        m.error = "VOX: missing MAIN chunk in " + path;
        return m;
    }
    r.p += 4;
    (void)r.u32();                                   // MAIN content size (0)
    (void)r.u32();                                   // MAIN children size

    glm::ivec3 voxDim(0);
    bool haveSize = false, haveModel = false;
    struct V4 { uint8_t x, y, z, c; };
    std::vector<V4> voxels;
    auto palette = defaultVoxPalette();
    std::array<VoxMat, 256> voxMats{};

    while (r.ok && r.p + 12 <= bytes.size()) {
        char id[5] = {};
        std::memcpy(id, &bytes[r.p], 4);
        r.p += 4;
        const uint32_t cs = r.u32();
        (void)r.u32();                               // child bytes (siblings follow)
        const size_t contentStart = r.p;
        if (!r.has(cs)) break;

        if (!std::strcmp(id, "SIZE") && !haveSize) {
            voxDim.x = (int)r.u32(); voxDim.y = (int)r.u32(); voxDim.z = (int)r.u32();
            haveSize = true;
        } else if (!std::strcmp(id, "XYZI") && !haveModel) {
            const uint32_t n = r.u32();
            if (!r.has((size_t)n * 4)) break;
            voxels.resize(n);
            for (uint32_t i = 0; i < n; i++)
                voxels[i] = { r.u8(), r.u8(), r.u8(), r.u8() };
            haveModel = true;                        // multi-model files: first model only
        } else if (!std::strcmp(id, "RGBA")) {
            if (!r.has(256 * 4)) break;
            // Colour index i (1..255) uses file entry i-1.
            for (int i = 1; i < 256; i++) {
                const size_t o = contentStart + (size_t)(i - 1) * 4;
                palette[i] = Rgba{ bytes[o], bytes[o+1], bytes[o+2], bytes[o+3] };
            }
        } else if (!std::strcmp(id, "MATL")) {
            const uint32_t matId = r.u32();
            const uint32_t pairs = r.u32();
            VoxMat vm;
            for (uint32_t i = 0; i < pairs && r.ok; i++) {
                const std::string key = r.str();
                const std::string val = r.str();
                if      (key == "_type")   vm.type   = val;
                else if (key == "_weight") vm.weight = (float)atof(val.c_str());
                else if (key == "_rough")  vm.rough  = (float)atof(val.c_str());
                else if (key == "_flux")   vm.flux   = (float)atof(val.c_str());
                else if (key == "_emit")   vm.weight = (float)atof(val.c_str());
            }
            if (matId >= 1 && matId < 256) voxMats[matId] = vm;
        }
        r.p = contentStart + cs;                     // next sibling chunk
    }

    if (!haveSize || !haveModel || voxels.empty()) {
        m.error = "VOX: no model data in " + path;
        return m;
    }

    // VOX is z-up, the engine is y-up: (x,y,z)vox -> (x, z, sy-1-y)engine.
    // The y flip keeps the model right-handed instead of mirrored.
    m.dim = glm::ivec3(voxDim.x, voxDim.z, voxDim.y) + 2 * pad;
    const uint64_t nCells = (uint64_t)m.dim.x * (uint64_t)m.dim.y * (uint64_t)m.dim.z;
    if (nCells > MAX_GRID_CELLS) {
        m.error = "VOX: implausibly large grid in " + path;
        return m;
    }
    m.cells.assign((size_t)nCells, 0);

    std::array<uint32_t, 256> localOfColor;
    localOfColor.fill(UINT32_MAX);

    for (const V4& v : voxels) {
        if (v.c == 0) continue;
        uint32_t local = localOfColor[v.c];
        if (local == UINT32_MAX) {
            const Rgba    c  = palette[v.c];
            const VoxMat& vm = voxMats[v.c];
            Material mat{};
            mat.color         = glm::vec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 0.0f);
            mat.specularColor = glm::vec4(1.0f);
            mat.roughness     = (vm.rough >= 0.0f) ? glm::clamp(vm.rough, 0.05f, 1.0f) : 0.8f;
            mat.metallic      = (vm.type == "_metal") ? glm::clamp(vm.weight, 0.0f, 1.0f) : 0.0f;
            mat.specular      = (mat.metallic > 0.0f) ? 0.9f : 0.0f;
            if (vm.type == "_emit" && vm.weight > 0.0f) {
                // MagicaVoxel semantics: _emit weight in [0,1], _flux a
                // power-of-ten multiplier.
                mat.emissive          = 1;
                mat.emissiveIntensity = vm.weight * std::pow(10.0f, vm.flux);
            }
            if (m.palette.size() >= scene::MAX_PALETTE) local = 0;   // cap
            else {
                local = (uint32_t)m.palette.size();
                m.palette.push_back(mat);
                m.paletteNames.push_back("vox" + std::to_string((int)v.c));
            }
            localOfColor[v.c] = local;
        }
        const int ex = v.x + pad, ey = v.z + pad, ez = (voxDim.y - 1) - v.y + pad;
        if (ex < 0 || ex >= m.dim.x || ey < 0 || ey >= m.dim.y || ez < 0 || ez >= m.dim.z) continue;
        m.cells[(size_t)ex + (size_t)m.dim.x * ((size_t)ey + (size_t)m.dim.y * (size_t)ez)]
            = (uint8_t)(local + 1u);
    }

    if (m.palette.empty()) { m.error = "VOX: no coloured voxels in " + path; return m; }

    // .vox has no textures: every source material is one palette entry, so place()
    // needs no special case for the format.
    for (size_t i = 0; i < m.palette.size(); i++) {
        m.srcSlot.push_back((int)i);
        m.srcCount.push_back(1);
        m.srcTex.push_back(-1);
        m.srcMask.push_back(-1);
    }
    return m;
}

// ---------------------------------------------------------------------------

// Register one palette entry, keyed by NAME rather than by value.
//
// The obvious alternative — dedup byte-exact materials — is actively wrong for
// real assets. Sponza's 25 .mtl entries all carry the SAME placeholder grey Kd
// (its colour lived in textures no importer here reads), so value-dedup collapses
// "floor", "column_a", "leaf", "fabric_a" … into a single grey slot and the scene
// can never address them apart. Keying on the name keeps them distinct and
// paintable; identical values across DIFFERENT names cost slots, which is what
// collapse() is for.
//
// Same name + identical material => reuse (re-importing a model is free).
// Same name + different material => the pool renames it "name (2)", Windows-style.
// Pool full => map to the NEAREST existing material (never fails). A model with
// more distinct materials than the 127-slot pool therefore degrades in fidelity
// rather than aborting — the closest surviving slot stands in. Emissive/non-
// emissive are infinitely far apart in distance(), so a light never folds onto a
// wall. Returns 0 only if the pool is somehow empty, which cannot happen here.
uint32_t bindMaterial(MaterialPool& pool, const Material& m, const std::string& name) {
    const uint32_t byName = pool.findByName(name);
    if (byName && std::memcmp(&pool.materials[byName], &m, sizeof(Material)) == 0)
        return byName;
    if (pool.length < pool.capacity) {
        Material copy = m;
        return pool.addMaterial(&copy, name);
    }
    uint32_t best = 1;
    float    bestD = FLT_MAX;
    for (uint32_t i = 1; i < pool.length; i++) {
        const float d = MaterialPool::distance(pool.materials[i], m);
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

namespace scene {

// Nearest-texel wrap. Bilinear would be wasted here: the caller quantizes the
// result to a palette entry anyway, and .obj UVs routinely run far outside [0,1].
glm::vec4 Texture::sample(glm::vec2 uv) const {
    if (!ok()) return glm::vec4(1.0f);
    const float fu = uv.x - std::floor(uv.x);
    const float fv = uv.y - std::floor(uv.y);
    const int x = std::min(w - 1, std::max(0, (int)(fu * w)));
    // .obj UV origin is bottom-left, image rows run top-down.
    const int y = std::min(h - 1, std::max(0, (int)((1.0f - fv) * h)));
    const uint8_t* t = &rgba[((size_t)y * w + x) * 4];
    return glm::vec4(t[0], t[1], t[2], t[3]) * (1.0f / 255.0f);
}

Model load(const std::string& path, int resolution, bool solid, int thickness,
           int colorsPerMaterial) {
    const std::string ext = extOf(path);
    Model m;
    const int pad = (thickness > 1) ? thickness / 2 : 0;   // room to grow outward
    if      (ext == ".obj") m = loadOBJ(path, resolution, pad, colorsPerMaterial);
    else if (ext == ".vox") m = loadVOX(path, pad);
    else {
        m.source = path;
        m.error  = "unsupported format '" + ext + "' (supported: .obj, .vox)";
        return m;
    }
    if (!m.ok()) return m;

    if (!m.tris.empty()) {
        // Mesh: thickness is applied by normal-extrusion during place()'s
        // rasterization (no grid to thicken here). Solid fill needs a dense grid
        // to flood, which no longer exists for meshes — warn rather than lie.
        m.thickness = thickness;
        if (solid)
            printf("[scene] %s: solid fill needs a dense grid — ignored for meshes\n",
                   path.c_str());
    } else {
        // .vox keeps its small authored grid, so both passes still apply to it.
        // thicken FIRST so a later solid fill sees a shell without grazing pinholes.
        if (thickness > 1) thicken(m, thickness);
        if (solid)         fillInterior(m);
    }
    return m;
}

uint64_t place(const Model& m, Octree& octree, MaterialPool& pool, glm::ivec3 origin) {
    if (!m.ok()) {
        printf("[scene] place: %s\n",
               m.error.empty() ? "model is empty" : m.error.c_str());
        return 0;
    }

    // Make room BEFORE binding, if the palette would overflow the pool: compress
    // the EXISTING pool so its redundant entries (near-duplicate materials left by
    // earlier imports) free slots for this model's distinct ones. A one-shot —
    // retrying is pointless, since this model's own palette names would just
    // re-fill the pool. collapse() is a no-op when there is nothing to merge (a
    // fresh pool), so a single oversized model simply relies on the nearest-match
    // fallback in bindMaterial. Either way the import never aborts.
    if (pool.length + (uint32_t)m.palette.size() > pool.capacity) {
        const uint32_t target = (pool.capacity * 2) / 3;
        std::vector<uint32_t> remap;
        std::vector<std::string> lost;
        if (pool.collapse(target, remap, &lost) > 0) {
            // Voxels already in the octree reference the OLD slots, so the remap has to
            // reach the tree before the new palette is bound over the top of it.
            octree.remapMaterials(remap);
            pool.sync();
            printf("[scene] pool compressed to %u slots to fit %s (%zu prior materials merged)\n",
                   pool.length - 1, m.source.c_str(), lost.size());
        }
    }

    // Bind the whole palette BEFORE writing any voxel, so a model never half-lands.
    // bindMaterial cannot fail now (overflow folds to the nearest slot), so this is
    // a single pass. Track how many distinct pool slots the palette resolved to: if
    // fewer than the palette size, the pool was too small and some materials folded.
    std::vector<uint32_t> poolId(m.palette.size(), 0);
    for (size_t i = 0; i < m.palette.size(); i++) {
        const std::string& nm = (i < m.paletteNames.size()) ? m.paletteNames[i]
                                                            : std::string();
        poolId[i] = bindMaterial(pool, m.palette[i], nm);
    }

    const int bound = (int)(1u << octree.depth);
    const uint32_t voxBefore = octree.numVoxels;
    uint64_t outside = 0;
    auto stamp = [&](glm::ivec3 p, uint32_t mat) {
        if (p.x < 0 || p.y < 0 || p.z < 0 ||
            p.x >= bound || p.y >= bound || p.z >= bound) { outside++; return; }
        octree.insert(glm::uvec3(p), mat);
    };

    if (!m.tris.empty()) {
        // MESH: rasterize each triangle straight into the octree — no grid.
        //
        // Thickness is done here, per triangle, by NORMAL EXTRUSION: every voxel a
        // triangle covers is also stamped along the triangle's normal so the run
        // spans exactly `thickness` voxels perpendicular to the surface — that is
        // what gives the normal kernel a non-zero occupancy gradient (see
        // OCTREE.md). The offsets k in [-(T-1)/2 .. T/2] give exactly T cells for
        // any T (even T biases one voxel toward +normal); T=1 is raw. An
        // axis-aligned sheet — the case that was failing (curtains, leaves) — has
        // an axis-aligned normal, so the step is a clean perpendicular +/-1. A
        // unit normal always rounds to a non-zero step, so the slab never
        // collapses; a degenerate (zero-area) triangle just stamps its footprint.
        const int T   = (m.thickness > 1) ? m.thickness : 1;
        const int klo = -(T - 1) / 2, khi = T / 2;
        const glm::vec3 half(0.5f);
        for (const Tri& t : m.tris) {
            const glm::vec3 v0 = t.v[0], v1 = t.v[1], v2 = t.v[2];
            glm::vec3   nrm = glm::cross(v1 - v0, v2 - v0);
            const float nl  = glm::length(nrm);
            const bool  ext = (T > 1 && nl > 1e-8f);
            const glm::ivec3 n = ext ? glm::ivec3(glm::round(nrm / nl)) : glm::ivec3(0);

            const int src  = (int)t.mat;
            const int beg  = m.srcSlot[(size_t)src];
            const int cnt  = m.srcCount[(size_t)src];
            const int tex  = m.srcTex[(size_t)src];
            const int mask = m.srcMask[(size_t)src];

            // Barycentric setup, once per triangle. Solved in the triangle's own
            // plane so it is stable for slivers, which .obj scenes are full of.
            const glm::vec3 e1 = v1 - v0, e2 = v2 - v0;
            const float d11 = glm::dot(e1, e1), d12 = glm::dot(e1, e2), d22 = glm::dot(e2, e2);
            const float den = d11 * d22 - d12 * d12;
            const float invDen = (std::fabs(den) > 1e-12f) ? 1.0f / den : 0.0f;

            // Clamp the scan to the model's own lattice AABB (the shared far face
            // belongs to dim-1); the extrusion offset can still push a voxel out,
            // which stamp()'s world-bounds check absorbs.
            const glm::ivec3 c0 = glm::clamp(glm::ivec3(glm::floor(glm::min(v0, glm::min(v1, v2)))),
                                             glm::ivec3(0), m.dim - 1);
            const glm::ivec3 c1 = glm::clamp(glm::ivec3(glm::floor(glm::max(v0, glm::max(v1, v2)))),
                                             glm::ivec3(0), m.dim - 1);
            for (int z = c0.z; z <= c1.z; z++)
            for (int y = c0.y; y <= c1.y; y++)
            for (int x = c0.x; x <= c1.x; x++) {
                const glm::vec3 centre(x + 0.5f, y + 0.5f, z + 0.5f);
                if (!triBoxOverlap(centre, half, v0, v1, v2)) continue;

                uint32_t mat = poolId[(size_t)beg];
                if (tex >= 0 && invDen != 0.0f) {
                    // Voxel centre -> barycentric -> UV -> texel. Clamped rather than
                    // rejected: a voxel straddles the triangle, so its centre can sit
                    // just outside and still legitimately belong to this surface.
                    const glm::vec3 vp = centre - v0;
                    const float dp1 = glm::dot(vp, e1), dp2 = glm::dot(vp, e2);
                    float b1 = (d22 * dp1 - d12 * dp2) * invDen;
                    float b2 = (d11 * dp2 - d12 * dp1) * invDen;
                    b1 = glm::clamp(b1, 0.0f, 1.0f);
                    b2 = glm::clamp(b2, 0.0f, 1.0f - b1);
                    const glm::vec2 uv = t.uv[0] + b1 * (t.uv[1] - t.uv[0])
                                                 + b2 * (t.uv[2] - t.uv[0]);

                    // Cutout foliage: without map_d a leaf card is a solid rectangle.
                    if (mask >= 0 && m.textures[(size_t)mask].sample(uv).r < 0.5f) continue;
                    const glm::vec4 texel = m.textures[(size_t)tex].sample(uv);
                    if (texel.a < 0.5f) continue;            // alpha baked into map_Kd

                    // Nearest of this material's dominant colours. cnt is small
                    // (colorsPerMaterial), so a linear scan beats any index.
                    const glm::vec3 c(texel);
                    float best = FLT_MAX;
                    for (int k = 0; k < cnt; k++) {
                        const glm::vec3 d = glm::vec3(m.palette[(size_t)(beg + k)].color) - c;
                        const float dd = glm::dot(d, d);
                        if (dd < best) { best = dd; mat = poolId[(size_t)(beg + k)]; }
                    }
                }

                const glm::ivec3 base = origin + glm::ivec3(x, y, z);
                if (ext) for (int k = klo; k <= khi; k++) stamp(base + n * k, mat);
                else     stamp(base, mat);
            }
        }
    } else {
        // .vox: iterate the small authored grid.
        for (int z = 0; z < m.dim.z; z++)
        for (int y = 0; y < m.dim.y; y++)
        for (int x = 0; x < m.dim.x; x++) {
            const uint8_t c = m.at(x, y, z);
            if (!c) continue;
            stamp(origin + glm::ivec3(x, y, z), poolId[c - 1u]);
        }
    }
    const uint64_t written = (uint64_t)(octree.numVoxels - voxBefore);   // unique voxels added

    // Distinct pool slots the palette resolved to — less than palette size means
    // the pool was too small and the shortfall folded onto nearest neighbours.
    std::vector<uint32_t> uniq(poolId);
    std::sort(uniq.begin(), uniq.end());
    const size_t distinct = (size_t)(std::unique(uniq.begin(), uniq.end()) - uniq.begin());
    char foldNote[64] = {0};
    if (distinct < m.palette.size())
        snprintf(foldNote, sizeof foldNote, ", %zu folded to nearest",
                 m.palette.size() - distinct);

    printf("[scene] %s: %lu voxels at (%d %d %d)..(%d %d %d), %u materials%s%s\n",
           m.source.c_str(), (unsigned long)written,
           origin.x, origin.y, origin.z,
           origin.x + m.dim.x - 1, origin.y + m.dim.y - 1, origin.z + m.dim.z - 1,
           (unsigned)m.palette.size(), foldNote,
           outside ? " (clipped to world bounds)" : "");
    return written;
}

uint64_t import(const std::string& path, Octree& octree, MaterialPool& pool,
                glm::ivec3 centre, int resolution, bool solid, int thickness,
                int colorsPerMaterial) {
    const Model m = load(path, resolution, solid, thickness, colorsPerMaterial);
    if (!m.ok()) {
        printf("[scene] import failed: %s\n", m.error.c_str());
        return 0;
    }
    return place(m, octree, pool, centre - m.dim / 2);
}

} // namespace scene
