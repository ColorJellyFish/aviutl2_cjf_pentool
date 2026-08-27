// PenEraserGeom.h
// P3 消しゴム用の幾何純関数群。
// プラグイン本体 (PreviewPenTool.cpp) と単体テスト (Phase3EraserTest.cpp) の
// 双方から include される header-only 実装。Win32 / SDK に依存しない。
//
// 設計 :
//   - 消しゴム 1 ストローク = サンプル点列 (ペンと同じ ≥1px 間隔)。
//     高速移動でサンプル間が離れても取りこぼさないよう、サンプル間は
//     線分 (カプセル) として判定する。
//   - 判定は「ストローク各セグメントと消しゴム経路各セグメントの距離 ≤ r_e」。
//     ストローク外接矩形 × 消しゴム経路外接矩形で事前除外する。
//   - ヒットしたストロークセグメントを累積長空間の区間 [d0,d1] として記録し、
//     隣接・重なる区間を併合。complement run を可視断片、消去区間を
//     ゴースト断片 (d 属性付き) として切り出す。
//   - 境界点は座標 x,y と再生時刻 t を距離比例で線形按分補間する。

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace pengeom {

// 幾何計算用の点。t は按分補間の結果が非整数になるため double で持つ
// (呼び出し側で DWORD へ丸める)。
struct GeomPoint {
    double x, y, t;
};

// 累積長空間の区間。d0 < d1 を保証する。
struct Interval {
    double d0, d1;
};

// 断片。ghost=false は可視断片 (complement run)、ghost=true は消去された
// 区間 (プラグイン側で消失時刻 d 属性を付けて保持する)。
// keep_start / keep_end は元ストロークの始端 / 終端を含むかどうか
// (テーパー引継ぎ制御。false なら xi / xo トークンを書く)。
// vanish_ms はゴースト断片の消失時刻 (未設定 0。ghost のみ意味を持つ)。
struct Fragment {
    std::vector<GeomPoint> pts;
    bool keep_start = false;
    bool keep_end = false;
    bool ghost = false;
    double vanish_ms = 0.0;
};

// 時刻タグ付き消去区間 (案 A「時刻分割ゴースト」)。
// 同じ消去領域でも「最後に触れた時刻」が異なれば別断片になり、
// 手の動きに沿って順番に消える再生になる。
// t は操作時刻 (セッション ms。呼び出し側でバケット丸め済み)。
struct TaggedInterval {
    double d0, d1;
    double t;
};

// タグ付き区間を挿入する。既存区間と重なる範囲は新しい方の時刻で上書き
// (後からもう一度撫でた場所はより遅い時刻に消える)。結果は常に d0 昇順・
// 互いに重ならない。同時刻かつ隣接 (隙間 1e-6 未満) の区間は併合する。
inline void insert_tagged(std::vector<TaggedInterval>* v, const TaggedInterval& e) {
    if (!(e.d1 > e.d0)) return;
    constexpr double kEps = 1e-9;
    std::vector<TaggedInterval> out;
    out.reserve(v->size() + 3);
    const size_t n = v->size();
    size_t i = 0;
    double cursor = e.d0;
    bool consumed = false; // 挿入域が既存区間の途中で尽きた
    while (!consumed && i < n) {
        const TaggedInterval g = (*v)[i];
        if (g.d1 <= cursor + kEps) {       // 既存は挿入域より完全に左
            out.push_back(g);
            ++i;
            continue;
        }
        if (g.d0 >= e.d1 - kEps) break;    // 残りは挿入域より完全に右
        if (g.d0 > cursor + kEps)
            out.push_back({cursor, g.d0, e.t});              // 新規部分
        if (g.d0 < cursor - kEps)
            out.push_back({g.d0, cursor, g.t});              // 左残片 (旧時刻保持)
        const double ov_lo = std::max(cursor, g.d0);
        if (g.d1 > e.d1 - kEps) {
            out.push_back({ov_lo, e.d1, e.t});               // 重なりは新時刻
            out.push_back({e.d1, g.d1, g.t});                // 右残片 (旧時刻保持)
            ++i;
            consumed = true;               // 挿入域はここで尽きた
            break;
        }
        out.push_back({ov_lo, g.d1, e.t});                   // 重なりは新時刻
        cursor = g.d1;                     // 既存区間の末尾まで処理済み
        ++i;
    }
    if (!consumed && cursor < e.d1 - kEps)
        out.push_back({cursor, e.d1, e.t});
    while (i < n) out.push_back((*v)[i++]);
    // 同時刻・隣接の併合 (閾値は分割側 kEps と同一。境界値はコピーで
    // 受け渡されるため浮動小数の実質誤差はこれより十分小さい)
    std::vector<TaggedInterval> merged;
    merged.reserve(out.size());
    for (const auto& p : out) {
        if (!merged.empty() && merged.back().t == p.t &&
            p.d0 <= merged.back().d1 + 1e-9)
            merged.back().d1 = std::max(merged.back().d1, p.d1);
        else
            merged.push_back(p);
    }
    *v = std::move(merged);
}

// 点 p と線分 [a,b] の距離
inline double point_seg_dist(double px, double py,
                             double ax, double ay, double bx, double by) {
    const double dx = bx - ax, dy = by - ay;
    const double len2 = dx * dx + dy * dy;
    double u = (len2 > 0.0) ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0;
    u = std::max(0.0, std::min(1.0, u));
    const double qx = ax + dx * u, qy = ay + dy * u;
    return std::hypot(px - qx, py - qy);
}

// 点と点列 (折れ線) の最短距離。1 点ドットストロークの抹消判定用。
inline double point_polyline_dist(double px, double py,
                                  const std::vector<GeomPoint>& pts) {
    if (pts.empty()) return HUGE_VAL;
    if (pts.size() == 1)
        return std::hypot(px - pts[0].x, py - pts[0].y);
    double best = HUGE_VAL;
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        best = std::min(best, point_seg_dist(px, py,
                                             pts[i].x, pts[i].y,
                                             pts[i + 1].x, pts[i + 1].y));
        if (best <= 0.0) break;
    }
    return best;
}

// 線分 [a,b] と線分 [c,d] の最小距離。
// 相互クランプ投影の反復 (Ericson の方法)。平行・退化・交差も正しく扱う。
inline double seg_seg_dist(double ax, double ay, double bx, double by,
                           double cx, double cy, double dx, double dy) {
    const double ux = bx - ax, uy = by - ay; // 線分1
    const double wx = dx - cx, wy = dy - cy; // 線分2
    const double A = ux * ux + uy * uy;
    const double C = wx * wx + wy * wy;
    if (A <= 1e-12 && C <= 1e-12)
        return std::hypot(ax - cx, ay - cy);
    if (A <= 1e-12)
        return point_seg_dist(ax, ay, cx, cy, dx, dy);
    if (C <= 1e-12)
        return point_seg_dist(cx, cy, ax, ay, bx, by);
    auto clamp01 = [](double v) { return std::max(0.0, std::min(1.0, v)); };
    double s = 0.0, t = 0.0;
    for (int it = 0; it < 4; ++it) {
        const double px = ax + ux * s, py = ay + uy * s;
        t = clamp01(((px - cx) * wx + (py - cy) * wy) / C);
        const double qx = cx + wx * t, qy = cy + wy * t;
        s = clamp01(((qx - ax) * ux + (qy - ay) * uy) / A);
    }
    return std::hypot((ax + ux * s) - (cx + wx * t),
                      (ay + uy * s) - (cy + wy * t));
}

// 点列の累積長。戻り値[i] = 先頭から頂点 i までの距離 ([0]=0)。
inline std::vector<double> cum_lengths(const std::vector<GeomPoint>& pts) {
    std::vector<double> cum(pts.size(), 0.0);
    for (size_t i = 1; i < pts.size(); ++i) {
        const double dx = pts[i].x - pts[i - 1].x;
        const double dy = pts[i].y - pts[i - 1].y;
        cum[i] = cum[i - 1] + std::sqrt(dx * dx + dy * dy);
    }
    return cum;
}

// 累積長 d の位置の点を線形補間で求める (座標 + t の按分 )。
// cum は cum_lengths の結果。d が範囲外なら両端の値にクランプ。
inline GeomPoint point_at(const std::vector<GeomPoint>& pts,
                          const std::vector<double>& cum, double d) {
    if (pts.empty()) return {0, 0, 0};
    if (pts.size() == 1 || d <= 0.0) return pts.front();
    if (d >= cum.back()) return pts.back();
    size_t lo = 0, hi = pts.size() - 1;
    while (lo + 1 < hi) {
        const size_t mid = (lo + hi) / 2;
        if (cum[mid] <= d) lo = mid; else hi = mid;
    }
    const double span = cum[lo + 1] - cum[lo];
    const double u = (span > 0.0) ? (d - cum[lo]) / span : 0.0;
    GeomPoint r;
    r.x = pts[lo].x + (pts[lo + 1].x - pts[lo].x) * u;
    r.y = pts[lo].y + (pts[lo + 1].y - pts[lo].y) * u;
    r.t = pts[lo].t + (pts[lo + 1].t - pts[lo].t) * u;
    return r;
}

// 消去区間の検出。stroke の各セグメントについて、消しゴム経路 path の
// 全セグメントとの距離が r 以内ならそのセグメントの累積長区間を記録し、
// ソート + 併合して返す。stroke/path の外接矩形で事前除外する。
inline std::vector<Interval> erase_intervals(const std::vector<GeomPoint>& stroke,
                                             const std::vector<GeomPoint>& path,
                                             double r) {
    std::vector<Interval> out;
    if (stroke.size() < 2 || path.empty()) return out;
    // 外接矩形 (ストローク)
    double sminx = stroke[0].x, smaxx = sminx, sminy = stroke[0].y, smaxy = sminy;
    for (const auto& p : stroke) {
        sminx = std::min(sminx, p.x); smaxx = std::max(smaxx, p.x);
        sminy = std::min(sminy, p.y); smaxy = std::max(smaxy, p.y);
    }
    // 外接矩形 (消しゴム経路)
    double pminx = path[0].x, pmaxx = pminx, pminy = path[0].y, pmaxy = pminy;
    for (const auto& p : path) {
        pminx = std::min(pminx, p.x); pmaxx = std::max(pmaxx, p.x);
        pminy = std::min(pminy, p.y); pmaxy = std::max(pmaxy, p.y);
    }
    if (sminx > pmaxx + r || smaxx < pminx - r ||
        sminy > pmaxy + r || smaxy < pminy - r)
        return out; // 事前除外

    const std::vector<double> cum = cum_lengths(stroke);
    for (size_t i = 0; i + 1 < stroke.size(); ++i) {
        bool hit = false;
        for (size_t j = 0; !hit && j + 1 < path.size(); ++j) {
            hit = seg_seg_dist(stroke[i].x, stroke[i].y,
                               stroke[i + 1].x, stroke[i + 1].y,
                               path[j].x, path[j].y,
                               path[j + 1].x, path[j + 1].y) <= r;
        }
        if (path.size() == 1) {
            hit = point_seg_dist(path[0].x, path[0].y,
                                 stroke[i].x, stroke[i].y,
                                 stroke[i + 1].x, stroke[i + 1].y) <= r;
        }
        if (hit && cum[i + 1] > cum[i])
            out.push_back({cum[i], cum[i + 1]});
    }
    // ソート + 併合 (隙間 eps 未満の隣接区間も連続扱い)
    std::sort(out.begin(), out.end(),
              [](const Interval& a, const Interval& b) { return a.d0 < b.d0; });
    std::vector<Interval> merged;
    constexpr double kEps = 1e-9;
    for (const auto& iv : out) {
        if (!merged.empty() && iv.d0 <= merged.back().d1 + kEps) {
            merged.back().d1 = std::max(merged.back().d1, iv.d1);
        } else {
            merged.push_back(iv);
        }
    }
    return merged;
}

// 断片化。erase は erase_intervals の結果 (併合済み昇順)。
// 可視断片 (ghost=false) とゴースト断片 (ghost=true) を混ぜて返す。
// 長さ min_len 未満の断片は破棄。total≈0 (実質ドット) の入力は空を返す
// (ドットの扱いは呼び出し側で point_polyline_dist 判定とセットで行う)。
inline std::vector<Fragment> fragment_stroke(const std::vector<GeomPoint>& stroke,
                                             const std::vector<Interval>& erase,
                                             double min_len) {
    std::vector<Fragment> frags;
    if (stroke.size() < 2) return frags;
    const std::vector<double> cum = cum_lengths(stroke);
    const double total = cum.back();
    if (!(total > min_len)) return frags;

    // 正規化: [0,total] へクランプし、total 内でのみ意味のある区間に整える
    std::vector<Interval> iv;
    for (const auto& e : erase) {
        const double d0 = std::max(0.0, e.d0);
        const double d1 = std::min(total, e.d1);
        if (d1 - d0 > 1e-9) iv.push_back({d0, d1});
    }

    // complement run と消去区間を交互に切り出す
    auto push_run = [&](double d0, double d1, bool ghost,
                        bool ks, bool ke) {
        if (d1 - d0 < min_len) return;
        Fragment f;
        f.ghost = ghost;
        f.keep_start = ks;
        f.keep_end = ke;
        f.pts.push_back(point_at(stroke, cum, d0));
        for (size_t i = 0; i < stroke.size(); ++i) {
            if (cum[i] > d0 + 1e-9 && cum[i] < d1 - 1e-9)
                f.pts.push_back(stroke[i]);
        }
        f.pts.push_back(point_at(stroke, cum, d1));
        frags.push_back(std::move(f));
    };

    double cur = 0.0;
    for (const auto& e : iv) {
        push_run(cur, e.d0, false, cur <= 1e-9, false);
        push_run(e.d0, e.d1, true, e.d0 <= 1e-9, e.d1 >= total - 1e-9);
        cur = e.d1;
    }
    push_run(cur, total, false, false, true);
    return frags;
}

// 時刻タグ付き断片化 (案 A)。pieces は insert_tagged の結果
// (d0 昇順・重なりなし・各々消去時刻 t を持つ)。可視断片の complement run と、
// 断片ごとに個別の時刻を持つゴースト断片を返す。手の動きに沿って
// 順番に消える再生になる ( /  P3 方針)。
// 注意: total ≤ min_len の極短ストロークは断片を 1 つも返さない
// (=呼び出し側から丸ごと消える)。1 点ドットの抹消規約に準ずる扱い。
inline std::vector<Fragment> fragment_stroke_tagged(
    const std::vector<GeomPoint>& stroke,
    const std::vector<TaggedInterval>& pieces,
    double min_len) {
    std::vector<Fragment> frags;
    if (stroke.size() < 2 || pieces.empty()) return frags;
    const std::vector<double> cum = cum_lengths(stroke);
    const double total = cum.back();
    if (!(total > min_len)) return frags;

    auto emit = [&](double d0, double d1, bool ghost, bool ks, bool ke, double vt) {
        if (d1 - d0 < min_len) return;
        Fragment f;
        f.ghost = ghost;
        f.keep_start = ks;
        f.keep_end = ke;
        f.vanish_ms = vt;
        f.pts.push_back(point_at(stroke, cum, d0));
        for (size_t i = 0; i < stroke.size(); ++i) {
            if (cum[i] > d0 + 1e-9 && cum[i] < d1 - 1e-9)
                f.pts.push_back(stroke[i]);
        }
        f.pts.push_back(point_at(stroke, cum, d1));
        frags.push_back(std::move(f));
    };

    double cur = 0.0;
    for (const auto& p : pieces) {
        emit(cur, p.d0, false, cur <= 1e-9, false, 0.0);   // 可視 (隙間)
        emit(p.d0, p.d1, true, p.d0 <= 1e-9, p.d1 >= total - 1e-9, p.t);
        cur = p.d1;
    }
    emit(cur, total, false, false, true, 0.0);             // 末尾の可視
    return frags;
}

// 消しゴム 1 サンプル分 (prev→cur のカプセル) を tagged 区間へ反映する純関数。
// プラグイン本体の apply_erase_sample から呼ぶ。戻り値は区間が変化したか。
// prev は「必ず直前サンプル」を呼び出し側が渡すこと。カプセル判定により
// 高速移動でサンプル間が離れても経路上のストロークを取りこぼさない。
inline bool apply_sample_tagged(std::vector<TaggedInterval>* pieces,
                                const std::vector<GeomPoint>& stroke_geom,
                                const GeomPoint& prev, const GeomPoint& cur,
                                double r_e, double bucket) {
    auto add = erase_intervals(stroke_geom, {prev, cur}, r_e);
    bool changed = false;
    for (const auto& iv : add) {
        insert_tagged(pieces, {iv.d0, iv.d1, bucket});
        changed = true;
    }
    return changed;
}

} // namespace pengeom
