// PenTimeline.h
//
// 確定時シリアライズ用のタイムライン圧縮 (純関数・header-only)。
// PreviewPenTool.cpp の build_pts_string() と単体テスト (Phase3EraserTest.cpp)
// の両方から使う。PenPoint に依存しない最小構造のみを扱うことで
// プラグイン本体とテストの二重実装を防ぐ。
//
// 背景: アンドゥ连打等でストローク間 t が大きく空くと再生がその分停止する。
// また ▶ で再生対象外 (lhide) にしたレイヤーへ描いた時間も丸ごと空白になる。
// GAP_CAP を超える空白を切り詰めることで、再生上の体感テンポだけを保存し直す。

#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace pentl {

struct TimelinePoint {
    double t;      // ms
    double vanish; // 消失時刻 ms (ゴースト断片のみ > 0、存在する線は 0)
};

struct TimelineStroke {
    int layer = 1;             // レイヤー番号 1..N (再生対象外判定に使用)
    std::size_t src_index = 0; // 呼び出し側ストローク配列への添字 (出力順の保持用)
    TimelinePoint* pts = nullptr;
    std::size_t n = 0;
};

// ストローク 1 本の終端時刻。obj2 側の t_end 延長 (最終点 t と最大 vanish の
// 大きい方) と同じ基準で比較する。
inline double stroke_end_ms(const TimelineStroke& s) {
    double e = 0.0;
    for (std::size_t i = 0; i < s.n; ++i) {
        if (s.pts[i].t > e) e = s.pts[i].t;
        if (s.pts[i].vanish > e) e = s.pts[i].vanish;
    }
    return e;
}

// entries を先頭点 t の安定ソート順で走査し、前回までの終端から gap_cap_ms を
// 超えて開く各エントリの全点 t / vanish を超過分だけ平行シフトする。
// layer_hidden(layer) が真を返すレイヤー (▶ 再生対象外) のエントリは
// 終端を進めない (=その描画時間ごと再生時間線からスキップされる)。ただし
// シフト自体は適用され、データ全体の時刻が無駄に伸びることを防ぐ。
// ゴースト断片は元ストロークと同じレイヤー番号を持つため自然に扱われる。
inline void compress_timeline(std::vector<TimelineStroke>& entries,
                              bool (*layer_hidden)(int),
                              double gap_cap_ms) {
    const auto start_ms = [](const TimelineStroke& s) -> double {
        return s.n ? s.pts[0].t : 1e18;
    };
    std::stable_sort(entries.begin(), entries.end(),
                     [&start_ms](const TimelineStroke& a, const TimelineStroke& b) {
                         return start_ms(a) < start_ms(b);
                     });
    bool have_prev = false;
    double prev_end = 0.0;
    for (auto& e : entries) {
        if (!e.n) continue;
        const double start = e.pts[0].t;
        if (have_prev && start > prev_end + gap_cap_ms) {
            const double shift = start - (prev_end + gap_cap_ms);
            for (std::size_t i = 0; i < e.n; ++i) {
                e.pts[i].t -= shift;
                if (e.pts[i].vanish > 0.0) {
                    e.pts[i].vanish -= shift;
                    if (e.pts[i].vanish < 0.0) e.pts[i].vanish = 0.0;
                }
            }
        }
        if (!(layer_hidden && layer_hidden(e.layer))) {
            const double end = stroke_end_ms(e);
            if (!have_prev || end > prev_end) prev_end = end;
            have_prev = true;
        }
    }
}

} // namespace pentl
