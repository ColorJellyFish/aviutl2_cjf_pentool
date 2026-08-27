// CJFPreviewPenTool.cpp
// AviUtl2 (AviUtl ExEdit2) generic plugin (.aux2)
// ペンツール: フリーハンドで線を描き、座標+タイミングを記録して
// ペンツール.obj2 の「線の座標」項目へ書き込み、「線が引かれていく」アニメーションオブジェクトにする。
//
// === アーキテクチャ概要 ===
//
// ■ 座標系 (CanvasView に集約):
// scene : シーン解像度 px・左上原点。ストロークの保存形式。
// client : ウィンドウ クライアント全体 px。
// canvas : プレビュー画像領域 (image_rect。シーンを等比フィットで表示)。
//
// ■ データ形式 (--value@pts「線の座標」に書く文字列):
// "x,y,t,l;x,y,t,l;...|..." ストローク区切りは '|'。
// x,y : シーンpx (左上原点) / t : 表示開始時刻 ms / l : レイヤー番号 1..5
// 属性トークン (ストローク先頭に ';' 区切りで配置。未知トークンは読み飛ばす):
// d<ms> : ゴースト断片の消失時刻。「引かれた後に消える」再生用
// xi/xo : 元ストローク始端/終端を含まない断片 (テーパー無効)
// u<ms> : 圧縮前の本来の先頭時刻 (タイムライン圧縮の原本保持)
//
// ■ レイヤーモデル:
// L1..L5 (L1=最前面)。描画 z 順は L5→L1、同一レイヤー内は描いた順。
// 各スロットはスタイル (太さ/入り抜き/色) + 表示チェック + ▶ 再生時非表示
// (lhide) をまとめて持ち、入替え/統合/クリアは「設定ごと移動」。
//
// ■ 確定と適用:
// 「確定」で strokes 全体を t 安定ソートして pts 文字列化し、対象オブジェクトの
// ペンツール効果へ書き込む (fmtver=2 / lhide / 標準描画 X/Y / スタイル差分)。
// 既存オブジェクトへの追記は、モード開始時に pts を読み込んで行う。
//
// ■ 履歴:
// すべての操作 (描画ジェスチャ/消しゴム/レイヤー統合/クリア/並べ替え) を
// ReplaceStrokes (strokes 全体 + レイヤー状態スナップショット) 1 オペとして記録。
// 上限 HISTORY_MAX_OPS 件、超過分は最古から破棄する。
//
// ■ 消しゴム (ベクター断片化方式):
// 消去判定 = ストローク各区間と消しゴム経路 (prev→cur カプセル) の距離 ≤ 半径。
// ヒット区間は累積長空間に「時刻タグ付き区間」として記録され、
// 可視部 (complement run) とゴースト断片 (d<ms> 付き) に断片化される。
// プレビューは最終状態 (ゴースト非表示)、再生は「引かれた後に消える」。
//
// ■ プレビュー描画の高速化:
// シーン画像 + 確定済み strokes をビットマップキャッシュし、フレームは
// 「キャッシュ blit + 描画中ストロークの上描き」。消しゴムは変化領域だけ部分更新。
// テーパーは幅を段階量子化したポリラインで擬似可変幅表現する。
//
// ■ タイムライン圧縮:
// 確定時に GAP_CAP (INI GapCapMs, 既定 1s) 超過の空白と ▶ 再生対象外
// レイヤーの描画時間を切り詰める。原本時刻は各ストロークの oshift と
// u<ms> トークンで保持されるため、▶ トグル変更後に再確定すれば
// 正しい時刻関係へ復帰する。
//
// ■ 入力:
// E/P/B ホットキーと Esc は GetAsyncKeyState ポーリング (前景/カーソルゲート付き)。
// Ctrl+Z/Y は RegisterHotKey。ペンモード中はホスト (AviUtl2 本体) への入力を
// EnableWindow(FALSE) で遮断し、終了時に復帰する。
//
// ■ INI (Plugin\CJF\CJFPreviewPenTool.ini [UI]):
// LeftPanelW/RightPanelW/SepToolY/SepWheelY : パネル幅・区切り位置
// WinW/WinH : ウィンドウサイズ (DPI 論理値)
// GapCapMs : タイムライン圧縮の最大空白 (既定 1000)
// PerfLog : フレーム計測ログ ([Perf]) 出力 (0/1)
#define NOMINMAX // windows.h の min/max マクロで std::min/std::max が壊れるのを防ぐ
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <new>
#include <string>
#include <vector>

#include "config2.h"
#include "plugin2.h"
#include "logger2.h"

#include "PenEraserGeom.h" // P3 消しゴム幾何純関数
#include "PenTimeline.h"   // P4 タイムライン圧縮純関数 (確定時シリアライズ)

COMMON_PLUGIN_TABLE common_plugin_table = {
    L"CJF Pen Tool",
    L"AviUtl2 freehand pen tool (drawing animation object)",
};

static LOG_HANDLE* logger = nullptr;
static CONFIG_HANDLE* config_handle = nullptr; // InitializeConfig で取得 (Alias パス用)
static HWND host_window = nullptr;
static HINSTANCE module_instance = nullptr;
static EDIT_HANDLE* edit_handle = nullptr; // RegisterPlugin で取得、以後も有効 (SDK サンプル準拠)

static HWND frame_window = nullptr;
static const wchar_t frame_class_name[] = L"CJFPreviewPenFrame";
static constexpr wchar_t panel_message_name[] = L"CJF.PreviewRangeSelector.Panel.Pen";
static constexpr wchar_t panel_clear_message_name[] = L"CJF.PreviewRangeSelector.Panel.PenClear";
// パネル自動作成で読み込むエイリアスファイル (Alias\ペンツール@推奨.object)。
static const wchar_t pen_alias_file_name[] = L"Alias\\ペンツール@推奨.object";

// アプリケーションデータフォルダ配下のエイリアスファイルを読み込む。
// 読み込めない場合は空文字列を返す (呼び出し側でオブジェクト作成を中止)。
static std::string load_alias_file(const wchar_t* rel_path) {
    if (!config_handle || !config_handle->app_data_path) return {};
    std::wstring path = config_handle->app_data_path;
    if (!path.empty() && path.back() != L'\\') path += L'\\';
    path += rel_path;
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || !fp) return {};
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) s.append(buf, n);
    fclose(fp);
    return s;
}

// チェックONで検知した対象オブジェクト（適用時に選択状態へ依存しない）
static OBJECT_HANDLE pen_trigger_object = nullptr;

// 「線をクリア」の対象オブジェクト（右クリックメニューから。遅延実行用）
static OBJECT_HANDLE clear_request_object = nullptr;

static void request_pen_mode();  // 前方宣言 (RegisterPlugin のメニュー登録から呼ぶ)
static void redraw_frame();      // 前方宣言
static void draw_drag_overlay(HDC dc);    // 前方宣言 (redraw_frame の最上位描画から呼ぶ)
static void mark_canvas_dirty(); // 前方宣言 (canvas_img 更新時に確定済みキャッシュを無効化)
static void draw_confirm_overlay(HDC dc); // 前方宣言 (同上。プリセット確認オーバーレイ)
static void layout_confirm_overlay();     // 前方宣言 (do_layout からの再配置に使用)
struct PenPoint;
static void parse_pts_string(const std::string& s,
                             std::vector<std::vector<PenPoint>>& out);
static bool apply_strokes_to_pen_tool(); // 前方宣言 (confirm_stroke から呼ぶ)
static void install_wheel_hook();        // 前方宣言 (finish_pen_mode / hide_frame から呼ぶ)
static void remove_wheel_hook();
struct RenderCtx;
static void restore_hidden_layer();      // 前方宣言 (hide_frame から呼ぶ)
static void update_color_box_texts();    // 前方宣言 (update_hue/sv/sync から呼ぶ)
static void apply_color_boxes();         // 前方宣言 (color_edit_proc から呼ぶ)

#define CONFIRM_RETRY_TIMER_ID 4
#define CONFIRM_RETRY_INTERVAL_MS 50
#define CONFIRM_RETRY_MAX 20
#define ESC_POLL_TIMER_ID 2
#define ESC_POLL_INTERVAL_MS 33
#define RESIZE_DEBOUNCE_TIMER_ID 21
#define RESIZE_DEBOUNCE_MS 50
#define HIDDEN_RENDER_TIMER_ID 22
#define HIDDEN_RENDER_DELAY_MS 200
static int confirm_retry_count = 0;

// レンダリングで取得した現在フレーム (シーン解像度, RGBA, r が先頭)
static std::vector<unsigned char> frame_rgba;
static int frame_w = 0;
static int frame_h = 0;

// クライアント全体の合成バッファ (BGRA 不透明)
static HDC g_dib_dc = nullptr;
static HBITMAP g_dib = nullptr;
static void* g_dib_bits = nullptr;
static int g_dib_w = 0, g_dib_h = 0;

// canvas 表示画像 (image_rect と同サイズ, BGRA)
static std::vector<unsigned char> canvas_img;
static int canvas_img_w = 0, canvas_img_h = 0;

static int client_w = 0;
static int client_h = 0;

//-----------------------------------------------------------------------------
// 共通ヘルパー
//-----------------------------------------------------------------------------

static int clamp_int(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

//-----------------------------------------------------------------------------
// レイアウト定数・色 (ダークテーマ)
//-----------------------------------------------------------------------------

constexpr int BAR_BOTTOM_H = 46;
// パネル幅とセパレータ位置はユーザー調整可 (セパレータドラッグ)。96dpi 基準で INI 保存。
static int g_left_panel_w = 190;
static int g_right_panel_w = 190;
static int g_sep_tool_y = 90;    // 左パネル: ツール領域とスライダー領域の境界 (チューニング値)
static int g_sep_wheel_y = 180;  // 左パネル: スライダー領域とホイール領域の境界 (チューニング値)

static const COLORREF COL_BG_RAIL     = RGB(24, 24, 28);
static const COLORREF COL_BTN_NORMAL  = RGB(45, 45, 50);
static const COLORREF COL_BTN_HOVER   = RGB(58, 58, 64);
static const COLORREF COL_SEL_ACCENT  = RGB(0, 120, 215);
static const COLORREF COL_TEXT_MAIN   = RGB(232, 232, 232);
static const COLORREF COL_TEXT_DIM    = RGB(160, 160, 160);
static const COLORREF COL_THUMB_EMPTY = RGB(35, 35, 40);
static const COLORREF COL_CANVAS_BG   = RGB(12, 12, 14);

// UI フォント (Per-Monitor DPI 対応)
static HFONT g_font_text = nullptr;
static HFONT g_font_small = nullptr;
static UINT g_current_dpi = 96;

static void destroy_fonts() {
    if (g_font_text) { DeleteObject(g_font_text); g_font_text = nullptr; }
    if (g_font_small) { DeleteObject(g_font_small); g_font_small = nullptr; }
}

static int dpi_s(int v) { return MulDiv(v, static_cast<int>(g_current_dpi), 96); }
static int left_panel_px() { return dpi_s(g_left_panel_w); }
static int right_panel_px() { return dpi_s(g_right_panel_w); }

// カスタムタイトルバー (設計確定): 標準フレームを除去し
// 自前描画のダークタイトル (ペンモード / 最大化・閉じるのみ。最小化は
// アイコン化時に WM_NCCALCSIZE カスタム処理が暴走するため廃止) を使う。
static int title_px() { return dpi_s(30); }
static RECT g_title_btn_rects[2] = {}; // 最大化 / 閉じる

static void rebuild_fonts(UINT dpi) {
    destroy_fonts();
    g_current_dpi = dpi ? dpi : 96;
    int h_main = -MulDiv(12, static_cast<int>(g_current_dpi), 96);
    int h_small = -MulDiv(11, static_cast<int>(g_current_dpi), 96);
    const wchar_t* faces[] = { L"Yu Gothic UI", L"Meiryo UI", L"MS UI Gothic" };
    for (const wchar_t* face : faces) {
        if (!g_font_text) {
            g_font_text = CreateFontW(h_main, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
        }
        if (!g_font_small) {
            g_font_small = CreateFontW(h_small, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
        }
    }
    if (!g_font_text) g_font_text = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    if (!g_font_small) g_font_small = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}


//-----------------------------------------------------------------------------
// 座標系一元化
// scene: シーン解像度 px・左上原点 (ストローク保存形式)
// client: ウィンドウクライアント全体 px (WM_ メッセージ)
// canvas: プレビュー画像領域 (image_rect, letterbox 余白は含まない)
// panel : 左右パネル・下部バー (ヒットテスト用矩形)
//-----------------------------------------------------------------------------

struct CanvasView {
    RECT area;       // canvas 利用可能領域 (client 座標, パネル間の帯全体)
    RECT image_rect; // シーン画像を実際に描く矩形 (= canvas_img の貼付先)
    float scale;     // client px per scene px (等比フィット)
};
static CanvasView g_view = {};

static void update_view(int cw, int ch) {
    int tpx = title_px();
    int ax = left_panel_px() + dpi_s(10);  // 縦セパレータ+canvas 枠線との隙間
    int ay = dpi_s(8) + tpx;               // タイトルバー分オフセット
    int aw = std::max(16, cw - left_panel_px() - right_panel_px() - dpi_s(20));
    int ah = std::max(16, ch - BAR_BOTTOM_H - dpi_s(16) - tpx);
    g_view.area = { ax, ay, ax + aw, ay + ah };
    if (frame_w >= 1 && frame_h >= 1 && aw >= 4 && ah >= 4) {
        float sx_scale = static_cast<float>(aw) / static_cast<float>(frame_w);
        float sy_scale = static_cast<float>(ah) / static_cast<float>(frame_h);
        float s = std::min(sx_scale, sy_scale);
        int fit_w = std::max(1, static_cast<int>(std::lroundf(frame_w * s)));
        int fit_h = std::max(1, static_cast<int>(std::lroundf(frame_h * s)));
        g_view.scale = s;
        g_view.image_rect = { ax + (aw - fit_w) / 2, ay + (ah - fit_h) / 2,
                              ax + (aw - fit_w) / 2 + fit_w, ay + (ah - fit_h) / 2 + fit_h };
    } else {
        g_view.scale = 1.0f;
        g_view.image_rect = { ax, ay, ax + aw, ay + ah };
    }
}

static POINT scene_to_client(double sx, double sy) {
    POINT p;
    p.x = g_view.image_rect.left +
          static_cast<int>(std::lround(static_cast<float>(sx) * g_view.scale));
    p.y = g_view.image_rect.top +
          static_cast<int>(std::lround(static_cast<float>(sy) * g_view.scale));
    return p;
}

static void client_to_scene(const POINT& cpt, double* sx, double* sy) {
    if (g_view.scale <= 0.0001f) {
        *sx = *sy = 0.0;
        return;
    }
    *sx = (cpt.x - g_view.image_rect.left) / g_view.scale;
    *sy = (cpt.y - g_view.image_rect.top) / g_view.scale;
}

// client pt が canvas 画像矩形内か (「外は何もしない」判定に使う)
static bool pt_in_canvas(const POINT& cpt) {
    return cpt.x >= g_view.image_rect.left && cpt.x < g_view.image_rect.right &&
           cpt.y >= g_view.image_rect.top && cpt.y < g_view.image_rect.bottom;
}

//-----------------------------------------------------------------------------
// モード・ストローク状態
//-----------------------------------------------------------------------------

enum class Mode { Idle, PenDraw };
static Mode mode = Mode::Idle;

// レンダリング中の再入を防ぐ。
static bool render_in_progress = false;

// カラーピッカー等モーダル中表示中 (Esc ポーリング等のタイマー動作を止める)
static bool in_modal_dialog = false;

// 透過レンダリング用に一時非表示にしたレイヤー (-1 = なし)。
// 対象オブジェクトの線を背景画像に写さず、下絵のみの状態で描き始めるための仕組み。
// maskselector の「隠しプレビューモード」と同じ set_layer_enable + set_edited_state 方式。
// 注意: 低レベル API でのレイヤー切替は直後のレンダリングに反映されない (旧キャッシュが
// 返る) ため、メッセージループへ戻ってから 200ms 待いてレンダリングを開始する必要がある。
static int g_hidden_layer = -1;

// 記録中のストローク（シーン座標 + モード開始からの ms）
// 左ドラッグ 1 回 = 1 ストローク。strokes に書き順で積み、ペンアップ（マウス解放）で区切る
struct PenPoint {
    double sx, sy;
    DWORD t;
    double acc; // ストローク先頭からの累積長（シーン px）。プレビュー描画の二分探索用に保持
    int layer;  // レイヤー番号 1..PEN_LAYER_MAX (ストローク内は全点同一。先頭点が代表)
    // ---- P3 消しゴム断片用のストローク属性 (全点同一値。先頭点が代表) ----
    bool taper_in = true;  // 元ストローク始端を含む。false なら書出し時に xi トークン
    bool taper_out = true; // 元ストローク終端を含む。false なら xo トークン
    DWORD vanish = 0;      // 消失時刻 ms。ゴースト断片のみ > 0。プレビューでは非描画
    // ---- P4 タイムライン圧縮の原本保持 (全点同一値。先頭点が代表) ----
    // oshift == u<orig> (原本先頭時刻) を保持。圧縮後 t との差 shift = oshift - front.t
    // を全点に加算して原本復元する (build_pts_string 参照)。旧実装の t+oshift は
    // orig+compressed の二重加算で非冪等だったためストローク単位 shift に修正。
    DWORD oshift = 0;
};

// 最大レイヤー数 (確定値)。1=最前面。
static constexpr int PEN_LAYER_MAX = 5;

// セッションストローク (既存読込分 + 新規描画分の統一配列)。
// 書き順で積む。確定時に t 安定ソートして座標列へ書き出す。
static std::vector<std::vector<PenPoint>> strokes;
static DWORD pen_mode_start = 0;
static bool pen_down = false;
// 描画ジェスチャがキャンバス外にいる間 true (外→内で新しいストロークとして再開)
static bool g_pen_outside = false;
// 1 ジェスチャ = 1 ReplaceStrokes 履歴のための開始時スナップショット (消しゴムと同一規約)
static std::vector<std::vector<PenPoint>> g_gesture_before;
static bool g_gesture_dirty = false;

//-----------------------------------------------------------------------------
// レイヤーモデル
//-----------------------------------------------------------------------------

// レイヤー別スタイル。obj2 の L1..L5 項目と対応する。
struct PenLayerStyle {
    COLORREF col = RGB(255, 255, 255);
    double w = 8.0;
    double ti = 0.0, twi = 0.0, to = 0.0, two_ = 0.0; // 入り長さ/入り太さ/抜き長さ/抜き太さ
};
static PenLayerStyle g_layer_styles[PEN_LAYER_MAX + 1]; // [1..PEN_LAYER_MAX]

static int g_cur_layer = 1;                    // 現在編集対象レイヤー (新規ストロークの所属)
static bool g_layer_visible[PEN_LAYER_MAX + 1] = {}; // 表示チェック。データは保持される
static bool g_layer_playback_hidden[PEN_LAYER_MAX + 1] = {}; // 再生時も非表示 (obj2 の lhide 項目と対応)
static bool g_legacy_object = false;           // 新構成項目を持たない旧オブジェクト
static bool g_legacy_warned = false;
static DWORD g_new_t_offset = 0;               // 新規ストロークの t 起点 (既存最終 t + 300ms)

// ドラッグ並べ替え状態 (挿入方式: 間に入れると他がシフトする)
static int g_drag_layer_src = -1;   // ドラッグ元レイヤー (-1=なし)
static int g_drop_gap = -1;         // 挿入位置 (0=L1の上 .. 5=L5の下、-1=範囲外)

// 履歴オペレーション。
// ReplaceStrokes: strokes 全体の入れ替え。描画ジェスチャ (キャンバス出入りで
// ストロークが分割されても 1 オペ)・消しゴム・レイヤー統合/クリア/並べ替えが
// 同一規約で扱える (セッション規模は高々数百ストロークなので全体スナップショットで十分)。
// レイヤーは「設定を含む単位」で扱う (設計確定) ため、
// スロット毎のスタイルと表示状態も before/after に含める。
struct HistoryOp {
    enum class Kind { ReplaceStrokes };
    Kind kind = Kind::ReplaceStrokes;
    std::vector<std::vector<PenPoint>> before;  // ストローク群
    std::vector<std::vector<PenPoint>> after;
    PenLayerStyle styles_before[PEN_LAYER_MAX + 1]; // レイヤー設定
    PenLayerStyle styles_after[PEN_LAYER_MAX + 1];
    bool vis_before[PEN_LAYER_MAX + 1];             // プレビュー表示
    bool vis_after[PEN_LAYER_MAX + 1];
    bool pbh_before[PEN_LAYER_MAX + 1];             // 動画出力 (lhide)
    bool pbh_after[PEN_LAYER_MAX + 1];
};
static std::vector<HistoryOp> undo_stack;
static std::vector<HistoryOp> redo_stack;

// 履歴スタック上限。ReplaceStrokes はセッション全体の before/after を持つため
// 無上限だと長セッションでメモリが二次的に膨らむ。
// 超過分は最古から破棄する (一般的なアプリと同じ「戻れる深さ」の制約)。
static constexpr size_t HISTORY_MAX_OPS = 100;

static void history_push(std::vector<HistoryOp>& stack, HistoryOp&& op) {
    stack.push_back(std::move(op));
    if (stack.size() > HISTORY_MAX_OPS)
        stack.erase(stack.begin(),
                    stack.begin() + static_cast<long>(stack.size() - HISTORY_MAX_OPS));
}

// レイヤー状態 (設定+表示+動画出力) のスナップショット取得
static void capture_layer_state(PenLayerStyle* st_out, bool* vis_out, bool* pbh_out) {
    for (int L = 1; L <= PEN_LAYER_MAX; ++L) {
        st_out[L] = g_layer_styles[L];
        vis_out[L] = g_layer_visible[L];
        pbh_out[L] = g_layer_playback_hidden[L];
    }
}

// 後方宣言 (定義は後の節)
static void rebind_layer_sliders();
static void sync_hsv_from_color();

// レイヤー状態の復元 (Undo/Redo 用)。選択中レイヤーの見た目も同期する。
static void restore_layer_state(const PenLayerStyle* st_in, const bool* vis_in,
                                const bool* pbh_in) {
    for (int L = 1; L <= PEN_LAYER_MAX; ++L) {
        g_layer_styles[L] = st_in[L];
        g_layer_visible[L] = vis_in[L];
        g_layer_playback_hidden[L] = pbh_in[L];
    }
    rebind_layer_sliders();
    sync_hsv_from_color();
}

// ペンモード中のホットキー（Ctrl+Z / Ctrl+Y）。フォーカスが裏のウィンドウ
// （オブジェクト設定等）にあっても吸われずに Undo/Redo を実行するために使う。
#define HOTKEY_UNDO_ID 0x5001
#define HOTKEY_REDO_ID 0x5002

//-----------------------------------------------------------------------------
// プレビュー用のスタイル（ペンツール.obj2 のレイヤー別項目から読み取り、スライダーへバインド）
//-----------------------------------------------------------------------------

// ペンモード開始時のスタイル値スナップショット (レイヤーごと)。
// 確定時に差分だけオブジェクトへ書き戻す (色・太さ・入り抜きの GUI 編集を保存する)。
struct StyleSnapshot {
    bool valid = false;
    COLORREF col = RGB(255, 255, 255);
    double w = 8.0, ti = 0.0, twi = 0.0, to = 0.0, two_ = 0.0;
};
static StyleSnapshot g_style_init[PEN_LAYER_MAX + 1]; // [1..PEN_LAYER_MAX]

// 現在選択中レイヤーの色 (カラーホイール/RGB/HEX 入力と同期)
static COLORREF cur_color() { return g_layer_styles[g_cur_layer].col; }

// 非選択レイヤーを薄く表示する倍率 (の「暗色擬似半透明」フォールバック)。
// 動作確認のフィードバックにより 55% → 35% に強化。
static COLORREF dim_color(COLORREF c) {
    return RGB(GetRValue(c) * 35 / 100, GetGValue(c) * 35 / 100, GetBValue(c) * 35 / 100);
}

// スライダーを現在レイヤーのスタイルへ再バインドする (定義は g_sliders の後)
static void rebind_layer_sliders();

//-----------------------------------------------------------------------------
// フレームレンダリング (EDIT_HANDLE::rendering_scene_video)
//-----------------------------------------------------------------------------

struct RenderCtx {
    unsigned char* dst; // RGBA コピー先 (frame_rgba.data)
    int w, h;
    volatile LONG copied;
    HWND notify_window;
    int frame;
    DWORD started_at;
};

static constexpr UINT WM_CJF_RENDER_COMPLETE = WM_APP + 10;

// レンダリング完了時にイベント通知スレッドから呼ばれる。
// バッファはコールバック中のみ有効なので、即座にコピーして完了を通知する。
static void on_rendered(void* param, int frame, const void* buffer, int width, int height, int pitch) {
    RenderCtx* ctx = static_cast<RenderCtx*>(param);
    if (ctx->dst && buffer && ctx->w == width && ctx->h == height) {
        const unsigned char* src = static_cast<const unsigned char*>(buffer);
        unsigned char* dst = ctx->dst;
        for (int y = 0; y < height; ++y) {
            memcpy(dst + static_cast<size_t>(y) * width * 4,
                   src + static_cast<size_t>(y) * pitch,
                   static_cast<size_t>(width) * 4);
        }
        InterlockedExchange(&ctx->copied, 1);
    }
    if (!PostMessageW(ctx->notify_window, WM_CJF_RENDER_COMPLETE, 0,
                      reinterpret_cast<LPARAM>(ctx)))
        delete ctx;
}

// 指定フレームのシーン出力をレンダリングし、RGBA バッファにコピーする。
// 戻り値: レンダリング受理かつバッファ取得成功なら true。
static bool render_current_frame(int frame, unsigned char* dst, int w, int h) {
    if (!edit_handle) return false;
    RenderCtx* ctx = new (std::nothrow) RenderCtx{ dst, w, h, 0, frame_window, frame, GetTickCount() };
    if (!ctx) return false;
    if (!edit_handle->rendering_scene_video(frame, ctx, on_rendered)) {
        delete ctx;
        return false;
    }
    return true;
}

//-----------------------------------------------------------------------------
// UI 部品レイアウト (client 座標で保持し do_layout で再計算)
//-----------------------------------------------------------------------------

struct MiniSlider {
    const wchar_t* label;
    double* value;
    double vmin, vmax;
    RECT rect;       // 行全体
    RECT track;      // つまみ可動域
    RECT minus_btn;  // −1 ボタン
    RECT plus_btn;   // +1 ボタン
    bool hover;
    bool drag;
    bool hover_minus;
    bool hover_plus;
    bool log_scale = false; // 対数スケール (広レンジ項目: 低値域を精密に)
};

// つまみ位置 u∈[0,1] と値の相互変換。log_scale は対数スケール
// (太さのように vmax/vmin が大きい項目でつまみドラッグを実用化する)
static double slider_u_to_value(const MiniSlider& s, double u) {
    u = clampd(u, 0.0, 1.0);
    if (s.log_scale && s.vmin > 0.0)
        return s.vmin * std::pow(s.vmax / s.vmin, u);
    return s.vmin + u * (s.vmax - s.vmin);
}

static double slider_value_to_u(const MiniSlider& s, double v) {
    const double vv = clampd(v, s.vmin, s.vmax);
    if (s.log_scale && s.vmin > 0.0)
        return std::log(vv / s.vmin) / std::log(s.vmax / s.vmin);
    return (vv - s.vmin) / (s.vmax - s.vmin);
}

enum class ToolKind { Pen = 0, Eraser = 1, Undo = 2, Redo = 3 };
static constexpr int TOOL_BTN_NUM = 4;

// ---- P3 消しゴム ----
// ツール種別。Undo/Redo はボタン索引でありツールではないため使わない。
static ToolKind g_active_tool = ToolKind::Pen;
static double g_eraser_size = 24.0;       // 消しゴム直径 (シーン px)。初回使用時 現在レイヤー太さ×3
static bool g_eraser_size_ready = false;
static constexpr double ERASER_MIN_FRAG_PX = 2.0;   // 微細断片の破棄閾値 (シーン px)
static constexpr double ERASER_SIZE_MIN = 4.0;      // 消しゴム直径の下限 (シーン px)
static constexpr double ERASER_SIZE_MAX = 500.0;    // 同上限
// 消去時刻のバケット幅 (ms)。この間隔でゴースト断片の時刻が分かれるため、
// ゆっくり撫でた消去ほど細かく順番に消える (案 A「時刻分割ゴースト」)
static constexpr double ERASER_TIME_BUCKET_MS = 100.0;

static DWORD erase_bucket_ms(DWORD ms) {
    return static_cast<DWORD>(std::floor(ms / ERASER_TIME_BUCKET_MS) *
                              ERASER_TIME_BUCKET_MS);
}

// ---- P4 タイムライン圧縮 (PenTimeline.h) ----
// 確定時シリアライズで許容するストローク間の最大空白 (ms)。実機運用で 1s を採用。
// INI [UI] GapCapMs で上書き可能 (アプリからは書き戻さない)。
static int g_gap_cap_ms = 1000;
// フレーム内訳ログ ([Perf]) の出力 ON/OFF。INI [UI] PerfLog=1 で有効
static bool g_perf_log = false;

static POINT g_cursor_client = {};        // リングカーソル用の最新カーソル位置
static bool g_cursor_valid = false;

// P2 からは全項目がレイヤー別スタイル。value は現在選択中レイヤーの
// PenLayerStyle メンバーへ動的にバインドする (rebind_layer_sliders)。
// 柔らかさ/手ブレ補正/形状優先は GDI プレビューに反映されないため非表示のまま (将来追加)。
static MiniSlider g_sliders[] = {
    { L"太さ",      &g_layer_styles[1].w,   1.0, 2000.0, {}, {}, {}, {}, false, false, false, false, true },
    { L"入り長さ",  &g_layer_styles[1].ti,  0.0, 50.0,   {}, {}, {}, {}, false, false, false, false },
    { L"入り太さ",  &g_layer_styles[1].twi, 0.0, 100.0,  {}, {}, {}, {}, false, false, false, false },
    { L"抜き長さ",  &g_layer_styles[1].to,  0.0, 50.0,   {}, {}, {}, {}, false, false, false, false },
    { L"抜き太さ",  &g_layer_styles[1].two_, 0.0, 100.0, {}, {}, {}, {}, false, false, false, false },
};
static constexpr int SLIDER_NUM = sizeof(g_sliders) / sizeof(g_sliders[0]);

// 消しゴムサイズ行 (動作確認確定): スライダー行と同一形式。
// index = SLIDER_NUM で既存スライダーと同じ HitZone を流用する。
static MiniSlider g_eraser_slider = {
    L"消しゴム", &g_eraser_size, ERASER_SIZE_MIN, ERASER_SIZE_MAX,
    {}, {}, {}, {}, false, false, false, false, true
};
static MiniSlider* slider_at(int idx) {
    return (idx == SLIDER_NUM) ? &g_eraser_slider : &g_sliders[idx];
}

static void rebind_layer_sliders() {
    PenLayerStyle& s = g_layer_styles[g_cur_layer];
    g_sliders[0].value = &s.w;
    g_sliders[1].value = &s.ti;
    g_sliders[2].value = &s.twi;
    g_sliders[3].value = &s.to;
    g_sliders[4].value = &s.two_;
}

static RECT g_tool_rects[TOOL_BTN_NUM];

// 消しゴムドラッグ状態。base に開始時スナップショットを置き、サンプル追加のたび
// 対象ストロークの消去区間 (累積長空間) を増更新 → rebuild_erase_preview で
// strokes を断片から作り直す。確定 (ペンアップ) 時に ReplaceStrokes 履歴 1 オペ。
static bool erase_down = false;
static std::vector<std::vector<PenPoint>> g_erase_base;
static std::vector<std::vector<pengeom::GeomPoint>> g_erase_geom; // base[i] の幾何変換キャッシュ
static std::vector<pengeom::GeomPoint> g_erase_path;              // 消しゴムサンプル列 (.t = 操作時刻 ms)
static std::vector<std::vector<pengeom::TaggedInterval>> g_erase_iv; // base[i] ごとの消去区間+時刻
static std::vector<bool> g_erase_dot;                             // base[i] をドット抹消
static int g_erase_layer = 1;                                     // 今回の対象レイヤー
static DWORD g_erase_dtime = 0;                                   // 今回ジェスチャ開始時刻 ms (フォールバック用)
static bool g_eraser_outside = false;                             // 消しゴムがキャンバス外にいる間 true
static bool g_erase_need_rebuild = false;                         // 断片再構築が未反映 (提示周期で実行)
static bool g_erase_dirty = false;                                // 何か消えたか (履歴 push 判定)

static RECT g_layer_header_rect = {};
static RECT g_layer_rows[PEN_LAYER_MAX] = {};       // [0]=L1 .. [4]=L5
static RECT g_layer_check_rects[PEN_LAYER_MAX] = {}; // 表示チェックの四角
static RECT g_layer_label_rects[PEN_LAYER_MAX] = {}; // レイヤー名 (右列上)
static RECT g_layer_play_rects[PEN_LAYER_MAX] = {};  // 再生非表示トグル (右列下)
static RECT g_merge_rect = {};                       // 下のレイヤーと統合
static RECT g_clear_rect = {};                       // レイヤーをクリア
static RECT g_apply_all_rect = {};                   // 全レイヤーに適用
static RECT g_preset_rects[10] = {};                 // プリセットスロット 01..10
static RECT g_preset_confirm_rect = {};              // 「確認しない」チェックボックス
static bool g_preset_confirm_disable = false;        // プリセット確認ダイアログの抑制 (アプリ起動中は保持)
static RECT g_sep3_line = {};                        // 色設定 | プリセット の区切り線

// プリセット確認オーバーレイ (本体描画内。別ウィンドウを作らないためチラつかない)
static bool g_confirm_active = false;
static wchar_t g_confirm_msg[128] = {};
static RECT g_cnf_box_rect = {};
static RECT g_cnf_yes_rect = {};
static RECT g_cnf_no_rect = {};
static bool g_confirm_result = false;
static bool g_confirm_done = false;

// ペン設定プリセットの実体 (読込/保存は下のプリセット節)
struct StylePreset {
    bool valid = false;
    COLORREF col = RGB(255, 255, 255);
    double w = 8.0, ti = 0.0, twi = 0.0, to = 0.0, two_ = 0.0;
};
static StylePreset g_presets[11]; // [1..10]
static RECT g_confirm_rect = {};
static RECT g_cancel_rect = {};
static RECT g_bottom_guide_rect = {};
static RECT g_rgb_box_rects[3];   // R/G/B 数値入力 (青枠は親が描画)
static RECT g_hex_box_rect = {};  // #RRGGBB 入力
static HWND g_rgb_edits[3] = {};
static HWND g_hex_edit = {};
constexpr int IDC_EDIT_RGB0 = 240; // R/G/B = RGB0..RGB2
constexpr int IDC_EDIT_HEX = 243;  // #RRGGBB
static int g_sep1_px = 0;         // セパレータ実効位置 (client px)
static int g_sep2_px = 0;
static int g_drag_sep = -1;       // ドラッグ中のセパレータ (-1=なし)
static int g_win_w = 1483;        // ウィンドウサイズ (INI 復元用)
static int g_win_h = 700;

// ---- カラーホイール (HSV Color Wheel + SV Square、クリスタ/AviUtl2 標準式) ----
struct WheelUi {
    RECT wheel_rect;   // ホイール全体 (外接矩形)
    POINT center;
    int r_out, r_in;   // リング外径/内径
    RECT sv_rect;      // 中央の SV 四角
};
static WheelUi g_wheel = { {}, {}, 0, 0, {} };
static double g_hue = 0.0, g_sat = 0.0, g_val = 1.0; // 現在レイヤーの色 (g_layer_styles[g_cur_layer].col) と同期
static bool g_drag_wheel = false;
static bool g_drag_square = false;
// 最後に編集した入力列 (0=RGB 列, 1=HEX 列, -1=未編集)。apply 時の優先判定に使用
static int g_last_color_source = -1;
static void build_wheel_dib();   // 前方宣言
static void build_sv_dib();      // 前方宣言

enum class HitZone { None, Canvas, ToolBtn, Slider, SliderMinus, SliderPlus,
                     LayerRow, LayerCheck, LayerPlay, MergeBtn, ClearBtn,
                     ApplyAllBtn, PresetBtn, PresetConfirmChk,
                     ConfirmYes, ConfirmNo,
                     TitleMax, TitleClose,
                     Confirm, Cancel, WheelRing, WheelSquare,
                     SepH1, SepH2, SepVL, SepVR };
struct HitTarget {
    HitZone zone = HitZone::None;
    int index = -1;
};
static HitTarget g_hover = {}; // ホバー状態 (変化時のみ再描画)

static int g_drag_slider = -1;

static bool point_in_rect(const POINT& pt, const RECT& r) {
    return pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom;
}

static void do_layout() {
    update_view(client_w, client_h);
    int dpi96 = MulDiv(static_cast<int>(g_current_dpi), 96, 96);
    auto S = [&](int v) { return MulDiv(v, static_cast<int>(g_current_dpi), 96); };
    (void)dpi96;

    // ===== 左パネル縦レイアウト =====
    // 基準: タイトルバー直下 (tpx) から下部バー上端まで。
    // 構成: [ツール2x2] ─ s1 ─ [スライダーx5] ─ s2 ─ [ホイール + 色設定一式]
    // 各セクションの必要高をここで定義し、境界 (s1/s2) とホイール径を導出する。
    // ホイールは「残り縦スペース」に応じて伸縮するため、どのサイズでも
    // 下に死にスペースが出ない (旧実装はホイール固定径で余っていた)。
    const int tpx = title_px();
    int lw = left_panel_px();
    int panel_top = tpx;
    int panel_bottom = client_h - BAR_BOTTOM_H;

    const int TOOL_BAND_MIN   = S(108); // 4+ボタン30+スライダー行26+ボタン30+行間x2+区切り余白8
    const int SLIDER_BAND_MIN = S(142); // 上6 + 5行 + 下2
    const int WHEEL_GAP       = S(6);   // 区切り線〜ホイール上端
    const int COLOR_ROWS_H    = S(56);  // ホイール下端〜区切り線 (RGB行+HEX行)
    const int WHEEL_MIN       = S(60);  // ホイール最小径
    const int PRESET_BLOCK_H  = S(111); // 区切り線〜確認チェック下端 (下端寄せ)

    // プリセットブロック (区切り線〜確認チェック) はパネル下端に寄せる
    // (設計確定)。余白は HEX 行とブロックの間に抜け、
    // カラーホイールがそのぶん大きくなる。
    int preset_block_top = panel_bottom - PRESET_BLOCK_H;
    int wheel_bottom_max = preset_block_top - COLOR_ROWS_H;

    // --- s1: ツール | スライダー境界 (INI 値はタイトル無し時代の絶対 y なので +tpx) ---
    int s1_lo = panel_top + TOOL_BAND_MIN;
    int s1_hi = std::max(s1_lo, wheel_bottom_max - WHEEL_GAP - SLIDER_BAND_MIN);
    int s1 = clamp_int(dpi_s(g_sep_tool_y) + tpx, s1_lo, s1_hi);

    // --- s2: スライダー | ホイール境界 ---
    int s2_lo = s1 + SLIDER_BAND_MIN;
    int s2_hi = std::max(s2_lo, wheel_bottom_max - WHEEL_GAP);
    int s2 = clamp_int(dpi_s(g_sep_wheel_y) + tpx, s2_lo, s2_hi);

    g_sep1_px = s1;
    g_sep2_px = s2;

    // ツールボタン: 2x2 グリッド (設計確定)。
    // 左上から [ペン][消しゴム] / [元に戻す][やり直し]。
    // 行間は小さく固定して上詰めで並べる (帯いっぱいに中央配置すると
    // 2 段の間が空きすぎるため。余白は区切り線側に残す)。
    {
        const int tool_margin = S(8);   // 区切り線との余白
        const int gap_t = S(4);
        const int btn_h = S(30);
        int bw_t = (lw - S(10) * 2 - gap_t) / 2;
        int band_h = s1 - panel_top - tool_margin;
        // 縦積み: [ペン|消しゴム] → 消しゴムサイズ行 (スライダー行と同形式) →
        // [元に戻す|やり直し]。設計確定。
        const int top_pad = S(4);
        const int row_h = S(26);       // スライダー行と同一の高さ
        int slack = band_h - btn_h * 2 - row_h - top_pad;
        int gap_v = clamp_int(slack / 2, S(1), S(6));
        int y0 = panel_top + top_pad;
        g_tool_rects[0] = { S(10), y0, S(10) + bw_t, y0 + btn_h };                       // ペン
        g_tool_rects[1] = { S(10) + bw_t + gap_t, y0,
                            S(10) + bw_t * 2 + gap_t, y0 + btn_h };                      // 消しゴム
        int ym = y0 + btn_h + gap_v;
        // スライダー行と同じ内訳 [ラベル|−|トラック|+|値] で配置する
        MiniSlider& s = g_eraser_slider;
        s.rect = { S(6), ym, lw - S(6), ym + row_h };
        int label_w = S(64);
        int step_w = S(18);
        int value_w = S(34);
        int gap = S(2);
        int mid_y = (s.rect.top + s.rect.bottom) / 2;
        s.minus_btn = { s.rect.left + label_w, mid_y - step_w / 2,
                        s.rect.left + label_w + step_w, mid_y - step_w / 2 + step_w };
        s.plus_btn = { s.rect.right - value_w - step_w, s.minus_btn.top,
                       s.rect.right - value_w, s.minus_btn.bottom };
        int track_left = s.minus_btn.right + gap;
        int track_right = s.plus_btn.left - gap;
        s.track = { track_left, s.rect.top + (row_h - S(14)) / 2,
                    std::max(track_left + 4, track_right),
                    s.rect.top + (row_h - S(14)) / 2 + S(14) };
        int y1 = ym + row_h + gap_v;
        g_tool_rects[2] = { S(10), y1, S(10) + bw_t, y1 + btn_h };                       // 元に戻す
        g_tool_rects[3] = { S(10) + bw_t + gap_t, y1,
                            S(10) + bw_t * 2 + gap_t, y1 + btn_h };                      // やり直し
    }

    for (int i = 0; i < SLIDER_NUM; ++i) {
        MiniSlider& s = g_sliders[i];
        // 帯の上下にマージンを確保し、区切り線やツール領域と重ならないようにする。
        // (旧実装は中央寄せオフセットが負になり、先頭の「太さ」が
        // 区切り線・やり直しボタン側へ食い込むことがあった)
        const int pad_top_s = S(6);
        const int pad_bot_s = S(2);
        // 下限ガード: 想定外の極小ジオメトリで複数行が同一 y に堆積しないようにする
        int rh = std::max(S(18), (s2 - s1 - pad_top_s - pad_bot_s) / SLIDER_NUM);
        int ry = s1 + pad_top_s + i * rh + std::max(0, (rh - S(26)) / 2);
        s.rect = { S(6), ry, lw - S(6), ry + S(26) };
        int label_w = S(64); // 「入り長さ(L1)」等が欠けない幅 (設計確定)
        int step_w = S(18);
        int value_w = S(34);
        int gap = S(2);
        int row_left = s.rect.left;
        int row_right = s.rect.right;
        s.minus_btn = { row_left + label_w, (s.rect.top + s.rect.bottom) / 2 - step_w / 2,
                        row_left + label_w + step_w,
                        (s.rect.top + s.rect.bottom) / 2 - step_w / 2 + step_w };
        s.plus_btn = { row_right - value_w - step_w, s.minus_btn.top,
                       row_right - value_w, s.minus_btn.bottom };
        int track_left = s.minus_btn.right + gap;
        int track_right = s.plus_btn.left - gap;
        s.track = { track_left, s.rect.top + (S(26) - S(14)) / 2,
                    std::max(track_left + 4, track_right),
                    s.rect.top + (S(26) - S(14)) / 2 + S(14) };
    }

    // カラーホイール (ペン設定のすぐ下) + RGB/HEX 入力行。
    // 径 = 幅基準 (パネル幅-余白) と「s2 〜 プリセットブロック上端までの縦空間
    // - RGB/HEX 行分」の小さい方。縦が足りないときはホイールが縮み、
    // 下に死にスペースを出さない。プリセットブロック下端寄せにより、
    // 余白は HEX 行と区切り線の間に抜けてホイールが大きくなる。
    int wheel_max_by_w = lw - S(16);
    int wheel_space = wheel_bottom_max - (s2 + WHEEL_GAP);
    int wh = clamp_int(std::min(wheel_max_by_w, wheel_space), WHEEL_MIN, wheel_max_by_w);
    int wx = S(8);
    int wy = s2 + WHEEL_GAP;
    g_wheel.wheel_rect = { wx, wy, wx + wh, wy + wh };
    g_wheel.center = { (g_wheel.wheel_rect.left + g_wheel.wheel_rect.right) / 2,
                       (g_wheel.wheel_rect.top + g_wheel.wheel_rect.bottom) / 2 };
    g_wheel.r_out = wh / 2 - S(2); // DIB 端でのクリップ防止のため余白を残す
    g_wheel.r_in = std::max(S(24), g_wheel.r_out - S(18)); // リング厚み S(18)
    double sqd = g_wheel.r_in * 2.0 / std::sqrt(2.0); // 内接正方形の対角線
    int sq = static_cast<int>(sqd * 0.92); // 少し小さめ
    g_wheel.sv_rect = { g_wheel.center.x - sq / 2, g_wheel.center.y - sq / 2,
                        g_wheel.center.x - sq / 2 + sq, g_wheel.center.y - sq / 2 + sq };
    build_wheel_dib();
    build_sv_dib();

    // RGB 数値行 (3 セル均等・左寄せ)
    int rgb_row_y = g_wheel.wheel_rect.bottom + S(10);
    int content_l = S(8);
    int content_r = lw - S(8);
    int content_w = content_r - content_l;
    int cell_w = content_w / 3;
    for (int i = 0; i < 3; ++i) {
        int cx0 = content_l + i * cell_w;
        // ラベル (R/G/B) は draw_left_panel がセル左に描くため EDIT はその右側
        int right = (i == 2) ? content_r : (cx0 + cell_w - dpi_s(2));
        g_rgb_box_rects[i] = { cx0 + dpi_s(12), rgb_row_y,
                               right, rgb_row_y + dpi_s(20) };
    }
    // HEX 入力行: 左端は R box の左端 (R edit 左端) に揃え、# ラベルはその左
    int hex_row_y = rgb_row_y + S(26);
    g_hex_box_rect = { content_l, hex_row_y, content_r, hex_row_y + S(20) };

    // 区切り線 (色設定 | プリセット) + 設定適用ボタン列。
    // ブロック全体はパネル下端に寄せて固定する (設計確定)。
    // 余白は HEX 行とこの区切り線の間に抜ける。
    g_sep3_line = { content_l, preset_block_top, content_r, preset_block_top + S(2) };
    int ex_row_y = preset_block_top + S(11);
    g_apply_all_rect = { content_l, ex_row_y, content_r, ex_row_y + S(22) };
    const int pw_ = (content_w - S(4) * 4) / 5;
    for (int i = 0; i < 10; ++i) {
        int row = i / 5, colI = i % 5;
        int bx = content_l + colI * (pw_ + S(4));
        int by = ex_row_y + S(26) + row * S(24);
        g_preset_rects[i] = { bx, by, bx + pw_, by + S(20) };
    }
    // 「確認しない」チェックボックス (ガイド行の下)
    g_preset_confirm_rect = { content_l, ex_row_y + S(88), content_l + dpi_s(96),
                              ex_row_y + S(100) };

    // EDIT コントロール再配置 (枠なし EDIT を青枠の内側へ)
    auto place_edit = [&](HWND h, const RECT& r) {
        if (h) {
            MoveWindow(h, r.left + S(1), r.top + S(1),
                       (r.right - r.left) - S(2), (r.bottom - r.top) - S(2), TRUE);
            // 移動後の残像防止 (区切りドラッグ中に他の box の内容が
            // 見える問題。明示的に再描画を要求する)
            InvalidateRect(h, nullptr, TRUE);
        }
    };
    for (int i = 0; i < 3; ++i) place_edit(g_rgb_edits[i], g_rgb_box_rects[i]);
    place_edit(g_hex_edit, g_hex_box_rect);

    // 右パネル: レイヤーリスト (5 行固定) + 統合/クリアボタン。
    // 設計確定: 高さに余裕があるため行を高く取り、16:9 の線が
    // 収まる大きなサムネイル + 再生非表示トグル列を設ける。
    // 統合/クリアは 2 行に分けて意味の分かるラベルにする。
    int rx = client_w - right_panel_px();
    g_layer_header_rect = { rx + S(8), S(10) + tpx, rx + right_panel_px() - S(8), S(30) + tpx };
    int ry = S(34) + tpx;
    const int row_h = S(72);
    const int row_gap = S(3);
    for (int i = 0; i < PEN_LAYER_MAX; ++i) {
        g_layer_rows[i] = { rx + S(8), ry, rx + right_panel_px() - S(8), ry + row_h };
        int ck_h = S(15);
        g_layer_check_rects[i] = { rx + S(12), ry + (row_h - ck_h) / 2,
                                   rx + S(27), ry + (row_h - ck_h) / 2 + ck_h };
        // 右列: レイヤー名 (上) + 再生トグル ▶ (下。ON=青系 / OFF=灰色)
        int col_l = rx + right_panel_px() - S(52);
        g_layer_label_rects[i] = { col_l, ry + S(6), rx + right_panel_px() - S(8), ry + S(30) };
        g_layer_play_rects[i] = { col_l, ry + S(36), col_l + S(40), ry + row_h - S(8) };
        ry += row_h + row_gap;
    }
    int btn_y = ry + S(4);
    g_merge_rect = { rx + S(8), btn_y, rx + right_panel_px() - S(8), btn_y + S(24) };
    g_clear_rect = { rx + S(8), btn_y + S(28), rx + right_panel_px() - S(8), btn_y + S(52) };

    // 確認オーバーレイ表示中はレイアウト変更に追従して再配置する (指摘 R4)
    if (g_confirm_active) layout_confirm_overlay();

    // カスタムタイトルバーのキャプションボタン (右端から 閉じる/最大化。
    // 左から見た並びは [最大化][閉じる])
    {
        int tb = title_px();
        for (int i = 0; i < 2; ++i) {
            int bx = client_w - tb * (2 - i);
            g_title_btn_rects[i] = { bx, 0, bx + tb, tb };
        }
    }

    // 下部バー: 確定 / キャンセル (中央寄せ)
    int bar_top = client_h - BAR_BOTTOM_H;
    int btn_w = S(150), btn_h = S(30);
    int cx = client_w / 2;
    g_confirm_rect = { cx - btn_w - S(10), bar_top + (BAR_BOTTOM_H - btn_h) / 2,
                       cx - S(10), bar_top + (BAR_BOTTOM_H - btn_h) / 2 + btn_h };
    g_cancel_rect = { cx + S(10), g_confirm_rect.top, cx + S(10) + btn_w, g_confirm_rect.bottom };
    g_bottom_guide_rect = { S(10), bar_top, cx - btn_w - S(20), client_h };
}

static HitTarget hit_test(POINT pt) {
    // 確認オーバーレイ表示中はそのボタンのみを判定する
    if (g_confirm_active) {
        if (point_in_rect(pt, g_cnf_yes_rect)) return { HitZone::ConfirmYes, 0 };
        if (point_in_rect(pt, g_cnf_no_rect)) return { HitZone::ConfirmNo, 0 };
        return { HitZone::None, -1 };
    }
    // カスタムタイトルバー (最優先。ボタン以外は HTCAPTION 側で処理)
    if (pt.y < title_px()) {
        for (int i = 0; i < 2; ++i)
            if (point_in_rect(pt, g_title_btn_rects[i]))
                return { i == 0 ? HitZone::TitleMax : HitZone::TitleClose, 0 };
        return { HitZone::None, -1 };
    }
    // セパレータ (ドラッグ調整ハンドル)。他要素より優先。
    if (std::abs(pt.x - left_panel_px()) <= 3 && pt.y >= title_px() &&
        pt.y < client_h - BAR_BOTTOM_H)
        return { HitZone::SepVL, 0 };
    if (pt.x >= client_w - right_panel_px() - 3 && pt.x <= client_w - right_panel_px() + 3 &&
        pt.y < client_h - BAR_BOTTOM_H)
        return { HitZone::SepVR, 0 };
    if (pt.x < left_panel_px()) {
        if (std::abs(pt.y - g_sep1_px) <= 3 && pt.y >= title_px())
        return { HitZone::SepH1, 0 };
        if (std::abs(pt.y - g_sep2_px) <= 3) return { HitZone::SepH2, 0 };
    }
    for (int i = 0; i < SLIDER_NUM; ++i) {
        if (point_in_rect(pt, g_sliders[i].minus_btn)) return { HitZone::SliderMinus, i };
        if (point_in_rect(pt, g_sliders[i].plus_btn)) return { HitZone::SliderPlus, i };
        if (point_in_rect(pt, g_sliders[i].rect)) return { HitZone::Slider, i };
    }
    // 消しゴムサイズ行 (index = SLIDER_NUM で既存ゾーンを流用)
    if (point_in_rect(pt, g_eraser_slider.minus_btn)) return { HitZone::SliderMinus, SLIDER_NUM };
    if (point_in_rect(pt, g_eraser_slider.plus_btn)) return { HitZone::SliderPlus, SLIDER_NUM };
    if (point_in_rect(pt, g_eraser_slider.rect)) return { HitZone::Slider, SLIDER_NUM };
    for (int i = 0; i < TOOL_BTN_NUM; ++i)
        if (point_in_rect(pt, g_tool_rects[i])) return { HitZone::ToolBtn, i };
    if (point_in_rect(pt, g_confirm_rect)) return { HitZone::Confirm, 0 };
    if (point_in_rect(pt, g_cancel_rect)) return { HitZone::Cancel, 0 };
    // カラーホイール: SV 四角 → リング帯 → ホイール内のその他 (無操作)
    if (point_in_rect(pt, g_wheel.sv_rect) && point_in_rect(pt, g_wheel.wheel_rect))
        return { HitZone::WheelSquare, 0 };
    if (point_in_rect(pt, g_wheel.wheel_rect)) {
        double dx = pt.x - g_wheel.center.x;
        double dy = pt.y - g_wheel.center.y;
        double r = std::sqrt(dx * dx + dy * dy);
        if (r >= g_wheel.r_in - 2 && r <= g_wheel.r_out + 2)
            return { HitZone::WheelRing, 0 };
        return { HitZone::None, -1 };
    }
    if (point_in_rect(pt, g_layer_header_rect)) return { HitZone::None, -1 };
    if (point_in_rect(pt, g_merge_rect)) return { HitZone::MergeBtn, 0 };
    if (point_in_rect(pt, g_clear_rect)) return { HitZone::ClearBtn, 0 };
    for (int i = PEN_LAYER_MAX - 1; i >= 0; --i) {
        if (point_in_rect(pt, g_layer_play_rects[i])) return { HitZone::LayerPlay, i };
        if (point_in_rect(pt, g_layer_check_rects[i])) return { HitZone::LayerCheck, i };
        if (point_in_rect(pt, g_layer_rows[i])) return { HitZone::LayerRow, i };
    }
    if (point_in_rect(pt, g_apply_all_rect)) return { HitZone::ApplyAllBtn, 0 };
    for (int i = 9; i >= 0; --i)
        if (point_in_rect(pt, g_preset_rects[i])) return { HitZone::PresetBtn, i };
    if (point_in_rect(pt, g_preset_confirm_rect)) return { HitZone::PresetConfirmChk, 0 };
    if (pt.x >= g_view.area.left && pt.x < g_view.area.right &&
        pt.y >= g_view.area.top && pt.y < g_view.area.bottom)
        return { HitZone::Canvas, 0 };
    return { HitZone::None, -1 };
}

//-----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// カラーホイール: HSV 変換・DIB 生成
// -----------------------------------------------------------------------------

static void hsv_to_rgb(double h, double s, double v, COLORREF* out) {
    h = std::fmod(h, 360.0);
    if (h < 0) h += 360.0;
    s = clampd(s, 0.0, 1.0);
    v = clampd(v, 0.0, 1.0);
    double c = v * s;
    double x = c * (1.0 - std::fabs(std::fmod(h / 60.0, 2.0) - 1.0));
    double m = v - c;
    double r = 0, g = 0, b = 0;
    if (h < 60)       { r = c; g = x; b = 0; }
    else if (h < 120) { r = x; g = c; b = 0; }
    else if (h < 180) { r = 0; g = c; b = x; }
    else if (h < 240) { r = 0; g = x; b = c; }
    else if (h < 300) { r = x; g = 0; b = c; }
    else              { r = c; g = 0; b = x; }
    *out = RGB(static_cast<int>(std::lround((r + m) * 255)),
               static_cast<int>(std::lround((g + m) * 255)),
               static_cast<int>(std::lround((b + m) * 255)));
}

static void rgb_to_hsv(COLORREF c, double* h, double* s, double* v) {
    double rr = GetRValue(c) / 255.0, gg = GetGValue(c) / 255.0, bb = GetBValue(c) / 255.0;
    double mx = std::max({ rr, gg, bb }), mn = std::min({ rr, gg, bb });
    double d = mx - mn;
    *v = mx;
    *s = (mx > 0.000001) ? d / mx : 0.0;
    if (d <= 0.000001) { *h = 0.0; return; }
    double hh;
    if (mx == rr)      hh = std::fmod((gg - bb) / d, 6.0);
    else if (mx == gg) hh = (bb - rr) / d + 2.0;
    else               hh = (rr - gg) / d + 4.0;
    hh *= 60.0;
    if (hh < 0) hh += 360.0;
    *h = hh;
}

// 色相角度: 12時=黄(60°)、3時=150°(黄緑/水色境)、6時=青(240°)、9時=330°(赤/紫境)
static double wheel_dx_dy_to_hue(double dx, double dy) {
    double theta = std::atan2(dx, -dy); // 12時基準・時計回り
    if (theta < 0) theta += 6.28318530717959;
    return std::fmod(theta * 180.0 / 3.14159265358979 + 60.0, 360.0);
}

// canvas 表示画像生成 (シーン画像 → image_rect サイズへバイリニア縮小)
//-----------------------------------------------------------------------------

static void build_canvas_image() {
    // シーン画像の再サンプルはキャッシュ (シーン+確定ストローク合成) を
    // 無効化する。これが無いとリサイズ直後のプレビューが古い解像度/位置の
    // まま残る (デバウンス後に再構築されるまでの間も含む)
    mark_canvas_dirty();
    int cw = g_view.image_rect.right - g_view.image_rect.left;
    int ch = g_view.image_rect.bottom - g_view.image_rect.top;
    canvas_img.assign(static_cast<size_t>(cw) * ch * 4, 0);
    canvas_img_w = cw;
    canvas_img_h = ch;
    if (frame_w < 1 || frame_h < 1 || cw < 1 || ch < 1) return;

    float sx_scale = static_cast<float>(frame_w) / static_cast<float>(cw);
    float sy_scale = static_cast<float>(frame_h) / static_cast<float>(ch);
    for (int y = 0; y < ch; ++y) {
        float sy = ((static_cast<float>(y) + 0.5f) * sy_scale) - 0.5f;
        int y0 = clamp_int(static_cast<int>(std::floorf(sy)), 0, frame_h - 1);
        int y1 = clamp_int(y0 + 1, 0, frame_h - 1);
        float fy = std::max(0.0f, std::min(1.0f, sy - static_cast<float>(y0)));
        for (int x = 0; x < cw; ++x) {
            float sx = ((static_cast<float>(x) + 0.5f) * sx_scale) - 0.5f;
            int x0 = clamp_int(static_cast<int>(std::floorf(sx)), 0, frame_w - 1);
            int x1 = clamp_int(x0 + 1, 0, frame_w - 1);
            float fx = std::max(0.0f, std::min(1.0f, sx - static_cast<float>(x0)));

            const unsigned char* p00 = frame_rgba.data() + (static_cast<size_t>(y0) * frame_w + x0) * 4;
            const unsigned char* p10 = frame_rgba.data() + (static_cast<size_t>(y0) * frame_w + x1) * 4;
            const unsigned char* p01 = frame_rgba.data() + (static_cast<size_t>(y1) * frame_w + x0) * 4;
            const unsigned char* p11 = frame_rgba.data() + (static_cast<size_t>(y1) * frame_w + x1) * 4;

            unsigned char* d = canvas_img.data() + (static_cast<size_t>(y) * cw + x) * 4;
            for (int c = 0; c < 3; ++c) {
                float v = (p00[c] * (1 - fx) + p10[c] * fx) * (1 - fy) +
                          (p01[c] * (1 - fx) + p11[c] * fx) * fy;
                d[2 - c] = static_cast<unsigned char>(v + 0.5f);
            }
            d[3] = 255;
        }
    }
}

//-----------------------------------------------------------------------------
// UI 描画
//-----------------------------------------------------------------------------

// 使用色のブラシをキャッシュして高速化 (ドラッグ中の高頻度再描画対策)
static HBRUSH get_cached_brush(COLORREF c) {
    struct Entry { COLORREF c; HBRUSH b; };
    static Entry cache[24] = {};
    static int count = 0;
    for (int i = 0; i < count; ++i)
        if (cache[i].c == c && cache[i].b) return cache[i].b;
    HBRUSH b = CreateSolidBrush(c);
    if (count < 24) {
        cache[count++] = { c, b };
        return b;
    }
    // キャッシュ満杯時は都度生成 (呼び出し側は Delete しない運用のため僅かにリークするが
    // 実使用色は 24 色に収まる)
    return b;
}

// 幅付きペンのキャッシュ。テーパー分割描画は 1 ストロークあたり最大 128 本の
// 幅違いペンを必要とし、CreatePen(PS_GEOMETRIC) の生成コストがプレビューの
// ボトルネックになっていた (P4 レビュー 対策 A)。キー = (スタイル, 幅, 色)。
// 呼び出し側は DeleteObject しないこと。満杯後は古いエントリをリングで再利用
// (ペンは描画関数内で選択→復元されるため、解放タイミングは選択外であることが保証される)
static HPEN get_cached_pen(UINT style, int width, COLORREF c) {
    struct Entry { UINT style; int width; COLORREF c; HPEN p; };
    static Entry cache[256] = {};
    static int count = 0;
    static int next_evict = 0;
    for (int i = 0; i < count; ++i)
        if (cache[i].width == width && cache[i].c == c && cache[i].style == style)
            return cache[i].p;
    HPEN p = CreatePen(style, width, c);
    if (!p) return nullptr;
    if (count < 256) {
        cache[count++] = { style, width, c, p };
    } else {
        Entry& e = cache[next_evict];
        if (e.p) DeleteObject(e.p);
        e = { style, width, c, p };
        next_evict = (next_evict + 1) % 256;
    }
    return p;
}

static void fill_rect_dc(HDC dc, const RECT& rc, COLORREF c) {
    FillRect(dc, &rc, get_cached_brush(c));
}

// 青系 1px 枠 (数値入力ボックス等)
static void draw_accent_frame(HDC dc, const RECT& rc) {
    HPEN pen = CreatePen(PS_SOLID, 1, COL_SEL_ACCENT);
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
}

static void draw_text_dc(HDC dc, const RECT& rc, const wchar_t* text, COLORREF color,
                         HFONT font, UINT format) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    HGDIOBJ old = SelectObject(dc, font);
    DrawTextW(dc, text, -1, const_cast<RECT*>(&rc), format | DT_NOPREFIX | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, old);
}

static void draw_button_face(HDC dc, const RECT& rc, const wchar_t* text, bool enabled,
                             bool hovered, bool pressed, bool accent) {
    // ダーク背景 + 青系枠 + 白文字で統一 (利用者指定)
    (void)accent;
    COLORREF bg = !enabled ? COL_BTN_NORMAL
                  : pressed ? RGB(18, 52, 100)   // アクティブ (押下中/選択中)
                  : hovered ? COL_BTN_HOVER
                  : COL_BTN_NORMAL;
    fill_rect_dc(dc, rc, bg);
    COLORREF border_col = enabled ? (pressed ? RGB(70, 150, 235) : COL_SEL_ACCENT)
                                  : COL_BTN_NORMAL;
    HPEN pen = CreatePen(PS_SOLID, pressed ? 2 : 1, border_col);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH)); // 内部は塗らず枠のみ
    Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen); // GDI リーク防止 (redraw 毎に生成するため必須)
    draw_text_dc(dc, rc, text, enabled ? COL_TEXT_MAIN : COL_TEXT_DIM,
                 g_font_text, DT_CENTER);
}

// ホイール DIB 生成: リング帯域 hue スペクトル。2x スーパーサンプリングでジャギー軽減
static HDC g_wheel_dc = nullptr;
static HBITMAP g_wheel_bmp = nullptr;
static void* g_wheel_bits = nullptr;
static int g_wheel_dw = 0, g_wheel_dh = 0;

static void build_wheel_dib();   // 前方宣言 (do_layout から呼ぶ)
static void build_sv_dib();      // 前方宣言
static void build_wheel_dib() {
    int w = g_wheel.wheel_rect.right - g_wheel.wheel_rect.left;
    int h = g_wheel.wheel_rect.bottom - g_wheel.wheel_rect.top;
    if (w < 8 || h < 8) return;
    // 静的ホイールのためサイズ不変なら再計算不要
    if (g_wheel_dc && g_wheel_dw == w && g_wheel_dh == h) return;
    if (!g_wheel_dc || g_wheel_dw != w || g_wheel_dh != h) {
        if (g_wheel_bmp) { DeleteObject(g_wheel_bmp); g_wheel_bmp = nullptr; }
        if (g_wheel_dc) { DeleteDC(g_wheel_dc); g_wheel_dc = nullptr; }
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        g_wheel_dc = CreateCompatibleDC(nullptr);
        g_wheel_bmp = CreateDIBSection(g_wheel_dc, &bmi, DIB_RGB_COLORS, &g_wheel_bits, nullptr, 0);
        if (g_wheel_dc && g_wheel_bmp) SelectObject(g_wheel_dc, g_wheel_bmp); // ビットマップを DC へ選択
        g_wheel_dw = w;
        g_wheel_dh = h;
    }
    if (!g_wheel_dc || !g_wheel_bits) return;
    unsigned char (*px)[4] = static_cast<unsigned char (*)[4]>(g_wheel_bits);
    double cx = w / 2.0, cy = h / 2.0;
    const int SS = 3; // 3x3 スーパーサンプリング (ジャギー軽減)
    const double SSF = SS * SS;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double ar = 0, ag = 0, ab = 0;
            for (int sy = 0; sy < SS; ++sy) {
                for (int sx = 0; sx < SS; ++sx) {
                    double pxs = x + (sx + 0.5) / SS;
                    double pys = y + (sy + 0.5) / SS;
                    double ddx = pxs - cx, ddy = pys - cy;
                    double r = std::sqrt(ddx * ddx + ddy * ddy);
                    COLORREF cc2;
                    if (r >= g_wheel.r_in - 1 && r <= g_wheel.r_out + 1) {
                        hsv_to_rgb(wheel_dx_dy_to_hue(ddx, ddy), 1.0, 1.0, &cc2);
                    } else {
                        cc2 = COL_BG_RAIL;
                    }
                    ar += GetRValue(cc2); ag += GetGValue(cc2); ab += GetBValue(cc2);
                }
            }
            unsigned char* p = px[y * w + x];
            p[0] = static_cast<unsigned char>(ab / SSF + 0.5);
            p[1] = static_cast<unsigned char>(ag / SSF + 0.5);
            p[2] = static_cast<unsigned char>(ar / SSF + 0.5);
            p[3] = 255;
        }
    }
}

// SV 四角 DIB 生成: 現在の hue で S(横)×V(縦) グラデーション
static HDC g_sv_dc = nullptr;
static HBITMAP g_sv_bmp = nullptr;
static void* g_sv_bits = nullptr;
static int g_sv_dw = 0, g_sv_dh = 0;

static void build_sv_dib() {
    int w = g_wheel.sv_rect.right - g_wheel.sv_rect.left;
    int h = g_wheel.sv_rect.bottom - g_wheel.sv_rect.top;
    if (w < 4 || h < 4) return;
    if (!g_sv_dc || g_sv_dw != w || g_sv_dh != h) {
        if (g_sv_bmp) { DeleteObject(g_sv_bmp); g_sv_bmp = nullptr; }
        if (g_sv_dc) { DeleteDC(g_sv_dc); g_sv_dc = nullptr; }
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        g_sv_dc = CreateCompatibleDC(nullptr);
        g_sv_bmp = CreateDIBSection(g_sv_dc, &bmi, DIB_RGB_COLORS, &g_sv_bits, nullptr, 0);
        if (g_sv_dc && g_sv_bmp) SelectObject(g_sv_dc, g_sv_bmp); // ビットマップを DC へ選択
        g_sv_dw = w;
        g_sv_dh = h;
    }
    if (!g_sv_dc || !g_sv_bits) return;
    unsigned char (*px)[4] = static_cast<unsigned char (*)[4]>(g_sv_bits);
    for (int y = 0; y < h; ++y) {
        double vv = 1.0 - static_cast<double>(y) / (h - 1);
        for (int x = 0; x < w; ++x) {
            double ss = static_cast<double>(x) / (w - 1);
            COLORREF cc2;
            hsv_to_rgb(g_hue, ss, vv, &cc2);
            px[y * w + x][0] = GetBValue(cc2);
            px[y * w + x][1] = GetGValue(cc2);
            px[y * w + x][2] = GetRValue(cc2);
            px[y * w + x][3] = 255;
        }
    }
}

// ホイール/SV 四角を dc へ描画 (マーカー込み)
static void draw_color_wheel(HDC dc) {
    if (g_wheel_dc && g_wheel_dw == g_wheel.wheel_rect.right - g_wheel.wheel_rect.left &&
        g_wheel_dh == g_wheel.wheel_rect.bottom - g_wheel.wheel_rect.top) {
        BitBlt(dc, g_wheel.wheel_rect.left, g_wheel.wheel_rect.top,
               g_wheel.wheel_rect.right - g_wheel.wheel_rect.left,
               g_wheel.wheel_rect.bottom - g_wheel.wheel_rect.top,
               g_wheel_dc, 0, 0, SRCCOPY);
    }
    if (g_sv_dc && g_sv_dw == g_wheel.sv_rect.right - g_wheel.sv_rect.left &&
        g_sv_dh == g_wheel.sv_rect.bottom - g_wheel.sv_rect.top) {
        BitBlt(dc, g_wheel.sv_rect.left, g_wheel.sv_rect.top,
               g_wheel.sv_rect.right - g_wheel.sv_rect.left,
               g_wheel.sv_rect.bottom - g_wheel.sv_rect.top,
               g_sv_dc, 0, 0, SRCCOPY);
        // SV マーカー: 白円+黒枠
        double sw = std::max(1.0, static_cast<double>(
            g_wheel.sv_rect.right - g_wheel.sv_rect.left - 1));
        double sh = std::max(1.0, static_cast<double>(
            g_wheel.sv_rect.bottom - g_wheel.sv_rect.top - 1));
        POINT sp = { g_wheel.sv_rect.left + static_cast<int>(std::lround(g_sat * sw)),
                     g_wheel.sv_rect.top + static_cast<int>(std::lround((1.0 - g_val) * sh)) };
        HBRUSH wb = get_cached_brush(RGB(255, 255, 255));
        HPEN bp = CreatePen(PS_SOLID, 1, RGB(20, 20, 20));
        HGDIOBJ ob = SelectObject(dc, wb);
        HGDIOBJ opn = SelectObject(dc, bp);
        Ellipse(dc, sp.x - 4, sp.y - 4, sp.x + 4, sp.y + 4);
        SelectObject(dc, ob);
        SelectObject(dc, opn);
        DeleteObject(bp);
    }
    // Hue マーカー: リング上の白点
    double rad = (g_hue - 60.0) * 3.14159265358979 / 180.0;
    double rm = (g_wheel.r_in + g_wheel.r_out) / 2.0;
    POINT hp = { g_wheel.center.x + static_cast<int>(std::lround(std::sin(rad) * rm)),
                 g_wheel.center.y - static_cast<int>(std::lround(std::cos(rad) * rm)) };
    HBRUSH wb2 = get_cached_brush(RGB(255, 255, 255));
    HGDIOBJ ob2 = SelectObject(dc, wb2);
    Ellipse(dc, hp.x - 3, hp.y - 3, hp.x + 3, hp.y + 3);
    SelectObject(dc, ob2);
}

// -----------------------------------------------------------------------------
// カラーホイール・RGB/HEX 入力の操作
// -----------------------------------------------------------------------------

static void update_hue_from_pt(POINT pt) {
    double dx = pt.x - g_wheel.center.x;
    double dy = pt.y - g_wheel.center.y;
    double nh = wheel_dx_dy_to_hue(dx, dy);
    if (std::fabs(nh - g_hue) < 0.0001) return;
    g_hue = nh;
    COLORREF c;
    hsv_to_rgb(g_hue, g_sat, g_val, &c);
    g_layer_styles[g_cur_layer].col = c;
    build_sv_dib();
    update_color_box_texts();
    redraw_frame();
}

static void update_sv_from_pt(POINT pt) {
    const RECT& r = g_wheel.sv_rect;
    double w = std::max(1.0, static_cast<double>(r.right - r.left - 1));
    double h = std::max(1.0, static_cast<double>(r.bottom - r.top - 1));
    g_sat = clampd((pt.x - r.left) / w, 0.0, 1.0);
    g_val = clampd(1.0 - (pt.y - r.top) / h, 0.0, 1.0);
    COLORREF c;
    hsv_to_rgb(g_hue, g_sat, g_val, &c);
    if (c != cur_color()) {
        g_layer_styles[g_cur_layer].col = c;
        update_color_box_texts();
        redraw_frame();
    }
}

static void sync_hsv_from_color() {
    rgb_to_hsv(cur_color(), &g_hue, &g_sat, &g_val);
    build_sv_dib();
    update_color_box_texts();
}

// --- RGB / HEX 数値入力ボックス (子 EDIT コントロール) ---

static WNDPROC g_edit_prev_proc = nullptr;

static void apply_color_boxes(); // 前方宣言

// キーボード操作 (Esc / E / P / B ポーリング) を受け付けてよい状態か。
// 前景が自ウィンドウ、またはカーソルが自ウィンドウ上にある場合のみ許可する。
// GetAsyncKeyState は OS 全体の物理キー状態のため何もゲートしないと
// alt-tab 先 (ブラウザ等) での Esc 押下がペンモード破棄に直結する
//。ホストはペンモード中無効化されているため
// 自ウィンドウ前景への復帰は ensure_pen_foreground が担う。
static bool pen_input_context() {
    if (!frame_window || !IsWindow(frame_window)) return false;
    if (GetForegroundWindow() == frame_window) return true;
    POINT cpt;
    if (!GetCursorPos(&cpt) || !ScreenToClient(frame_window, &cpt)) return false;
    return cpt.x >= 0 && cpt.y >= 0 && cpt.x < client_w && cpt.y < client_h;
}

// Esc 専用のより厳しいゲート。前景が自ウィンドウのときのみ許可する。
// カーソルゲートでは保護できないケースがある: Alt+Tab はキーボード操作の
// ためカーソルが自ウィンドウ上に残ったままブラウザへ切り替わり、そこでの
// Esc 押下 (タブ閉じ等) がペンモード破棄に直結した (同種不具合の再発)。
static bool pen_esc_context() {
    return frame_window && IsWindow(frame_window) &&
           GetForegroundWindow() == frame_window;
}

// ブラウザ等へ奪った前景を自ウィンドウへ戻す。WS_EX_NOACTIVATE のため
// クリックしても自動活性化せず、ホストはペンモード中無効化されているので
// 再活性化の入口が他にない (指摘: RGB/HEX への入力がブラウザに吸われる)。
// 自プロセスが直近の入力イベントを受け取った直後なら SetForegroundWindow は許可される。
static void ensure_pen_foreground() {
    if (in_modal_dialog || !frame_window || !IsWindow(frame_window)) return;
    if (GetForegroundWindow() == frame_window) return;
    SetForegroundWindow(frame_window);
}

static LRESULT CALLBACK color_edit_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_KEYDOWN && w == VK_RETURN) {
        apply_color_boxes();
        if (frame_window) SetFocus(frame_window);
        return 0;
    }
    if (msg == WM_LBUTTONDOWN) {
        ensure_pen_foreground();
        // WS_EX_NOACTIVATE 環境では EDIT が自動でフォーカスを得られないため明示的に設定
        SetFocus(h);
    }
    return CallWindowProcW(g_edit_prev_proc, h, msg, w, l);
}

static void subclass_color_edit(HWND h) {
    if (!g_edit_prev_proc) {
        g_edit_prev_proc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(color_edit_proc)));
    } else {
        SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(color_edit_proc));
    }
}

static void create_color_edits(HWND hwnd) {
    for (int i = 0; i < 3; ++i) {
        // 青系枠は親 (draw_left_panel の draw_accent_frame) が描画するため EDIT は枠なし
        g_rgb_edits[i] = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_LEFT | ES_NUMBER,
            0, 0, 40, 18, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_RGB0 + i)),
            module_instance, nullptr);
        SendMessageW(g_rgb_edits[i], WM_SETFONT,
                     reinterpret_cast<WPARAM>(g_font_small), TRUE);
        subclass_color_edit(g_rgb_edits[i]);
    }
    g_hex_edit = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_LEFT,
        0, 0, 60, 18, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_HEX)),
        module_instance, nullptr);
    SendMessageW(g_hex_edit, WM_SETFONT,
                 reinterpret_cast<WPARAM>(g_font_small), TRUE);
    subclass_color_edit(g_hex_edit);
}

static BOOL read_box_int(HWND h, int lo, int hi, int* out) {
    wchar_t buf[16] = {};
    GetWindowTextW(h, buf, 16);
    if (!buf[0]) return FALSE;
    int v = _wtoi(buf);
    *out = clamp_int(v, lo, hi);
    return TRUE;
}

static void update_color_box_texts() {
    if (!frame_window || !g_rgb_edits[0] || !g_hex_edit) return;
    COLORREF c = cur_color();
    SetDlgItemInt(frame_window, IDC_EDIT_RGB0,     GetRValue(c), FALSE);
    SetDlgItemInt(frame_window, IDC_EDIT_RGB0 + 1, GetGValue(c), FALSE);
    SetDlgItemInt(frame_window, IDC_EDIT_RGB0 + 2, GetBValue(c), FALSE);
    wchar_t hx[16];
    swprintf_s(hx, L"#%02x%02x%02x",
               static_cast<unsigned>(GetRValue(c)),
               static_cast<unsigned>(GetGValue(c)),
               static_cast<unsigned>(GetBValue(c)));
    SetWindowTextW(g_hex_edit, hx);
}

// 入力値を現在色へ適用 (HEX 入力があれば優先、無ければ R/G/B)
static void apply_color_boxes() {
    wchar_t hx[16] = {};
    GetWindowTextW(g_hex_edit, hx, 16);
    const wchar_t* p = hx;
    while (*p == L'#' || *p == L' ') ++p;
    unsigned hexv = 0;
    int digits = 0;
    for (const wchar_t* q = p; *q; ++q) {
        if (!iswxdigit(*q)) break;
        hexv = hexv * 16 + (*q <= L'9' ? *q - L'0' : (towlower(*q) - L'a' + 10));
        ++digits;
    }
    bool hex_ok = (digits == 6);

    int r = 0, g = 0, b = 0;
    bool rgb_ok = read_box_int(g_rgb_edits[0], 0, 255, &r) &&
                  read_box_int(g_rgb_edits[1], 0, 255, &g) &&
                  read_box_int(g_rgb_edits[2], 0, 255, &b);

    COLORREF nc;
    bool changed = false;
    // 最後に編集した入力列を優先 (両方有効な場合は競合を避ける)
    if (g_last_color_source == 1 && hex_ok) {
        nc = RGB((hexv >> 16) & 0xFF, (hexv >> 8) & 0xFF, hexv & 0xFF);
        changed = true;
    } else if (g_last_color_source != 1 && rgb_ok) {
        nc = RGB(r, g, b);
        changed = true;
    } else if (hex_ok) {
        nc = RGB((hexv >> 16) & 0xFF, (hexv >> 8) & 0xFF, hexv & 0xFF);
        changed = true;
    } else if (rgb_ok) {
        nc = RGB(r, g, b);
        changed = true;
    }
    if (changed && nc != cur_color()) {
        g_layer_styles[g_cur_layer].col = nc;
        sync_hsv_from_color();
        update_color_box_texts();
    }
    redraw_frame();
}

static void draw_left_panel(HDC dc) {
    RECT pr = { 0, 0, left_panel_px(), std::max(0, client_h - BAR_BOTTOM_H) };
    fill_rect_dc(dc, pr, COL_BG_RAIL);

    const wchar_t* names[TOOL_BTN_NUM] = { L"ペン", L"消しゴム", L"元に戻す", L"やり直し" };
    for (int i = 0; i < TOOL_BTN_NUM; ++i) {
        bool enabled = true;
        // 描画中も通常表示を維持する (無効化スタイルへの切替が「押下残留」に
        // 見えるための対処)。クリック自体は描画中ハンドラ側で拒否される。
        if (i == static_cast<int>(ToolKind::Undo))
            enabled = !undo_stack.empty();
        if (i == static_cast<int>(ToolKind::Redo))
            enabled = !redo_stack.empty();
        // アクティブツールのボタンを押下表示 (P3 で消しゴムが有効化)
        bool pressed =
            (i == static_cast<int>(ToolKind::Pen) && g_active_tool == ToolKind::Pen) ||
            (i == static_cast<int>(ToolKind::Eraser) && g_active_tool == ToolKind::Eraser);
        bool hovered = g_hover.zone == HitZone::ToolBtn && g_hover.index == i && enabled;
        draw_button_face(dc, g_tool_rects[i], names[i], enabled, hovered, pressed, false);
    }

    // スライダー行描画 (レイヤー行と消しゴムサイズ行で共有)。
    // 内訳: [ラベル|−|トラック|+|値]
    auto draw_slider_row = [&](MiniSlider& s, bool layer_label) {
        RECT lr = { s.rect.left + 4, s.rect.top, s.minus_btn.left - 2, s.rect.bottom };
        wchar_t lbl[64];
        if (layer_label)
            swprintf_s(lbl, L"%s(L%d)", s.label, g_cur_layer);
        else
            wcscpy_s(lbl, s.label);
        draw_text_dc(dc, lr, lbl, COL_TEXT_DIM, g_font_small,
                     DT_LEFT | DT_END_ELLIPSIS);
        // −1 / +1 ボタン
        auto draw_step_btn = [&](const RECT& rc, const wchar_t* t, bool hov) {
            fill_rect_dc(dc, rc, hov ? COL_BTN_HOVER : COL_BTN_NORMAL);
            HPEN pen = CreatePen(PS_SOLID, 1, COL_SEL_ACCENT);
            HGDIOBJ op = SelectObject(dc, pen);
            HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(pen); // GDI リーク防止
            draw_text_dc(dc, rc, t, COL_TEXT_MAIN, g_font_small, DT_CENTER);
        };
        draw_step_btn(s.minus_btn, L"\x2212", s.hover_minus); // − (U+2212)
        draw_step_btn(s.plus_btn, L"+", s.hover_plus);
        // トラック
        RECT tr = s.track;
        tr.top = (s.track.top + s.track.bottom) / 2 - 3;
        tr.bottom = tr.top + 6;
        fill_rect_dc(dc, tr, COL_BTN_NORMAL);
        // つまみ
        double u = slider_value_to_u(s, *s.value);
        int tx = tr.left + static_cast<int>(
            std::lround((tr.right - tr.left - 8) * u));
        RECT thumb = { tx, s.track.top - 2, tx + 8, s.track.bottom + 2 };
        fill_rect_dc(dc, thumb, (s.drag || s.hover) ? COL_TEXT_MAIN : COL_SEL_ACCENT);
        // 値
        RECT vr = { s.plus_btn.right + 2, s.rect.top, s.rect.right, s.rect.bottom };
        wchar_t vt[32];
        swprintf_s(vt, L"%.0f", *s.value);
        draw_text_dc(dc, vr, vt, COL_TEXT_MAIN, g_font_small, DT_RIGHT);
    };
    for (int i = 0; i < SLIDER_NUM; ++i)
        draw_slider_row(g_sliders[i], true);
    draw_slider_row(g_eraser_slider, false);

    // カラーホイール (hue リング + SV 四角 + マーカー)
    draw_color_wheel(dc);

    // RGB 数値行ラベル + 青系枠
    const wchar_t* rgb_labels[3] = { L"R", L"G", L"B" };
    for (int r = 0; r < 3; ++r) {
        RECT rl = { g_rgb_box_rects[r].left - dpi_s(14), g_rgb_box_rects[r].top,
                    g_rgb_box_rects[r].left - dpi_s(2), g_rgb_box_rects[r].bottom };
        draw_text_dc(dc, rl, rgb_labels[r], COL_TEXT_DIM, g_font_small, DT_LEFT);
        draw_accent_frame(dc, g_rgb_box_rects[r]);
    }
    draw_accent_frame(dc, g_hex_box_rect);
    // 区切り線 (色設定 | プリセット)
    if (g_sep3_line.bottom > g_sep3_line.top)
        fill_rect_dc(dc, g_sep3_line, COL_BTN_NORMAL);

    // 設定適用ボタン列 + プリセットグリッド (設計確定)
    draw_button_face(dc, g_apply_all_rect, L"全レイヤーに適用", true,
                     g_hover.zone == HitZone::ApplyAllBtn, false, false);
    wchar_t pnum[8];
    for (int i = 0; i < 10; ++i) {
        const RECT& rc = g_preset_rects[i];
        bool filled = g_presets[i + 1].valid;
        bool hov = g_hover.zone == HitZone::PresetBtn && g_hover.index == i;
        // 保存済み = ペンモード選択中ボタンと同じ配色 (押下状態スタイル)
        COLORREF bg = filled ? (hov ? RGB(30, 70, 130) : RGB(18, 52, 100))
                             : (hov ? COL_BTN_HOVER : COL_BTN_NORMAL);
        COLORREF fg = filled ? COL_TEXT_MAIN : COL_TEXT_DIM;
        fill_rect_dc(dc, rc, bg);
        HPEN pen = CreatePen(PS_SOLID, filled ? 2 : 1,
                             filled ? RGB(70, 150, 235) : COL_SEL_ACCENT);
        HGDIOBJ op = SelectObject(dc, pen);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(pen);
        swprintf_s(pnum, L"%02d", i + 1);
        draw_text_dc(dc, rc, pnum, fg, g_font_small, DT_CENTER);
    }
    RECT pg = { g_apply_all_rect.left, g_preset_rects[9].bottom + dpi_s(2),
                g_apply_all_rect.right, g_preset_rects[9].bottom + dpi_s(14) };
    draw_text_dc(dc, pg, L"プリセット クリック:適用 / 右クリック:保存",
                 COL_TEXT_DIM, g_font_small, DT_CENTER);
    // 「確認しない」チェックボックス
    {
        int side = g_preset_confirm_rect.bottom - g_preset_confirm_rect.top;
        RECT sq = { g_preset_confirm_rect.left, g_preset_confirm_rect.top,
                    g_preset_confirm_rect.left + side, g_preset_confirm_rect.bottom };
        fill_rect_dc(dc, sq, RGB(20, 20, 24));
        // 枠とチェック線は青系 (ダークテーマ統一)
        HPEN pen = CreatePen(PS_SOLID, 1,
                             g_hover.zone == HitZone::PresetConfirmChk ? RGB(90, 165, 245)
                                                                       : COL_SEL_ACCENT);
        HGDIOBJ op = SelectObject(dc, pen);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, sq.left, sq.top, sq.right, sq.bottom);
        if (g_preset_confirm_disable) {
            MoveToEx(dc, sq.left + 2, sq.top + 2, nullptr);
            LineTo(dc, sq.right - 2, sq.bottom - 2);
            MoveToEx(dc, sq.right - 2, sq.top + 2, nullptr);
            LineTo(dc, sq.left + 2, sq.bottom - 2);
        }
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(pen);
        RECT lbl = { sq.right + dpi_s(4), g_preset_confirm_rect.top,
                     g_preset_confirm_rect.right + dpi_s(40), g_preset_confirm_rect.bottom };
        draw_text_dc(dc, lbl, L"確認しない", COL_TEXT_DIM, g_font_small, DT_LEFT | DT_VCENTER);
    }

    // セパレータ (ドラッグ可能な境界。ホバー/ドラッグ中は強調)
    bool h1 = (g_hover.zone == HitZone::SepH1) || g_drag_sep == 0;
    bool h2 = (g_hover.zone == HitZone::SepH2) || g_drag_sep == 1;
    fill_rect_dc(dc, { dpi_s(4), g_sep1_px - 2, left_panel_px() - dpi_s(4), g_sep1_px + 2 },
                 h1 ? COL_SEL_ACCENT : COL_BTN_NORMAL);
    fill_rect_dc(dc, { dpi_s(4), g_sep2_px - 2, left_panel_px() - dpi_s(4), g_sep2_px + 2 },
                 h2 ? COL_SEL_ACCENT : COL_BTN_NORMAL);
}

//-----------------------------------------------------------------------------
// レイヤーリスト補助 (P2)
//-----------------------------------------------------------------------------

// 指定レイヤーにストロークが存在するか (空レイヤーは「存在しないもの」扱い)
static bool layer_has_strokes(int L) {
    for (const auto& st : strokes)
        if (!st.empty() && st[0].layer == L) return true;
    return false;
}

// 統合先: 選択中レイヤーの直下 (番号大側) の最初の非空レイヤー。無ければ -1
static int next_nonempty_layer(int L) {
    for (int j = L + 1; j <= PEN_LAYER_MAX; ++j)
        if (layer_has_strokes(j)) return j;
    return -1;
}

// サムネイル描画: 常にシーン全体を表示する (設計確定)。
// 線の外接矩形にフィットさせないため、端まで描かなくても全体像と
// 線の位置関係が分かる。空レイヤーではカメラ枠のみ表示される。
static void draw_layer_thumb(HDC dc, const RECT& dst, int layer) {
    // 外周 = カメラ外の余白
    fill_rect_dc(dc, dst, RGB(16, 16, 20));
    int scene_w = frame_w >= 1 ? frame_w : 1920;
    int scene_h = frame_h >= 1 ? frame_h : 1080;
    double tw = static_cast<double>(dst.right - dst.left);
    double th = static_cast<double>(dst.bottom - dst.top);
    double sc = std::min((tw - 8.0) / scene_w, (th - 8.0) / scene_h);
    if (sc <= 0.0001) return;
    double vw = scene_w * sc, vh = scene_h * sc;
    double ox = dst.left + (tw - vw) * 0.5;
    double oy = dst.top + (th - vh) * 0.5;
    RECT view = { static_cast<int>(std::lround(ox)), static_cast<int>(std::lround(oy)),
                  static_cast<int>(std::lround(ox + vw)), static_cast<int>(std::lround(oy + vh)) };
    // シーン枠 (カメラ範囲)
    fill_rect_dc(dc, view, COL_THUMB_EMPTY);
    {
        HPEN pen = CreatePen(PS_SOLID, 1, COL_BTN_HOVER);
        HGDIOBJ op = SelectObject(dc, pen);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, view.left, view.top, view.right, view.bottom);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(pen);
    }

    SaveDC(dc);
    IntersectClipRect(dc, view.left, view.top, view.right, view.bottom);
    COLORREF col = g_layer_styles[layer].col;
    if (!g_layer_visible[layer]) col = dim_color(col); // 非表示は薄暗く
    int pen_w = clamp_int(static_cast<int>(std::lround(g_layer_styles[layer].w * sc)), 1, 3);
    HPEN pen = CreatePen(PS_GEOMETRIC | PS_ENDCAP_ROUND | PS_JOIN_ROUND, pen_w, col);
    HGDIOBJ old = SelectObject(dc, pen);
    for (const auto& st : strokes) {
        if (st.empty() || st[0].layer != layer) continue;
        if (st[0].vanish > 0) continue; // ゴースト断片は最終静止画に出ない
        MoveToEx(dc, static_cast<int>(std::lround(st[0].sx * sc + ox)),
                      static_cast<int>(std::lround(st[0].sy * sc + oy)), nullptr);
        for (size_t i = 1; i < st.size(); ++i)
            LineTo(dc, static_cast<int>(std::lround(st[i].sx * sc + ox)),
                        static_cast<int>(std::lround(st[i].sy * sc + oy)));
    }
    SelectObject(dc, old);
    DeleteObject(pen);
    RestoreDC(dc, -1);
}

static void draw_right_panel(HDC dc) {
    int rx = client_w - right_panel_px();
    RECT pr = { rx, 0, client_w, std::max(0, client_h - BAR_BOTTOM_H) };
    fill_rect_dc(dc, pr, COL_BG_RAIL);

    // レイヤー見出し (▶ ボタンの意味を添える)
    draw_text_dc(dc, g_layer_header_rect, L"レイヤー  ▶ = 動画として出力",
                 COL_TEXT_DIM, g_font_small, DT_LEFT);

    for (int i = 0; i < PEN_LAYER_MAX; ++i) {
        const int L = i + 1;
        const RECT& row = g_layer_rows[i];
        bool selected = (L == g_cur_layer);
        bool hovered = g_hover.zone == HitZone::LayerRow && g_hover.index == i;
        fill_rect_dc(dc, row,
                     selected ? COL_SEL_ACCENT :
                     hovered ? COL_BTN_HOVER : COL_BTN_NORMAL);

        // 表示チェック : OFF でもデータは保持され確定時に書き出される
        RECT ck = g_layer_check_rects[i];
        int cw = ck.right - ck.left, chh = ck.bottom - ck.top;
        HPEN pen = CreatePen(PS_SOLID, 1, selected ? RGB(140, 190, 255) : COL_TEXT_DIM);
        HGDIOBJ op = SelectObject(dc, pen);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, ck.left, ck.top, ck.right, ck.bottom);
        if (g_layer_visible[L]) {
            MoveToEx(dc, ck.left + cw * 18 / 100, ck.top + chh * 55 / 100, nullptr);
            LineTo(dc, ck.left + cw * 40 / 100, ck.top + chh * 80 / 100);
            LineTo(dc, ck.left + cw * 82 / 100, ck.top + chh * 20 / 100);
        }
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(pen);

        // サムネイル (シーン全体表示。右列を除いた領域全体)
        RECT thumb = { ck.right + dpi_s(6), row.top + dpi_s(6),
                       g_layer_label_rects[i].left - dpi_s(6), row.bottom - dpi_s(6) };
        draw_layer_thumb(dc, thumb, L);

        // 右列: レイヤー名 + 再生トグル ▶ (設計確定)
        wchar_t lbl[16];
        swprintf_s(lbl, L"L%d", L);
        draw_text_dc(dc, g_layer_label_rects[i], lbl,
                     selected ? COL_TEXT_MAIN : COL_TEXT_DIM, g_font_text, DT_CENTER);
        {
            // ▶ アイコンボタン: ON (再生に含める) = 青系 / OFF (除外 = 下書き) = 灰色
            const RECT& pr = g_layer_play_rects[i];
            bool off = g_layer_playback_hidden[L];
            bool phov = g_hover.zone == HitZone::LayerPlay && g_hover.index == i;
            COLORREF pbg = !off ? (phov ? RGB(30, 70, 130) : RGB(18, 52, 100))
                                : (phov ? COL_BTN_HOVER : COL_BTN_NORMAL);
            COLORREF pbc = !off ? RGB(70, 150, 235)
                                : (phov ? COL_TEXT_DIM : COL_BTN_HOVER);
            COLORREF pfg = !off ? RGB(160, 200, 250) : COL_TEXT_DIM;
            fill_rect_dc(dc, pr, pbg);
            HPEN pen = CreatePen(PS_SOLID, 1, pbc);
            HGDIOBJ opn = SelectObject(dc, pen);
            HGDIOBJ obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, pr.left, pr.top, pr.right, pr.bottom);
            SelectObject(dc, obr);
            SelectObject(dc, opn);
            DeleteObject(pen);
            // ▶ 塗り三角
            int pw2 = pr.right - pr.left, ph2 = pr.bottom - pr.top;
            POINT tri[3] = {
                { pr.left + pw2 * 36 / 100, pr.top + ph2 * 26 / 100 },
                { pr.left + pw2 * 36 / 100, pr.top + ph2 * 74 / 100 },
                { pr.left + pw2 * 74 / 100, pr.top + ph2 * 50 / 100 },
            };
            HBRUSH hb = get_cached_brush(pfg);
            HGDIOBJ ob2 = SelectObject(dc, hb);
            HGDIOBJ op2 = SelectObject(dc, GetStockObject(NULL_PEN));
            Polygon(dc, tri, 3);
            SelectObject(dc, ob2);
            SelectObject(dc, op2);
        }
    }

    // 統合 / クリア。見た目は描画中も通常のまま (クリックは拒否される)。
    bool merge_ok = next_nonempty_layer(g_cur_layer) > 0;
    bool clear_ok = layer_has_strokes(g_cur_layer);
    draw_button_face(dc, g_merge_rect, L"下のレイヤーと統合", merge_ok,
                     g_hover.zone == HitZone::MergeBtn, false, false);
    draw_button_face(dc, g_clear_rect, L"レイヤーをクリア", clear_ok,
                     g_hover.zone == HitZone::ClearBtn, false, false);
}

// カスタムタイトルバー描画 (設計確定)。
// ダーク背景 + 「ペンモード」 + 右端に最大化/閉じるのみ。アイコン・最小化なし。
static void draw_title_bar(HDC dc) {
    RECT tr = { 0, 0, client_w, title_px() };
    fill_rect_dc(dc, tr, RGB(38, 38, 46));

    RECT tx = { dpi_s(12), 0, dpi_s(400), title_px() };
    draw_text_dc(dc, tx, L"ペンモード", COL_TEXT_MAIN, g_font_small,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    for (int i = 0; i < 2; ++i) {
        const RECT& rc = g_title_btn_rects[i];
        HitZone tz = (i == 0) ? HitZone::TitleMax : HitZone::TitleClose;
        bool hov = g_hover.zone == tz;
        // 閉じるは Windows 準拠の赤ホバー
        COLORREF bg;
        if (i == 1 && hov)      bg = RGB(210, 50, 40);
        else if (hov)           bg = COL_BTN_HOVER;
        else                    bg = RGB(38, 38, 46);
        fill_rect_dc(dc, rc, bg);
        COLORREF fg = (i == 1 && hov) ? RGB(255, 255, 255) : COL_TEXT_MAIN;

        HPEN pen = CreatePen(PS_SOLID, 1, fg);
        HGDIOBJ op = SelectObject(dc, pen);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        int cxm = (rc.left + rc.right) / 2;
        int cym = (rc.top + rc.bottom) / 2;
        if (i == 0) { // 最大化 / 元に戻す
            if (!IsZoomed(frame_window)) {
                Rectangle(dc, cxm - dpi_s(5), cym - dpi_s(4),
                          cxm + dpi_s(5), cym + dpi_s(6));
            } else {
                Rectangle(dc, cxm - dpi_s(5), cym - dpi_s(2),
                          cxm + dpi_s(4), cym + dpi_s(6));
                MoveToEx(dc, cxm - dpi_s(3), cym - dpi_s(4), nullptr);
                LineTo(dc, cxm + dpi_s(5), cym - dpi_s(4));
                LineTo(dc, cxm + dpi_s(5), cym + dpi_s(3));
            }
        } else { // 閉じる: ×
            MoveToEx(dc, cxm - dpi_s(4), cym - dpi_s(4), nullptr);
            LineTo(dc, cxm + dpi_s(4), cym + dpi_s(4));
            MoveToEx(dc, cxm + dpi_s(4), cym - dpi_s(4), nullptr);
            LineTo(dc, cxm - dpi_s(4), cym + dpi_s(4));
        }
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(pen);
    }
}

static void draw_bottom_bar(HDC dc) {
    RECT bar = { 0, client_h - BAR_BOTTOM_H, client_w, client_h };
    fill_rect_dc(dc, bar, COL_BG_RAIL);
    draw_button_face(dc, g_confirm_rect, L"確定", true,
                     g_hover.zone == HitZone::Confirm,
                     g_hover.zone == HitZone::Confirm, true);
    draw_button_face(dc, g_cancel_rect, L"キャンセル", true,
                     g_hover.zone == HitZone::Cancel, false, false);
    draw_text_dc(dc, g_bottom_guide_rect,
                 L"P・B:ペン / E:消しゴム / Ctrl+Z:元に戻す / Ctrl+Y:やり直し / Esc:キャンセル / スライダーでShift+クリックorホイール:±5",
                 COL_TEXT_DIM, g_font_small, DT_LEFT | DT_END_ELLIPSIS);
}

//-----------------------------------------------------------------------------
// オーバーレイ描画 (クライアント全体を 1 枚の DIB に合成)
//-----------------------------------------------------------------------------

static bool ensure_dib(int w, int h) {
    if (g_dib_dc && g_dib && g_dib_w == w && g_dib_h == h) return true;
    if (g_dib_dc) { DeleteDC(g_dib_dc); g_dib_dc = nullptr; }
    if (g_dib) { DeleteObject(g_dib); g_dib = nullptr; }
    g_dib_bits = nullptr;
    g_dib_w = w;
    g_dib_h = h;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    g_dib_dc = CreateCompatibleDC(nullptr);
    if (!g_dib_dc) return false;
    g_dib = CreateDIBSection(g_dib_dc, &bmi, DIB_RGB_COLORS, &g_dib_bits, nullptr, 0);
    return g_dib && g_dib_bits;
}

// 記録済みストローク 1 本を描く（プレビュー）。dc の原点は canvas 左上に設定済み。
// 入り抜き（taper）を区間分割の擬似可変幅で表現する (現行ロジック踏襲)。
// スタイルはストロークの属するレイヤーのものを使う (レイヤー別スタイル)。
// 非選択レイヤーは暗色表示、非表示チェック中は描かない。
static void draw_one_stroke(HDC dc, const std::vector<PenPoint>& st) {
    if (st.empty()) return;
    if (st[0].vanish > 0) return; // ゴースト断片: プレビューは最終状態を表示する
    int L = st[0].layer;
    if (L < 1 || L > PEN_LAYER_MAX) L = 1;
    if (!g_layer_visible[L]) return; // プレビューのみ非表示。データは保持される
    const PenLayerStyle& sty = g_layer_styles[L];
    COLORREF col = (L == g_cur_layer) ? sty.col : dim_color(sty.col);
    float sc = std::max(0.0001f, g_view.scale);
    auto CX = [&](double x) { return static_cast<int>(std::lround(x * sc)); };
    auto CY = [&](double y) { return static_cast<int>(std::lround(y * sc)); };

    int pen_w = std::max(1, static_cast<int>(std::lround(sty.w * sc)));
    HPEN pen = get_cached_pen(PS_GEOMETRIC | PS_ENDCAP_ROUND | PS_JOIN_ROUND, pen_w, col);
    HGDIOBJ old = SelectObject(dc, pen);
    int x0 = CX(st[0].sx);
    int y0 = CY(st[0].sy);
    if (st.size() == 1) {
        MoveToEx(dc, x0, y0, nullptr);
        LineTo(dc, x0, y0);
        SelectObject(dc, old);
        return;
    }

    const double total = st.back().acc;
    // 入り/抜きはストローク属性 (xi/xo) で個別に無効化できる。
    // 消しゴム断片は元ストローク始端/終端を含む断片のみ有効 → 消去前の
    // 再生表示が元の線と同一になり、消去後は切断端がフラットに残る。
    const bool has_in = st[0].taper_in && sty.ti > 0 && sty.twi < 100;
    const bool has_out = st[0].taper_out && sty.to > 0 && sty.two_ < 100;
    const bool taper_on = total > 0 && (has_in || has_out);

    auto polyline_all = [&]() {
        MoveToEx(dc, x0, y0, nullptr);
        for (size_t i = 1; i < st.size(); ++i) LineTo(dc, CX(st[i].sx), CY(st[i].sy));
    };

    if (!taper_on) { polyline_all(); SelectObject(dc, old); return; }

    const double seg_in = has_in ? std::min(sty.ti * sty.w, total * 0.5) : 0.0;
    const double seg_out = has_out ? std::min(sty.to * sty.w, total * 0.5) : 0.0;
    if (seg_in <= 0 && seg_out <= 0) {
        polyline_all();
        SelectObject(dc, old);
        return;
    }

    auto point_at = [&](double d, double* px, double* py) {
        if (d <= 0) { *px = st[0].sx; *py = st[0].sy; return; }
        if (d >= total) { *px = st.back().sx; *py = st.back().sy; return; }
        size_t lo = 0, hi = st.size() - 1;
        while (lo + 1 < hi) {
            size_t mid = (lo + hi) / 2;
            if (st[mid].acc <= d) lo = mid; else hi = mid;
        }
        double seglen = st[lo + 1].acc - st[lo].acc;
        double u = seglen > 0 ? (d - st[lo].acc) / seglen : 0;
        *px = st[lo].sx + (st[lo + 1].sx - st[lo].sx) * u;
        *py = st[lo].sy + (st[lo + 1].sy - st[lo].sy) * u;
    };
    auto ratio_at = [&](double d) -> double {
        if (seg_in > 0 && d <= seg_in)
            return sty.twi / 100.0 + (1.0 - sty.twi / 100.0) * (d / seg_in);
        if (seg_out > 0 && d >= total - seg_out)
            return sty.two_ / 100.0 + (1.0 - sty.two_ / 100.0) * ((total - d) / seg_out);
        return 1.0;
    };

    double win_per_scene = static_cast<double>(sc);

    // テーパーゾーンの描画: 2px 刻みで幅をサンプルし、**整数幅が同じ連続区間を
    // 1 本のポリラインにまとめる**。従来は各区間ごとにペン選択↔復帰を行っており、
    // ストローク数に比例して GDI 呼び出しが増えフレーム予算を超過していた
    // (P4 チェックリスト 計測: 115 ストロークで strokes 成分 35ms)。
    // 出力ジオメトリ (サンプル点列と各区間の幅) は従来と同一。
    auto draw_sampled = [&](double d_from, double d_to) {
        if (d_to - d_from < 1e-9) return;
        double x, y;
        point_at(d_from, &x, &y);
        MoveToEx(dc, CX(x), CY(y), nullptr);
        const double step = std::max(1e-9, 2.0 / std::max(win_per_scene, 0.0001));
        for (double d = d_from + step; d < d_to; d += step) {
            point_at(d, &x, &y);
            LineTo(dc, CX(x), CY(y));
        }
        point_at(d_to, &x, &y);
        LineTo(dc, CX(x), CY(y));
    };
    auto draw_zone_runs = [&](double a, double b) {
        if (b - a < 1e-9) return;
        const double step = std::max(1e-9, 2.0 / std::max(win_per_scene, 0.0001));
        // 幅を量子化してラン数を削減する。近傍丸めでテーパーの階段差を最小化
        // (段階幅 qw = pen_w/12、最小幅 1 は維持)
        const int qw = std::max(1, static_cast<int>(std::lround(pen_w / 12.0)));
        auto snap = [&](int w) {
            const int q = ((w + qw / 2) / qw) * qw;
            return std::max(1, q);
        };
        double run_d0 = a;
        int run_w = snap(std::max(1, static_cast<int>(
            std::lround(pen_w * ratio_at(std::min(b, a + step))))));
        for (double d = a + step; d < b;) {
            const double dn = std::min(b, d + step);
            const int w = snap(std::max(1, static_cast<int>(
                std::lround(pen_w * ratio_at((d + dn) * 0.5)))));
            if (w != run_w) {
                SelectObject(dc, get_cached_pen(
                    PS_GEOMETRIC | PS_ENDCAP_ROUND | PS_JOIN_ROUND, run_w, col));
                draw_sampled(run_d0, d);
                run_d0 = d;
                run_w = w;
            }
            d = dn;
        }
        SelectObject(dc, get_cached_pen(
            PS_GEOMETRIC | PS_ENDCAP_ROUND | PS_JOIN_ROUND, run_w, col));
        draw_sampled(run_d0, b);
        SelectObject(dc, pen);
    };

    if (seg_in > 0) draw_zone_runs(0.0, seg_in);
    if (seg_in < total - seg_out) {
        double cx0, cy0, cx1, cy1;
        point_at(seg_in, &cx0, &cy0);
        point_at(total - seg_out, &cx1, &cy1);
        SelectObject(dc, pen);
        MoveToEx(dc, CX(cx0), CY(cy0), nullptr);
        for (size_t i = 1; i < st.size(); ++i) {
            if (st[i].acc <= seg_in) continue;       // 入り区間内の頂点はスキップ
            if (st[i].acc >= total - seg_out) break; // 抜き区間に入ったら終了
            LineTo(dc, CX(st[i].sx), CY(st[i].sy));
        }
        LineTo(dc, CX(cx1), CY(cy1));
    }
    if (seg_out > 0) draw_zone_runs(total - seg_out, total);
    SelectObject(dc, old);
}

// 診断カウンタ (perf_frame 用)




static void draw_all_strokes(HDC dc, size_t stroke_begin, size_t stroke_end) {
    // 再生 (obj2) と同じ z 規約で描く: レイヤー 5→1 (L1 が最上)、
    // 同一レイヤー内は書いた順。stroke_begin/end はベクタ添字の半開区間
    // [begin, end)。レイヤー順走査でも添字比較により範囲が正しく効く。
    for (int L = PEN_LAYER_MAX; L >= 1; --L) {
        for (size_t i = 0; i < strokes.size(); ++i) {
            const auto& st = strokes[i];
            if (i < stroke_begin || i >= stroke_end || st.empty() ||
                clamp_int(st.front().layer, 1, PEN_LAYER_MAX) != L)
                continue;
            draw_one_stroke(dc, st);
        }
    }
}

// ---- 確定済みコンテンツキャッシュ (P4 チェックリスト 対策 B) ----
// シーン画像 + strokes[0..live_from) を image_rect 解像度で合成した不透明
// ビットマップ。ジェスチャ中のフレームは「キャッシュ blit + ライブ分の
// 上描き」になり、ストローク数に依存しない描画コストになる。
// 無効化: 構造変更は各所で g_cc_dirty を立てる。スタイル/色/選択/表示の
// 変更は canvas_sig の照合で自動検出する (立て忘れ対策)。
static HDC g_cc_dc = nullptr;
static HBITMAP g_cc_bmp = nullptr;
static void* g_cc_bits = nullptr;
static int g_cc_w = 0, g_cc_h = 0;
static bool g_cc_dirty = true;          // フル再構築が必要
static bool g_cc_part_valid = false;    // 部分更新リージョンが有効
static RECT g_cc_part_rect = {};        // 部分更新の変化領域 (デバイス px)
static size_t g_live_from = 0;      // この添字以降 = 未確定 (ライブ描画対象)
static unsigned long long g_cc_sig = 0;

static void rebuild_erase_preview(); // 前方宣言 (消しゴム断片→strokes 再構築)

static unsigned long long canvas_sig() {
    // スタイル/選択/表示状態の変化を検出する簡易 FNV ハッシュ
    unsigned long long h = 1469598103934665603ull;
    auto mix = [&](unsigned long long v) { h ^= v; h *= 1099511628211ull; };
    mix(static_cast<unsigned>(g_cur_layer));
    for (int L = 1; L <= PEN_LAYER_MAX; ++L) {
        const PenLayerStyle& s = g_layer_styles[L];
        mix(static_cast<unsigned>(s.col));
        unsigned long long wbits = 0;
        wbits |= static_cast<unsigned long long>(std::llround(s.w)) & 0xFFFF;
        wbits |= (static_cast<unsigned long long>(std::llround(s.ti)) & 0xFF) << 16;
        wbits |= (static_cast<unsigned long long>(std::llround(s.twi)) & 0xFF) << 24;
        wbits |= (static_cast<unsigned long long>(std::llround(s.to)) & 0xFF) << 32;
        wbits |= (static_cast<unsigned long long>(std::llround(s.two_)) & 0xFF) << 40;
        mix(wbits);
        mix(g_layer_visible[L] ? 1ull : 0ull);
    }
    return h;
}

static void cleanup_committed_cache() {
    if (g_cc_bmp) DeleteObject(g_cc_bmp);
    if (g_cc_dc) DeleteDC(g_cc_dc);
    g_cc_bmp = nullptr;
    g_cc_dc = nullptr;
    g_cc_bits = nullptr;
    g_cc_w = g_cc_h = 0;
    g_cc_dirty = true;
}

static void mark_canvas_dirty() {
    g_cc_dirty = true;
    g_cc_part_valid = false;
}

// デバイス px (キャッシュローカル座標) の変化領域を登録する。
// フル無効化済みなら何もしない (次の ensure で全体再構築される)
static void mark_canvas_region_device(const RECT& rc) {
    if (g_cc_dirty) return;
    if (!g_cc_part_valid) {
        g_cc_part_rect = rc;
        g_cc_part_valid = true;
        return;
    }
    g_cc_part_rect.left = std::min<long>(g_cc_part_rect.left, rc.left);
    g_cc_part_rect.top = std::min<long>(g_cc_part_rect.top, rc.top);
    g_cc_part_rect.right = std::max<long>(g_cc_part_rect.right, rc.right);
    g_cc_part_rect.bottom = std::max<long>(g_cc_part_rect.bottom, rc.bottom);
}


// キャッシュを必要なら再構築し、利用可能なら true を返す
static bool ensure_committed_cache(const RECT& ir) {
    const int w = ir.right - ir.left;
    const int h = ir.bottom - ir.top;
    if (w < 1 || h < 1) return false;
    if (!g_cc_dc || g_cc_w != w || g_cc_h != h) {
        cleanup_committed_cache();
        g_cc_dc = CreateCompatibleDC(nullptr);
        if (!g_cc_dc) return false;
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        g_cc_bmp = CreateDIBSection(g_cc_dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!g_cc_bmp) { cleanup_committed_cache(); return false; }
        SelectObject(g_cc_dc, g_cc_bmp);
        g_cc_bits = bits;
        g_cc_w = w;
        g_cc_h = h;
        g_cc_dirty = true;
    }
    const unsigned long long sig = canvas_sig();
    if (!g_cc_dirty && sig == g_cc_sig) return true;

    // 再構築: 背景色 + シーン画像 + strokes[0..live_from)
    fill_rect_dc(g_cc_dc, { 0, 0, w, h }, COL_CANVAS_BG);
    if (!canvas_img.empty() && canvas_img_w >= 1 && canvas_img_h >= 1 && g_cc_bits) {
        const int blit_w = std::min(canvas_img_w, w);
        const int blit_h = std::min(canvas_img_h, h);
        for (int y = 0; y < blit_h; ++y) {
            unsigned char* dst = static_cast<unsigned char*>(g_cc_bits) +
                                 (static_cast<size_t>(y) * w) * 4;
            const unsigned char* src =
                canvas_img.data() + static_cast<size_t>(y) * canvas_img_w * 4;
            memcpy(dst, src, static_cast<size_t>(blit_w) * 4);
        }
    }
    SaveDC(g_cc_dc);
    IntersectClipRect(g_cc_dc, 0, 0, w, h);
    draw_all_strokes(g_cc_dc, 0, g_live_from);
    RestoreDC(g_cc_dc, -1);
    g_cc_sig = sig;
    g_cc_dirty = false;
    return true;
}

// 変化のあった領域だけを再描画してキャッシュを部分更新する (消しゴム用)。
// 領域内を背景→シーン→「領域と交差するストローク」の順で描き直す。
// 描画はストローク毎に bbox 交差判定してから行う (幅の半分をマージン)。
static bool update_committed_cache_region(const RECT& ir, RECT rc) {
    if (!g_cc_dc || !g_cc_bits || g_cc_w < 1 || g_cc_h < 1) return false;
    if (rc.left < 0) rc.left = 0;
    if (rc.top < 0) rc.top = 0;
    if (rc.right > g_cc_w) rc.right = g_cc_w;
    if (rc.bottom > g_cc_h) rc.bottom = g_cc_h;
    if (rc.right <= rc.left || rc.bottom <= rc.top) return false;

    fill_rect_dc(g_cc_dc, rc, COL_CANVAS_BG);
    const float sc = std::max(0.0001f, g_view.scale);
    if (!canvas_img.empty() && canvas_img_w >= 1 && canvas_img_h >= 1 && g_cc_bits) {
        const int sx0 = clamp_int(rc.left, 0, canvas_img_w - 1);
        const int sy0 = clamp_int(rc.top, 0, canvas_img_h - 1);
        const int sx1 = clamp_int(rc.right - 1, 0, canvas_img_w - 1);
        const int sy1 = clamp_int(rc.bottom - 1, 0, canvas_img_h - 1);
        for (int y = sy0; y <= sy1; ++y) {
            unsigned char* dst = static_cast<unsigned char*>(g_cc_bits) +
                                 (static_cast<size_t>(y) * g_cc_w + sx0) * 4;
            const unsigned char* src = canvas_img.data() +
                                       (static_cast<size_t>(y) * canvas_img_w + sx0) * 4;
            memcpy(dst, src, static_cast<size_t>(sx1 - sx0 + 1) * 4);
        }
    }

    // 領域と交差するストロークだけ再描画 (シーン座標へ変換し、太さ半分をマージン)
    const double inv_sc = 1.0 / sc;
    const double s_l = rc.left * inv_sc, s_t = rc.top * inv_sc;
    const double s_r = rc.right * inv_sc, s_b = rc.bottom * inv_sc;
    SaveDC(g_cc_dc);
    IntersectClipRect(g_cc_dc, rc.left, rc.top, rc.right, rc.bottom);
    for (int L = PEN_LAYER_MAX; L >= 1; --L) {
        for (size_t i = 0; i < strokes.size(); ++i) {
            const auto& st = strokes[i];
            // 注意: 部分更新はキャッシュ内容 (= 全確定ストローク) の再描画。
            // ライブ除外 (i < g_live_from) を入れると全て skip され
            // 矩形ごと消えたようになる (デバッグで発見)
            if (st.empty()) continue;
            if (clamp_int(st.front().layer, 1, PEN_LAYER_MAX) != L) continue;
            const PenLayerStyle& sty2 = g_layer_styles[L];
            double mnx = 1e18, mny = 1e18, mxx = -1e18, mxy = -1e18;
            for (const auto& p : st) {
                mnx = std::min(mnx, p.sx); mxx = std::max(mxx, p.sx);
                mny = std::min(mny, p.sy); mxy = std::max(mxy, p.sy);
            }
            const double m = sty2.w * 0.5 + 2.0;
            if (mxx < s_l - m || mnx > s_r + m || mxy < s_t - m || mny > s_b + m)
                continue; // 領域外
            draw_one_stroke(g_cc_dc, st);
        }
    }
    RestoreDC(g_cc_dc, -1);
    return true;
}

// フレーム内訳の計測 (P4 チェックリスト)。120 フレームごとに平均をログへ出す。
// strokes=ストローク描画 / scene=シーン画像転送 / rest=パネル等のそれ以外。
// cache=キャッシュ使用率(%) dirty/live は診断用


static void perf_frame(double total_ms, double strokes_ms, double scene_ms,
                       bool used_cache, bool cc_dirty, size_t live_cnt) {
    static double acc_t = 0, acc_s = 0, acc_i = 0;
    static int acc_cnt = 0, acc_used = 0;
    static int max_strokes = 0;
    static size_t last_live = 0;
    acc_t += total_ms; acc_s += strokes_ms; acc_i += scene_ms;
    if (used_cache) ++acc_used;
    int n = 0;
    for (const auto& st : strokes) if (!st.empty()) ++n;
    if (n > max_strokes) max_strokes = n;
    last_live = live_cnt;
    if (++acc_cnt >= 120) {
        if (logger && g_perf_log) {
            wchar_t m[200];
            swprintf_s(m, L"[Perf] frame avg %.2fms (strokes %.2f scene %.2f rest %.2f) strokes=%d cache=%d%% dirty=%d live=%zu",
                       acc_t / acc_cnt, acc_s / acc_cnt, acc_i / acc_cnt,
                       (acc_t - acc_s - acc_i) / acc_cnt, max_strokes,
                       acc_used * 100 / acc_cnt, g_cc_dirty ? 1 : 0, last_live);
            logger->log(logger, m);
        }
        acc_t = acc_s = acc_i = 0; acc_cnt = 0; acc_used = 0; max_strokes = 0;
    }
}

// クライアント全体を DIB へ合成し、再描画を要求する。
static void redraw_frame() {
    if (!frame_window || !IsWindow(frame_window) || !IsWindowVisible(frame_window)) return;
    if (client_w < 1 || client_h < 1) return;
    if (!ensure_dib(client_w, client_h)) return;

    // ジェスチャ中は合成と提示を ~60Hz に間引く (イベント毎の全再合成が
    // フレーム予算を圧迫するため)。間引いた分は無効化領域を維持することで
    // 次回提示時に最新状態へ収束する。非ドラッグ時は即時 (UI 応答性優先)。
    static DWORD s_last_present = 0;
    const DWORD now_tick = GetTickCount();
    const bool dragging = pen_down || erase_down;
    if (dragging && now_tick - s_last_present < 15) {
        InvalidateRect(frame_window, nullptr, FALSE);
        return;
    }

    // 消しゴムの断片再構築が保留されていれば合成前に反映する
    // (サンプル毎の即時再構築をやめ、提示周期に統合して軽量化)
    if (erase_down && g_erase_need_rebuild) {
        g_erase_need_rebuild = false;
        rebuild_erase_preview();
        g_live_from = strokes.size();
    }

    LARGE_INTEGER pf = {}, t0 = {}, t1 = {}, t2 = {}, t3 = {};
    QueryPerformanceFrequency(&pf);
    QueryPerformanceCounter(&t0);

    HDC dc = g_dib_dc;
    HGDIOBJ old_bmp = SelectObject(dc, g_dib);

    // ベース: パネル背景色
    RECT full = { 0, 0, client_w, client_h };
    fill_rect_dc(dc, full, COL_BG_RAIL);

    // パネル類 (背景含む) を先に描く
    draw_left_panel(dc);
    draw_right_panel(dc);
    QueryPerformanceCounter(&t1);

    // canvas 領域: 背景色
    fill_rect_dc(dc, g_view.area, COL_CANVAS_BG);

    // canvas 外付け枠線 (シーン画像の外側、隙間 2px)
    {
        const RECT& ir = g_view.image_rect;
        RECT fr = { ir.left - 3, ir.top - 3, ir.right + 3, ir.bottom + 3 };
        HPEN fpen = CreatePen(PS_SOLID, 1, COL_BTN_HOVER);
        HGDIOBJ opn = SelectObject(dc, fpen);
        HGDIOBJ obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, fr.left, fr.top, fr.right, fr.bottom);
        SelectObject(dc, opn);
        SelectObject(dc, obr);
        DeleteObject(fpen);
    }

    // 確定済みキャッシュ + ライブストローク (P4 対策 B)。
    // ライブ線は常に最上面に上描きする。厳密な z 順ではライブ層より手前の
    // 確定ストロークが存在する場合フル再描画が必要だが、それだと多レイヤー
    // 運用で常時重くなるため、「描画中だけ進行中の線が最前面に見える」
    // ことを許容し、確定時に再構築して正しい重なりへ戻す。
    const RECT& ir = g_view.image_rect;
    const bool have_live = g_live_from < strokes.size();
    const size_t live_count = strokes.size() - g_live_from;

    bool used_cache = false;
    const bool sig_ok = (canvas_sig() == g_cc_sig);
    if (!g_cc_dirty && sig_ok && g_cc_part_valid && g_cc_dc &&
        update_committed_cache_region(ir, g_cc_part_rect)) {
        // 部分更新成功: 変化領域だけ再描画した
        g_cc_part_valid = false;
        BitBlt(dc, ir.left, ir.top, g_cc_w, g_cc_h, g_cc_dc, 0, 0, SRCCOPY);
        used_cache = true;
    } else if (ensure_committed_cache(ir)) {
        BitBlt(dc, ir.left, ir.top, g_cc_w, g_cc_h, g_cc_dc, 0, 0, SRCCOPY);
        used_cache = true;
        g_cc_part_valid = false;
    } else {
        // シーン画像 (行ごとに DIB へ直接転送)
        if (!canvas_img.empty() && canvas_img_w >= 1 && canvas_img_h >= 1) {
            int blit_w = std::min(canvas_img_w, static_cast<int>(ir.right - ir.left));
            int blit_h = std::min(canvas_img_h, static_cast<int>(ir.bottom - ir.top));
            for (int y = 0; y < blit_h; ++y) {
                unsigned char* dst = static_cast<unsigned char*>(g_dib_bits) +
                                     (static_cast<size_t>(ir.top + y) * client_w + ir.left) * 4;
                const unsigned char* src =
                    canvas_img.data() + static_cast<size_t>(y) * canvas_img_w * 4;
                memcpy(dst, src, static_cast<size_t>(blit_w) * 4);
            }
        }
    }
    QueryPerformanceCounter(&t2);

    // ストローク (canvas 原点基準)。画像矩形へクリップする (極端な太さでも
    // シーン外の GUI 領域に線が侵食しないようにする)
    SaveDC(dc);
    IntersectClipRect(dc, g_view.image_rect.left, g_view.image_rect.top,
                      g_view.image_rect.right, g_view.image_rect.bottom);
    SetViewportOrgEx(dc, g_view.image_rect.left, g_view.image_rect.top, nullptr);
    if (used_cache) {
        if (have_live) draw_all_strokes(dc, g_live_from, strokes.size());
    } else {
        draw_all_strokes(dc, 0, strokes.size());
    }

    // 消しゴムのリングカーソル。ツールが消しゴムでカーソルが
    // キャンバス上 (または消去ドラッグ中) のとき、半径を常時提示する。
    if (mode == Mode::PenDraw && g_active_tool == ToolKind::Eraser &&
        g_cursor_valid &&
        (erase_down || point_in_rect(g_cursor_client, g_view.area))) {
        const float sc = std::max(0.0001f, g_view.scale);
        const int rr = std::max(2, static_cast<int>(std::lround(g_eraser_size * 0.5 * sc)));
        const int ccx = g_cursor_client.x - g_view.image_rect.left;
        const int ccy = g_cursor_client.y - g_view.image_rect.top;
        HPEN rpen = CreatePen(PS_DASH, 1, RGB(255, 255, 255));
        HGDIOBJ ropen = SelectObject(dc, rpen);
        HGDIOBJ robr = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Ellipse(dc, ccx - rr, ccy - rr, ccx + rr, ccy + rr);
        SelectObject(dc, robr);
        SelectObject(dc, ropen);
        DeleteObject(rpen);
    }
    SetViewportOrgEx(dc, 0, 0, nullptr);
    RestoreDC(dc, -1);
    QueryPerformanceCounter(&t3);

    if (pf.QuadPart > 0) {
        const double total = static_cast<double>(t3.QuadPart - t0.QuadPart) * 1000.0 / pf.QuadPart;
        const double strokes = static_cast<double>(t3.QuadPart - t2.QuadPart) * 1000.0 / pf.QuadPart;
        const double scene = static_cast<double>(t2.QuadPart - t1.QuadPart) * 1000.0 / pf.QuadPart;
        perf_frame(total, strokes, scene, used_cache, g_cc_dirty, live_count);
    }

    // パネル境界の縦セパレータ (ドラッグハンドル、ホバー/ドラッグ中は青強調)
    // パネル背景描画の後に行うため、ここで描いても隠れない
    bool vl_hot = g_drag_sep == 2 || (g_hover.zone == HitZone::SepVL);
    bool vr_hot = g_drag_sep == 3 || (g_hover.zone == HitZone::SepVR);
    fill_rect_dc(dc, { left_panel_px() - 2, 0, left_panel_px() + 2, client_h - BAR_BOTTOM_H },
                 vl_hot ? COL_SEL_ACCENT : COL_BTN_NORMAL);
    fill_rect_dc(dc, { client_w - right_panel_px() - 2, 0,
                       client_w - right_panel_px() + 2, client_h - BAR_BOTTOM_H },
                 vr_hot ? COL_SEL_ACCENT : COL_BTN_NORMAL);

    // カスタムタイトルバー (パネル描画の上に重ねる)
    draw_title_bar(dc);

    // ドラッグ中オーバーレイ (挿入線・フロートサムネイル) は最上位に描く
    draw_drag_overlay(dc);
    // プリセット確認オーバーレイ (さらに最上位)
    draw_confirm_overlay(dc);

    draw_bottom_bar(dc);

    SelectObject(dc, old_bmp);
    // ジェスチャ中はキャンバス領域だけを無効化する (パネル類は変化しない)
    RECT inv = full;
    if (pen_down || erase_down) inv = g_view.area;
    InvalidateRect(frame_window, &inv, FALSE);
    UpdateWindow(frame_window);
    s_last_present = now_tick;
}

//-----------------------------------------------------------------------------
// ペンツール.obj2 の設定読み取り (プレビュー用)
//-----------------------------------------------------------------------------

// 色項目 "ffffff" / "0xffffff" のどちらでも解釈する
static COLORREF parse_color_item(const char* v, COLORREF fallback) {
    if (!v) return fallback;
    const char* p = v;
    while (*p == ' ' || *p == '\t') ++p;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    unsigned int c = 0;
    int digits = 0;
    while (std::isxdigit(static_cast<unsigned char>(*p)) && digits < 6) {
        int d = 0;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
        else d = *p - 'A' + 10;
        c = c * 16 + d;
        ++p;
        ++digits;
    }
    if (digits == 0) return fallback;
    return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

struct PenStyleCtx {
    OBJECT_HANDLE obj;
    int frame;
    bool modern; // 新構成オブジェクトか (L2 項目の読出成否で判別、)
    PenLayerStyle styles[PEN_LAYER_MAX + 1]; // [1..5]
    int crashed;
};

// Lk の表示名を組む。L1 は従来名 (= 旧オブジェクト互換)、L2 以降は "(Lk)" 付き。
// 表示名は obj2 側の項目定義と完全一致させること (名称指定 API は表示名で探す、T-07)。
static const wchar_t* layer_color_name(int L, wchar_t* buf, size_t cch) {
    if (L <= 1) return L"色";
    swprintf_s(buf, cch, L"色(L%d)", L);
    return buf;
}
static const wchar_t* layer_width_name(int L, wchar_t* buf, size_t cch) {
    if (L <= 1) return L"ペンの太さ(px)";
    swprintf_s(buf, cch, L"ペンの太さ(L%d)(px)", L);
    return buf;
}
static const wchar_t* layer_track_name(int L, const wchar_t* base, wchar_t* buf, size_t cch) {
    if (L <= 1) return base;
    swprintf_s(buf, cch, L"%s(L%d)", base, L);
    return buf;
}

// ペンツール.obj2 の 全レイヤーの 色/太さ/入り抜き を読み取る（プレビュー描画用）。
// 新旧判別 : 新構成のみが持つ「ペンの太さ(L2)(px)」トラックの読出成否を使う。
// fmtver 項目は未確定の新規オブジェクトでも空文字列になるため判別に使えない。
// L2..L5 の値が読めない場合は L1 の値で初期化する (obj2 側フォールバックと同じ挙動)。
static void read_pen_style_edit(void* param, EDIT_SECTION* edit) {
    PenStyleCtx* ctx = static_cast<PenStyleCtx*>(param);
    __try {
        if (!ctx->obj) return;
        if (edit->count_object_effect(ctx->obj, L"ペンツール") <= 0) return;
        const wchar_t* eff = L"ペンツール";

        // L1
        ctx->styles[1].col = parse_color_item(
            edit->get_object_item_value(ctx->obj, eff, L"色"), RGB(0, 0, 0));
        double v = 0.0;
        if (edit->get_object_track_value(ctx->obj, eff, L"ペンの太さ(px)", ctx->frame, &v) && v >= 1.0)
            ctx->styles[1].w = v;
        if (edit->get_object_track_value(ctx->obj, eff, L"入り長さ", ctx->frame, &v) && v >= 0.0)
            ctx->styles[1].ti = v;
        if (edit->get_object_track_value(ctx->obj, eff, L"抜き長さ", ctx->frame, &v) && v >= 0.0)
            ctx->styles[1].to = v;
        if (edit->get_object_track_value(ctx->obj, eff, L"入り太さ", ctx->frame, &v) && v >= 0.0)
            ctx->styles[1].twi = v;
        if (edit->get_object_track_value(ctx->obj, eff, L"抜き太さ", ctx->frame, &v) && v >= 0.0)
            ctx->styles[1].two_ = v;

        // 新旧判別
        double probe = 0.0;
        ctx->modern = edit->get_object_track_value(
            ctx->obj, eff, L"ペンの太さ(L2)(px)", ctx->frame, &probe);

        // L2..L5 (L1 値を基準に、読めた項目だけ上書き)
        for (int L = 2; L <= PEN_LAYER_MAX; ++L) {
            ctx->styles[L] = ctx->styles[1];
            if (!ctx->modern) break;
            wchar_t nb[48];
            const char* cv = edit->get_object_item_value(ctx->obj, eff,
                                                         layer_color_name(L, nb, 48));
            if (cv) ctx->styles[L].col = parse_color_item(cv, ctx->styles[L].col);
            if (edit->get_object_track_value(ctx->obj, eff,
                                             layer_width_name(L, nb, 48), ctx->frame, &v) && v >= 1.0)
                ctx->styles[L].w = v;
            if (edit->get_object_track_value(ctx->obj, eff,
                                             layer_track_name(L, L"入り長さ", nb, 48), ctx->frame, &v) && v >= 0.0)
                ctx->styles[L].ti = v;
            if (edit->get_object_track_value(ctx->obj, eff,
                                             layer_track_name(L, L"抜き長さ", nb, 48), ctx->frame, &v) && v >= 0.0)
                ctx->styles[L].to = v;
            if (edit->get_object_track_value(ctx->obj, eff,
                                             layer_track_name(L, L"入り太さ", nb, 48), ctx->frame, &v) && v >= 0.0)
                ctx->styles[L].twi = v;
            if (edit->get_object_track_value(ctx->obj, eff,
                                             layer_track_name(L, L"抜き太さ", nb, 48), ctx->frame, &v) && v >= 0.0)
                ctx->styles[L].two_ = v;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ctx->crashed = 1;
    }
}

//-----------------------------------------------------------------------------
// モード制御
//-----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// UI 設定の INI 保存/読込 (Plugin\CJF\CJFPreviewPenTool.ini)
// -----------------------------------------------------------------------------

static std::wstring ui_ini_path() {
    std::wstring p;
    if (config_handle && config_handle->app_data_path) {
        p = config_handle->app_data_path;
        if (!p.empty() && p.back() != L'\\') p += L'\\';
        p += L"Plugin\\CJF";
    }
    return p + L"\\CJFPreviewPenTool.ini";
}

static int ini_get_int(const std::wstring& path, const wchar_t* key, int def) {
    return GetPrivateProfileIntW(L"UI", key, def, path.c_str());
}

static void load_ui_settings() {
    std::wstring path = ui_ini_path();
    if (path.empty()) return;
    g_left_panel_w  = clamp_int(ini_get_int(path, L"LeftPanelW", g_left_panel_w), 150, 320);
    g_right_panel_w = clamp_int(ini_get_int(path, L"RightPanelW", g_right_panel_w), 150, 320);
    g_win_w         = clamp_int(ini_get_int(path, L"WinW", g_win_w), 900, 10000);
    g_win_h         = clamp_int(ini_get_int(path, L"WinH", g_win_h), 700, 10000);
    // 下限は旧実装 (タイトル無し絶対座標時代) の名残。実際の有効範囲は
    // do_layout が現在のジオメトリから再クランプするため、ここでは広く受ける。
    // (旧下限 SepWheelY>=280 が復元値を強制的に初期位置へ戻していた)
    g_sep_tool_y    = clamp_int(ini_get_int(path, L"SepToolY", g_sep_tool_y), 80, 100000);
    g_sep_wheel_y   = clamp_int(ini_get_int(path, L"SepWheelY", g_sep_wheel_y), 80, 100000);
    // ストローク間の最大空白。0 で全空白圧縮。上限 1 時間。
    g_gap_cap_ms    = clamp_int(ini_get_int(path, L"GapCapMs", g_gap_cap_ms), 0, 3600000);
    g_perf_log      = ini_get_int(path, L"PerfLog", 0) != 0;
}

static void save_ui_settings(HWND wnd) {
    std::wstring path = ui_ini_path();
    if (path.empty()) return;
    wchar_t buf[32];
    swprintf_s(buf, L"%d", g_left_panel_w);
    WritePrivateProfileStringW(L"UI", L"LeftPanelW", buf, path.c_str());
    swprintf_s(buf, L"%d", g_right_panel_w);
    WritePrivateProfileStringW(L"UI", L"RightPanelW", buf, path.c_str());
    swprintf_s(buf, L"%d", g_sep_tool_y);
    WritePrivateProfileStringW(L"UI", L"SepToolY", buf, path.c_str());
    swprintf_s(buf, L"%d", g_sep_wheel_y);
    WritePrivateProfileStringW(L"UI", L"SepWheelY", buf, path.c_str());
    RECT rcw = {};
    if (wnd && IsWindow(wnd) && !IsIconic(wnd)) {
        // 最小化中は座標が不正になるため保存しない。
        // サイズは DPI 論理値で保存する (物理 px のままだとモニタ間で
        // 復元サイズが変わるため)。
        GetWindowRect(wnd, &rcw);
        UINT dpi = std::max(96u, g_current_dpi);
        swprintf_s(buf, L"%d", MulDiv(static_cast<int>(rcw.right - rcw.left), 96,
                                      static_cast<int>(dpi)));
        WritePrivateProfileStringW(L"UI", L"WinW", buf, path.c_str());
        swprintf_s(buf, L"%d", MulDiv(static_cast<int>(rcw.bottom - rcw.top), 96,
                                      static_cast<int>(dpi)));
        WritePrivateProfileStringW(L"UI", L"WinH", buf, path.c_str());
    }
}

//-----------------------------------------------------------------------------
// ペン設定プリセット (設計確定)。10 スロット。
// 専用 INI (CJFPreviewPenTool.presets.ini) の [Preset01..10] へ保存する。
// 操作: 左クリック = 現在のレイヤーへ適用 / 右クリック = 現在の設定を保存。
// (修飾キー方式より右クリック保存を採用: キーボード不要で本体側のタイピングと
// 衝突せず、この UI の「右クリック=補助操作」の規約とも一致する)
// StylePreset / g_presets の定義は状態セクション参照。

static std::wstring presets_ini_path() {
    std::wstring p;
    if (config_handle && config_handle->app_data_path) {
        p = config_handle->app_data_path;
        if (!p.empty() && p.back() != L'\\') p += L'\\';
        p += L"Plugin\\CJF";
    }
    return p + L"\\CJFPreviewPenTool.presets.ini";
}

// セッション開始時に全スロットを読み込む
static void load_style_presets() {
    std::wstring path = presets_ini_path();
    for (int n = 1; n <= 10; ++n) {
        g_presets[n] = {};
        if (path.empty()) continue;
        wchar_t sec[16], buf[64];
        swprintf_s(sec, L"Preset%02d", n);
        if (GetPrivateProfileStringW(sec, L"W", L"", buf, 64, path.c_str()) <= 0)
            continue; // 空スロット
        g_presets[n].valid = true;
        g_presets[n].w = clampd(wcstod(buf, nullptr), 1.0, 2000.0);
        GetPrivateProfileStringW(sec, L"Ti", L"0", buf, 64, path.c_str());
        g_presets[n].ti = clampd(wcstod(buf, nullptr), 0.0, 50.0);
        GetPrivateProfileStringW(sec, L"Twi", L"0", buf, 64, path.c_str());
        g_presets[n].twi = clampd(wcstod(buf, nullptr), 0.0, 100.0);
        GetPrivateProfileStringW(sec, L"To", L"0", buf, 64, path.c_str());
        g_presets[n].to = clampd(wcstod(buf, nullptr), 0.0, 50.0);
        GetPrivateProfileStringW(sec, L"Two", L"0", buf, 64, path.c_str());
        g_presets[n].two_ = clampd(wcstod(buf, nullptr), 0.0, 100.0);
        GetPrivateProfileStringW(sec, L"Col", L"", buf, 64, path.c_str());
        unsigned v = 0xffffff;
        int digits = 0;
        for (const wchar_t* q = buf; *q && digits < 6; ++q) {
            int d;
            if (*q >= L'0' && *q <= L'9') d = *q - L'0';
            else if (*q >= L'a' && *q <= L'f') d = *q - L'a' + 10;
            else if (*q >= L'A' && *q <= L'F') d = *q - L'A' + 10;
            else break;
            v = v * 16 + d;
            ++digits;
        }
        if (digits == 6)
            g_presets[n].col = RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
    }
}

//-----------------------------------------------------------------------------
// プリセット確認オーバーレイ (設計確定)。
// 別ウィンドウを作らず本体の描画パイプライン内に確認 UI を重ねる。
// ウィンドウ生成を行わないため、表示時の白チラつき・閉じ時の再合成が
// 構造的に発生しない。位置はプリセットボタンのすぐ右。
//-----------------------------------------------------------------------------

// 確認オーバーレイの矩形を計算する
static void layout_confirm_overlay() {
    int w = dpi_s(238), h = dpi_s(58);
    int x = left_panel_px() + dpi_s(12);
    int y = (g_preset_rects[4].top + g_preset_rects[9].bottom) / 2 - h / 2;
    x = clamp_int(x, dpi_s(4), std::max(dpi_s(4), client_w - w - dpi_s(8)));
    y = clamp_int(y, dpi_s(4),
                  std::max(dpi_s(4), client_h - BAR_BOTTOM_H - h - dpi_s(6)));
    g_cnf_box_rect = { x, y, x + w, y + h };
    int bw = dpi_s(64), bh = dpi_s(20);
    int by = y + h - bh - dpi_s(7);
    g_cnf_no_rect  = { x + w - bw - dpi_s(8), by, x + w - dpi_s(8), by + bh };
    g_cnf_yes_rect = { x + w - bw * 2 - dpi_s(14), by, x + w - bw - dpi_s(11), by + bh };
}

// 本体 DIB へ確認オーバーレイを描画する (redraw_frame の最上位から呼ぶ)
static void draw_confirm_overlay(HDC dc) {
    if (!g_confirm_active) return;
    fill_rect_dc(dc, g_cnf_box_rect, RGB(30, 30, 36));
    HPEN pen = CreatePen(PS_SOLID, 1, COL_SEL_ACCENT);
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, g_cnf_box_rect.left, g_cnf_box_rect.top,
              g_cnf_box_rect.right - 1, g_cnf_box_rect.bottom - 1);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);

    RECT mr = { g_cnf_box_rect.left + dpi_s(10), g_cnf_box_rect.top + dpi_s(7),
                g_cnf_box_rect.right - dpi_s(10), g_cnf_yes_rect.top - dpi_s(4) };
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, COL_TEXT_MAIN);
    HGDIOBJ of = SelectObject(dc, g_font_small);
    DrawTextW(dc, g_confirm_msg, -1, &mr,
              DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, of);

    bool hy = g_hover.zone == HitZone::ConfirmYes;
    bool hn = g_hover.zone == HitZone::ConfirmNo;
    draw_button_face(dc, g_cnf_yes_rect, L"はい", true, hy, false, true);
    draw_button_face(dc, g_cnf_no_rect, L"いいえ", true, hn, false, false);
}

// プリセット操作の確認。「確認しない」チェック中は無条件で続行。
// メッセージ例: 「現在の設定をプリセット05で上書き」
// ボタン [はい] 以外 (いいえ / 外側クリック / Esc) はキャンセル扱い。
static bool confirm_preset_op(const wchar_t* message) {
    if (g_preset_confirm_disable) return true;
    if (!frame_window || !IsWindow(frame_window)) return true;
    wcsncpy_s(g_confirm_msg, sizeof(g_confirm_msg) / sizeof(wchar_t), message, _TRUNCATE);
    layout_confirm_overlay();
    g_confirm_result = false;
    g_confirm_done = false;
    g_confirm_active = true;
    in_modal_dialog = true; // Esc ポーリング等を停止
    redraw_frame();
    MSG m;
    while (!g_confirm_done && GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    g_confirm_active = false;
    in_modal_dialog = false;
    redraw_frame();
    return g_confirm_result;
}

// 右クリック: 現在のレイヤー設定をスロット n へ保存
// (既存スロットへの上書き時のみ確認。チェックで抑制可)
static void save_style_preset(int n) {
    if (n < 1 || n > 10) return;
    std::wstring path = presets_ini_path();
    if (path.empty()) return;
    if (g_presets[n].valid) {
        wchar_t msg[96];
        swprintf_s(msg, L"現在の設定をプリセット%02dで上書き", n);
        if (!confirm_preset_op(msg)) return;
    }
    const PenLayerStyle& s = g_layer_styles[g_cur_layer];
    wchar_t sec[16], buf[32];
    swprintf_s(sec, L"Preset%02d", n);
    swprintf_s(buf, L"%02x%02x%02x",
               static_cast<unsigned>(GetRValue(s.col)),
               static_cast<unsigned>(GetGValue(s.col)),
               static_cast<unsigned>(GetBValue(s.col)));
    WritePrivateProfileStringW(sec, L"Col", buf, path.c_str());
    swprintf_s(buf, L"%g", s.w);    WritePrivateProfileStringW(sec, L"W",   buf, path.c_str());
    swprintf_s(buf, L"%g", s.ti);   WritePrivateProfileStringW(sec, L"Ti",  buf, path.c_str());
    swprintf_s(buf, L"%g", s.twi);  WritePrivateProfileStringW(sec, L"Twi", buf, path.c_str());
    swprintf_s(buf, L"%g", s.to);   WritePrivateProfileStringW(sec, L"To",  buf, path.c_str());
    swprintf_s(buf, L"%g", s.two_); WritePrivateProfileStringW(sec, L"Two", buf, path.c_str());
    g_presets[n].valid = true;
    g_presets[n].col = s.col;
    g_presets[n].w = s.w; g_presets[n].ti = s.ti; g_presets[n].twi = s.twi;
    g_presets[n].to = s.to; g_presets[n].two_ = s.two_;
    redraw_frame();
    if (logger) {
        wchar_t m[96];
        swprintf_s(m, L"[CJF PenTool] preset %02d saved", n);
        logger->log(logger, m);
    }
}

// 左クリック: スロット n の設定を現在のレイヤーへ適用
static void apply_style_preset(int n) {
    if (n < 1 || n > 10 || !g_presets[n].valid) {
        if (logger) logger->log(logger, L"[CJF PenTool] preset slot is empty");
        return;
    }
    PenLayerStyle& d = g_layer_styles[g_cur_layer];
    const StylePreset& p = g_presets[n];
    wchar_t msg[96];
    swprintf_s(msg, L"プリセット%02dを現在の設定で上書き", n);
    if (!confirm_preset_op(msg)) return;
    d.col = p.col;
    d.w = p.w; d.ti = p.ti; d.twi = p.twi; d.to = p.to; d.two_ = p.two_;
    sync_hsv_from_color();
    update_color_box_texts();
    redraw_frame();
}

// 「全レイヤーに適用」: 現在レイヤーの設定を L1..L5 へ複写する
static void apply_settings_to_all_layers() {
    PenLayerStyle cur = g_layer_styles[g_cur_layer];
    for (int L = 1; L <= PEN_LAYER_MAX; ++L) g_layer_styles[L] = cur;
    redraw_frame();
}

static void hide_frame() {
    if (frame_window && IsWindow(frame_window)) {
        KillTimer(frame_window, ESC_POLL_TIMER_ID);
        KillTimer(frame_window, RESIZE_DEBOUNCE_TIMER_ID);
        KillTimer(frame_window, CONFIRM_RETRY_TIMER_ID);
        ShowWindow(frame_window, SW_HIDE);
    }
    save_ui_settings(frame_window);
    // 診断: GDI リーク監視 (正常なら 100 未満程度で安定する)
    if (logger) {
        wchar_t m[128];
        swprintf_s(m, L"[CJF PenTool] GDI objects: %lu",
                   GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS));
        logger->log(logger, m);
    }
    mode = Mode::Idle;
    pen_down = false;
    strokes.clear();
    cleanup_committed_cache();
    g_live_from = 0;
    undo_stack.clear();
    redo_stack.clear();
    // 消しゴム状態のリセット (ツールはペンへ戻す)
    g_active_tool = ToolKind::Pen;
    erase_down = false;
    g_erase_base.clear();
    g_erase_geom.clear();
    g_erase_path.clear();
    g_erase_iv.clear();
    g_erase_dot.clear();
    g_erase_dirty = false;
    g_eraser_outside = false;
    g_erase_need_rebuild = false;
    g_cursor_valid = false;
    g_cur_layer = 1;
    for (int L = 1; L <= PEN_LAYER_MAX; ++L) g_layer_visible[L] = true;
    for (int L = 1; L <= PEN_LAYER_MAX; ++L) g_layer_playback_hidden[L] = false;
    g_drag_layer_src = -1;
    g_drop_gap = -1;
    g_legacy_object = false;
    g_legacy_warned = false;
    g_new_t_offset = 0;
    g_hover = {};
    g_drag_slider = -1;
    g_drag_wheel = false;
    g_drag_square = false;
    g_drag_sep = -1;
    for (auto& s : g_sliders) { s.hover = false; s.drag = false; }
    g_eraser_slider.hover = false;
    g_eraser_slider.drag = false;
    // 異常系の安全網: 透過レンダリング用に非表示にしたレイヤーが残っていれば復帰
    restore_hidden_layer();
    // ホットキー解除（ペンモード終了時。登録されていなくても無害）
    UnregisterHotKey(frame_window, HOTKEY_UNDO_ID);
    UnregisterHotKey(frame_window, HOTKEY_REDO_ID);
    remove_wheel_hook();
    if (GetCapture() == frame_window) ReleaseCapture();
    // ホスト入力を再許可してからフォーカスを返す (ペンモード中は無効化済み)
    if (host_window && IsWindow(host_window)) EnableWindow(host_window, TRUE);
    if (host_window && IsWindow(host_window)) SetFocus(host_window);
}

static void cancel_pen_mode() {
    if (logger) logger->log(logger, L"[CJF PenTool] canceled (Esc)");
    // 消しゴムドラッグ中のキャンセル: 未コミットの編集を破棄して状態を戻す
    erase_down = false;
    g_eraser_outside = false;
    g_erase_base.clear();
    g_erase_geom.clear();
    g_erase_path.clear();
    g_erase_iv.clear();
    g_erase_dot.clear();
    g_erase_dirty = false;
    g_erase_need_rebuild = false;
    pen_trigger_object = nullptr;
    hide_frame();
}

// 「線をクリア」: 座標列を空にする（1 つの Undo エントリ）。
// 既に空の場合は何もしない（無駄な Undo を積まない）。
static void handle_clear_request() {
    if (!edit_handle) return;
    // ペンモード中の外部クリアはセッション strokes と不整合になるため拒否
    if (mode == Mode::PenDraw) {
        if (logger) logger->log(logger, L"[CJF PenTool] clear request ignored during pen mode");
        return;
    }
    OBJECT_HANDLE obj = clear_request_object;
    clear_request_object = nullptr;
    if (!obj) return;
    edit_handle->call_edit_section_param(&obj, [](void* param, EDIT_SECTION* edit) {
        OBJECT_HANDLE o = *static_cast<OBJECT_HANDLE*>(param);
        __try {
            if (edit->count_object_effect(o, L"ペンツール") <= 0) return;
            const char* v = edit->get_object_item_value(o, L"ペンツール", L"線の座標(シーンpx・ミリ秒)");
            if (!v || v[0] == '\0') return;
            edit->set_object_item_value(o, L"ペンツール", L"線の座標(シーンpx・ミリ秒)", "");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    });
    if (logger) logger->log(logger, L"[CJF PenTool] cleared stroke data");
}

static void begin_pen_mode();

// 透過レンダリング用に非表示にしたレイヤーを復帰する (冪等)
static void restore_hidden_layer() {
    if (g_hidden_layer < 0 || !edit_handle) return;
    struct OnCtx { int layer; } oc = { g_hidden_layer };
    g_hidden_layer = -1;
    edit_handle->call_edit_section_param(&oc, [](void* param, EDIT_SECTION* edit) {
        OnCtx* c = static_cast<OnCtx*>(param);
        __try {
            edit->set_layer_enable(c->layer, true);
            edit->set_edited_state();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    });
}

static void finish_pen_mode(RenderCtx* ctx) {
    if (!ctx->copied) {
        frame_rgba.clear();
        render_in_progress = false;
        delete ctx;
        return;
    }
    frame_w = ctx->w;
    frame_h = ctx->h;

    // 透過レンダリング完了: 非表示にしていたレイヤーを即復帰させる
    // (canvas 画像は取得済みなので本体プレビューへすぐ元の線が戻る)
    restore_hidden_layer();

    // ウィンドウを表示 (初回生成済み。位置はホスト中央、サイズは現状維持)
    RECT host_rc = {}, win_rc = {};
    GetWindowRect(host_window, &host_rc);
    GetWindowRect(frame_window, &win_rc);
    int win_w = win_rc.right - win_rc.left, win_h = win_rc.bottom - win_rc.top;
    int host_w = static_cast<int>(host_rc.right - host_rc.left);
    int host_h = static_cast<int>(host_rc.bottom - host_rc.top);
    int hx = host_rc.left + std::max(0, (host_w - win_w) / 2);
    int hy = host_rc.top + std::max(0, (host_h - win_h) / 2);
    SetWindowPos(frame_window, HWND_TOPMOST, hx, hy, win_w, win_h,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    // ペンモード中はホスト (AviUtl2 本体) への入力を遮断する。
    // 本体にフォーカスが残ると P/B/E 等が本体側のショートカットとして働き
    // 意図しないメニューが出るため。無効化するとキーボード/マウス入力が
    // 本体へ届かず、キーはフォーカス先なしで破棄される
    // (自ウィンドウは GetAsyncKeyState ポーリング + RegisterHotKey なので影響なし)。
    if (host_window && IsWindow(host_window)) {
        EnableWindow(host_window, FALSE);
        // 前景も自ウィンドウへ移しておく (Esc/E/P/B のゲートを即座に開く)
        SetForegroundWindow(frame_window);
        SetFocus(frame_window);
    }

    RECT crc = {};
    GetClientRect(frame_window, &crc);
    client_w = crc.right - crc.left;
    client_h = crc.bottom - crc.top;
    do_layout();

    // プレビュー用の全レイヤーのスタイルを対象オブジェクトから読み取る。
    // 読めなかったレイヤーは L1 値で初期化される (obj2 フォールバックと同じ挙動)。
    for (int L = 1; L <= PEN_LAYER_MAX; ++L) {
        g_layer_styles[L] = PenLayerStyle{};
        g_style_init[L] = {};
    }
    g_legacy_object = false;
    PenStyleCtx style = { pen_trigger_object, ctx->frame, false, {}, 0 };
    if (style.obj) {
        edit_handle->call_read_section_param(&style, read_pen_style_edit);
        if (!style.crashed) {
            for (int L = 1; L <= PEN_LAYER_MAX; ++L) {
                PenLayerStyle& d = g_layer_styles[L];
                const PenLayerStyle& s = style.styles[L];
                d.col = s.col;
                d.w = std::max(1.0, s.w);
                d.ti = std::max(0.0, s.ti);
                d.to = std::max(0.0, s.to);
                d.twi = clampd(s.twi, 0, 100);
                d.two_ = clampd(s.two_, 0, 100);
                // 差分書込の基準値として保持
                g_style_init[L] = { true, d.col, d.w, d.ti, d.twi, d.to, d.two_ };
            }
            // 新旧判別 : 旧構成オブジェクトは L2 以降を使えない
            g_legacy_object = !style.modern;
        }
    }
    if (logger && g_legacy_object && style.obj) {
        logger->log(logger,
                    L"[CJF PenTool] legacy object detected (no v2 items); layers 2-5 disabled");
    }
    g_cur_layer = 1;
    for (int L = 1; L <= PEN_LAYER_MAX; ++L) g_layer_visible[L] = true;
    rebind_layer_sliders();
    sync_hsv_from_color();
    update_color_box_texts();

    // ---- 既存ストロークの事前読込。セッション配列へ統合する (P2) ----
    strokes.clear();
    for (int L = 1; L <= PEN_LAYER_MAX; ++L) g_layer_playback_hidden[L] = false;
    load_style_presets(); // プリセット 01..10 を専用 INI から読込
    struct LoadPtsCtx {
        OBJECT_HANDLE obj;
        std::string existing;
        char lhide[8];
        int crashed;
    } lp = { pen_trigger_object, {}, "", 0 };
    edit_handle->call_read_section_param(&lp, [](void* param, EDIT_SECTION* edit) {
        LoadPtsCtx* c = static_cast<LoadPtsCtx*>(param);
        __try {
            // 対象解決: 明示トリガー (右クリック/パネル起動) が無い場合
            // (編集メニュー「ペンで描く」等) は 選択中 → フォーカス の順で
            // 最初の ペンツール オブジェクトを使う (確定時の解決と同一規約)。
            // これを怠ると既存線が背景画像としてしか表示されず、スライダーや
            // 色の変更が既存線に反映されなくなる。
            if (!c->obj) {
                auto is_target = [&edit](OBJECT_HANDLE o) {
                    return o && edit->count_object_effect(o, L"ペンツール") > 0;
                };
                int n = edit->get_selected_object_num();
                for (int i = 0; i < n && !c->obj; ++i) {
                    OBJECT_HANDLE o = edit->get_selected_object(i);
                    if (is_target(o)) c->obj = o;
                }
                if (!c->obj) {
                    OBJECT_HANDLE o = edit->get_focus_object();
                    if (is_target(o)) c->obj = o;
                }
            }
            if (!c->obj) return;
            if (edit->count_object_effect(c->obj, L"ペンツール") <= 0) return;
            const char* e = edit->get_object_item_value(
                c->obj, L"ペンツール", L"線の座標(シーンpx・ミリ秒)");
            if (e) {
                // 旧ビルドのエイリアス作成で "" （引用符2文字）が残った
                // オブジェクトを救済するため、先頭/末尾の引用符を除去する。
                const char* b = e;
                const char* en = e + strlen(e);
                while (b < en && *b == '"') ++b;
                while (en > b && en[-1] == '"') --en;
                c->existing.assign(b, en);
            }
            const char* lh = edit->get_object_item_value(c->obj, L"ペンツール", L"lhide");
            if (lh && lh[0]) {
                strncpy_s(c->lhide, sizeof(c->lhide), lh, _TRUNCATE);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            c->crashed = 1;
        }
    });
    // 解決した対象を以後の適用先として固定する (確定時の優先候補になる)
    if (lp.obj && !pen_trigger_object) pen_trigger_object = lp.obj;
    // 再生時非表示レイヤー (lhide) の復元
    for (int L = 1; L <= PEN_LAYER_MAX; ++L)
        g_layer_playback_hidden[L] = lp.lhide[L - 1] == '1';
    if (!lp.existing.empty()) {
        parse_pts_string(lp.existing, strokes);
        DWORD max_t = 0;
        size_t total_pts = 0;
        for (auto& st : strokes) {
            for (size_t i = 1; i < st.size(); ++i) {
                double dx = st[i].sx - st[i - 1].sx;
                double dy = st[i].sy - st[i - 1].sy;
                st[i].acc = st[i - 1].acc + std::sqrt(dx * dx + dy * dy);
            }
            // 原本時刻基準で最大を取る。u<orig> を持つ圧縮済ストロークは
            // t が圧縮後なので p.t 単体では過小評価になり、新規 t が過去に
            // 割り当てられて次回 compress で中間挿入→既存順序崩れを起こす。
            double shift = 0.0;
            if (!st.empty() && st.front().oshift != 0) {
                shift = static_cast<double>(st.front().oshift) -
                        static_cast<double>(st.front().t);
            }
            for (const auto& p : st) {
                // 消失時刻 (ゴースト断片の d) も排番起点に含める。
                // 再起動後の描き足しが過去の消失時刻を追い越さないようにする
                double orig_t = static_cast<double>(p.t) + shift;
                double orig_vanish = p.vanish ? static_cast<double>(p.vanish) + shift : 0.0;
                // DWORD へ丸めて比較 (build と同規約 +0.5 は不要。max は単調なので double のまま)
                DWORD cand_t = static_cast<DWORD>(orig_t + 0.5);
                DWORD cand_v = static_cast<DWORD>(orig_vanish + 0.5);
                max_t = std::max({ max_t, cand_t, cand_v });
                ++total_pts;
            }
        }
        // 追記型 t 採番: 新規ストロークは既存最終 t + 300ms から続く
        g_new_t_offset = max_t + 300;
        if (logger) {
            wchar_t m[256] = {};
            swprintf_s(m, L"[CJF PenTool] loaded %d existing stroke(s), %zu pts (%zu chars)",
                       static_cast<int>(strokes.size()), total_pts, lp.existing.size());
            logger->log(logger, m);
        }
    } else {
        g_new_t_offset = 300;
    }

    build_canvas_image();
    mode = Mode::PenDraw;
    g_live_from = strokes.size();
    undo_stack.clear();
    redo_stack.clear();
    pen_down = false;
    pen_mode_start = GetTickCount();
    RegisterHotKey(frame_window, HOTKEY_UNDO_ID, MOD_CONTROL | MOD_NOREPEAT, 'Z');
    RegisterHotKey(frame_window, HOTKEY_REDO_ID, MOD_CONTROL | MOD_NOREPEAT, 'Y');
    install_wheel_hook();

    // 診断: 子ウィンドウ (EDIT) の存在と位置を確認
    if (logger) {
        int child_count = 0;
        (void)child_count;
    }

    redraw_frame();
    SetTimer(frame_window, ESC_POLL_TIMER_ID, ESC_POLL_INTERVAL_MS, nullptr);
    render_in_progress = false;
    delete ctx;
}

static void begin_pen_mode() {
    if (mode != Mode::Idle || render_in_progress || !frame_window || !edit_handle) return;
    render_in_progress = true;

    EDIT_INFO info = {};
    edit_handle->get_edit_info(&info, sizeof(info));
    if (info.width < 1 || info.height < 1) {
        render_in_progress = false;
        if (logger) logger->warn(logger, L"[CJF PenTool] no scene (invalid size), pen mode aborted");
        return;
    }

    // ---- 対象解決 + レイヤー一時非表示 (透過レンダリング準備) ----
    // 既存線が背景画像に写らないよう、対象オブジェクトの属するレイヤーを
    // 一時的に非表示にしてからレンダリングする。既存線は strokes (セッション配列) として
    // ベクター描画されるため、見えなくなることはない。
    struct HideCtx {
        OBJECT_HANDLE obj;
        int layer;
        bool was_on;
        bool hidden;   // 非表示へ切り替えできたか
        int crashed;
    } hc = { pen_trigger_object, -1, false, false, 0 };
    edit_handle->call_edit_section_param(&hc, [](void* param, EDIT_SECTION* edit) {
        HideCtx* c = static_cast<HideCtx*>(param);
        __try {
            auto is_target = [&](OBJECT_HANDLE o) {
                return o && edit->count_object_effect(o, L"ペンツール") > 0;
            };
            if (!c->obj) {
                int n = edit->get_selected_object_num();
                for (int i = 0; i < n && !c->obj; ++i) {
                    OBJECT_HANDLE o = edit->get_selected_object(i);
                    if (is_target(o)) c->obj = o;
                }
                if (!c->obj) {
                    OBJECT_HANDLE o = edit->get_focus_object();
                    if (is_target(o)) c->obj = o;
                }
            }
            if (!c->obj) return; // 対象なし → 透過せず通常レンダリング
            OBJECT_LAYER_FRAME lf = edit->get_object_layer_frame(c->obj);
            if (lf.layer < 0) return;
            c->layer = lf.layer;
            c->was_on = edit->get_layer_enable(c->layer);
            if (c->was_on) {
                edit->set_layer_enable(c->layer, false);
                edit->set_edited_state(); // ユーザー Undo の対象にしない (maskselector 実績)
                c->hidden = true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            c->crashed = 1;
        }
    });

    if (!hc.crashed && hc.hidden && hc.layer >= 0) {
        // 透過レンダリング: レイヤー切替がシーンキャッシュへ反映されるまで
        // メッセージループへ戻って待つ必要がある (200ms、maskselector 実績値)
        g_hidden_layer = hc.layer;
        if (!pen_trigger_object) pen_trigger_object = hc.obj;
        frame_rgba.assign(static_cast<size_t>(info.width) * info.height * 4, 0);
        SetTimer(frame_window, HIDDEN_RENDER_TIMER_ID, HIDDEN_RENDER_DELAY_MS, nullptr);
        return; // タイマー経由でレンダリング開始
    }

    // フォールバック: 透過なしで即レンダリング (対象なし / 編集セクション失敗)
    g_hidden_layer = -1;
    frame_rgba.assign(static_cast<size_t>(info.width) * info.height * 4, 0);
    if (!render_current_frame(info.frame, frame_rgba.data(), info.width, info.height)) {
        frame_rgba.clear();
        render_in_progress = false;
        return;
    }
    // 完了後の表示処理はWM_CJF_RENDER_COMPLETEから続行する。
    return;
}

//-----------------------------------------------------------------------------
// ストローク操作 (scene 座標)
//-----------------------------------------------------------------------------

// 左ドラッグ開始: 現在レイヤーへ新しいストロークを追加（既存ストロークは保持）
static void start_new_stroke_at(double sx, double sy, int lyr) {
    std::vector<PenPoint> st;
    PenPoint pp;
    pp.sx = sx;
    pp.sy = sy;
    pp.t = g_new_t_offset + (GetTickCount() - pen_mode_start);
    pp.acc = 0.0; // ストローク先頭
    pp.layer = lyr;
    st.push_back(pp);
    strokes.push_back(std::move(st));
}

static void begin_stroke(const POINT& pt_raw) {
    if (!pt_in_canvas(pt_raw)) return; // キャンバス外開始は無効
    double sx, sy;
    client_to_scene(pt_raw, &sx, &sy);
    // 所属レイヤー確定 (: 描画開始時の現在レイヤーで決まる)
    int lyr = g_cur_layer;
    if (g_legacy_object && lyr > 1) {
        // ガード: 旧構成オブジェクトに L2 以降は書けないため L1 へ統合する
        lyr = 1;
        if (!g_legacy_warned && logger) {
            logger->warn(logger,
                         L"[CJF PenTool] legacy object: drawing on layer >=2 is merged into L1");
            g_legacy_warned = true;
        }
    }
    pen_down = true;
    g_pen_outside = false;
    SetCapture(frame_window);
    // 新しいストロークを描くと redo 履歴は無効（標準的な Undo/Redo の挙動）
    redo_stack.clear();
    g_gesture_before = strokes;
    g_gesture_dirty = true;
    g_live_from = strokes.size();
    start_new_stroke_at(sx, sy, lyr);
    redraw_frame();
}

static void extend_stroke(const POINT& pt_raw) {
    if (!pen_down || strokes.empty()) return;
    if (!pt_in_canvas(pt_raw)) {
        // キャンバス外: 記録を打ち切り、戻ってきたら新しいストロークとして再開
        g_pen_outside = true;
        return;
    }
    double sx, sy;
    client_to_scene(pt_raw, &sx, &sy);
    if (g_pen_outside) {
        g_pen_outside = false;
        start_new_stroke_at(sx, sy, strokes.back().front().layer);
        redraw_frame();
        return;
    }
    std::vector<PenPoint>& st = strokes.back();
    const PenPoint& last = st.back();
    double dx = sx - last.sx;
    double dy = sy - last.sy;
    // シーン座標で 1px 以上動いたら記録（補間なし）。
    // イベント間を距離刻みで補間すると、速く描くほど 1 イベントで追加する点が
    // 数十個になり、点の総数が爆発的に増える（→ プレビュー描画・再生メッシュ生成が
    // 二次関数的に重くなり、AviUtl2 ごと固まることがある）。
    if (dx * dx + dy * dy >= 1.0) {
        PenPoint pp = { sx, sy, g_new_t_offset + (GetTickCount() - pen_mode_start),
                        last.acc + std::sqrt(dx * dx + dy * dy), last.layer };
        st.push_back(pp);
        redraw_frame();
    }
}

static void append_end_point(const POINT& pt_raw) {
    if (!pt_in_canvas(pt_raw)) return; // 外側での終端付けはしない
    double sx, sy;
    client_to_scene(pt_raw, &sx, &sy);
    std::vector<PenPoint>& st = strokes.back();
    const PenPoint& last = st.back();
    double dx = sx - last.sx;
    double dy = sy - last.sy;
    if (dx * dx + dy * dy >= 0.25) {
        PenPoint pp = { sx, sy, g_new_t_offset + (GetTickCount() - pen_mode_start),
                        last.acc + std::sqrt(dx * dx + dy * dy), last.layer };
        st.push_back(pp);
    }
}

// ペンアップしたジェスチャ全体を履歴へ登録 (ReplaceStrokes オペレーション)。
// ジェスチャ内でキャンバス出入りによりストロークが分割されても 1 Undo で復帰する。
static void commit_gesture_history() {
    if (!g_gesture_dirty) return;
    HistoryOp op;
    op.kind = HistoryOp::Kind::ReplaceStrokes;
    capture_layer_state(op.styles_before, op.vis_before, op.pbh_before);
    op.before = g_gesture_before;
    op.after = strokes;
    capture_layer_state(op.styles_after, op.vis_after, op.pbh_after);
    history_push(undo_stack, std::move(op));
    g_gesture_dirty = false;
}

static void finish_current_stroke(const POINT& pt) {
    if (!pen_down) return;
    // ReleaseCapture が WM_CAPTURECHANGED を同期的に送出するため、
    // pen_down を先に落としておく (二重終端・二重履歴コミットの防止)
    pen_down = false;
    g_pen_outside = false;
    ReleaseCapture();
    if (strokes.empty()) return;
    append_end_point(pt);
    commit_gesture_history();
    g_gesture_before.clear();
    g_live_from = strokes.size();
    mark_canvas_dirty();
    redraw_frame();
}

static void end_stroke(const POINT& pt) {
    if (!pen_down) return;
    finish_current_stroke(pt);
}

//-----------------------------------------------------------------------------
// 消しゴム。ベクター断片化方式。
// 消去区間をゴースト断片 (d 属性付き) として保持し、「引かれた後に消える」
// 再生にする (動作確認で P3 スコープへ繰り入れ済み。)。
// 注: サイズ定数 (ERASER_SIZE_*) とバケット関数はファイル先頭の
// 消しゴム状態ブロックにある (スライダー行 g_eraser_slider から参照)。
//-----------------------------------------------------------------------------

static std::vector<pengeom::GeomPoint> stroke_to_geom(const std::vector<PenPoint>& st) {
    std::vector<pengeom::GeomPoint> g;
    g.reserve(st.size());
    for (const auto& p : st)
        g.push_back({ p.sx, p.sy, static_cast<double>(p.t) });
    return g;
}

// g_erase_base + g_erase_iv/g_erase_dot から strokes を作り直す。
// 可視断片は complement run、消去区間は時刻タグ付きのゴースト断片になる
// (案 A: 手の動きに沿って順番に消える)。ゴーストはプレビューでは非描画
// (最終状態の表示) だがデータとして保持され、確定時に座標列へ書き出されて
// 「引かれた後に消える」再生を実現する。
static void rebuild_erase_preview() {
    strokes.clear();
    strokes.reserve(g_erase_base.size() + 4);
    for (size_t i = 0; i < g_erase_base.size(); ++i) {
        const auto& base = g_erase_base[i];
        if (base.empty()) continue;
        if (g_erase_dot[i]) continue; // ドット抹消: 幾何ごと消える
        if (g_erase_iv[i].empty()) {
            strokes.push_back(base);
            continue;
        }
        auto frags = pengeom::fragment_stroke_tagged(
            g_erase_geom[i], g_erase_iv[i], ERASER_MIN_FRAG_PX);
        // テーパー引継ぎ : 元ストローク始端を含む断片のみ入り有効、
        // 終端を含む断片のみ抜き有効。元が既に断片ならその属性を引き継ぐ。
        const bool base_in = base.front().taper_in;
        const bool base_out = base.front().taper_out;
        const int layer = clamp_int(base.front().layer, 1, PEN_LAYER_MAX);
        for (auto& f : frags) {
            const bool tin = f.keep_start && base_in;
            const bool tout = f.keep_end && base_out;
            const DWORD vanish =
                f.ghost ? static_cast<DWORD>(std::llround(f.vanish_ms)) : 0;
            std::vector<PenPoint> out;
            out.reserve(f.pts.size());
            double acc = 0.0;
            for (size_t k = 0; k < f.pts.size(); ++k) {
                if (k > 0) {
                    const double ddx = f.pts[k].x - f.pts[k - 1].x;
                    const double ddy = f.pts[k].y - f.pts[k - 1].y;
                    acc += std::sqrt(ddx * ddx + ddy * ddy);
                }
                PenPoint p;
                p.sx = f.pts[k].x;
                p.sy = f.pts[k].y;
                p.t = static_cast<DWORD>(std::llround(f.pts[k].t));
                p.acc = acc;
                p.layer = layer;
                p.taper_in = tin;
                p.taper_out = tout;
                p.vanish = vanish;
                // 原本時刻の保持 (P4 u<ms> トークン)。断片が元ストロークの
                // 累積シフトを失うと、▶ トグル後の再確定でその断片だけ
                // 正しい時刻へ復帰できなくなる
                p.oshift = base.front().oshift;
                out.push_back(p);
            }
            strokes.push_back(std::move(out));
        }
    }
}

// 1 サンプル分の消去判定を適用する。何か消えたら true。
// prev→cur のカプセル (線分+半径) で判定するため高速移動でも取りこぼさない。
// prev は直前サンプルを呼び出し側が明示的に渡す (path.back をここで読むと
// push 済みの現サンプルを掴んでカプセルが縮退する——P3 レビュー F1)。
// ヒット区間にはこのサンプルの操作時刻 (バケット丸め) をタグ付けする。
// 後から触れた範囲ほど遅い時刻に消える (insert_tagged の上書き規則)。
// 変化があったストロークの「ベース bbox 全体」を部分更新領域へ加算する。
// 切断されると残存ピースのテーパー配分がピース全長にわたって変わるため、
// カプセル近傍だけの加算では領域外に古い描画が残る (レビュー A1)
static void mark_region_for_changed_stroke(size_t base_idx) {
    if (!g_cc_dc || g_cc_dirty) return;
    const auto& st = g_erase_base[base_idx];
    if (st.empty()) return;
    const float sc = std::max(0.0001f, g_view.scale);
    double mnx = 1e18, mny = 1e18, mxx = -1e18, mxy = -1e18;
    for (const auto& p : st) {
        mnx = std::min(mnx, p.sx); mxx = std::max(mxx, p.sx);
        mny = std::min(mny, p.sy); mxy = std::max(mxy, p.sy);
    }
    const int Lx = clamp_int(st.front().layer, 1, PEN_LAYER_MAX);
    const double md = g_layer_styles[Lx].w * 0.5 * sc + 3.0;
    RECT rr;
    rr.left = clamp_int(static_cast<int>(std::lround(mnx * sc - md)), 0, g_cc_w);
    rr.top = clamp_int(static_cast<int>(std::lround(mny * sc - md)), 0, g_cc_h);
    rr.right = clamp_int(static_cast<int>(std::lround(mxx * sc + md)) + 1, 0, g_cc_w);
    rr.bottom = clamp_int(static_cast<int>(std::lround(mxy * sc + md)) + 1, 0, g_cc_h);
    mark_canvas_region_device(rr);
}
static bool apply_erase_sample(const pengeom::GeomPoint& prev,
                               double sx, double sy, DWORD sample_ms) {
    const double r_e = g_eraser_size * 0.5;
    const double bucket =
        static_cast<double>(erase_bucket_ms(sample_ms));
    bool dirty = false;
    for (size_t i = 0; i < g_erase_base.size(); ++i) {
        const auto& geom = g_erase_geom[i];
        if (geom.empty()) continue;
        const auto& b = g_erase_base[i];
        if (b.size() == 1) {
            // 1 点ドット: 中心距離判定で丸ごと抹消
            if (!g_erase_dot[i] &&
                pengeom::point_polyline_dist(sx, sy, geom) <= r_e) {
                g_erase_dot[i] = true;
                dirty = true;
                mark_region_for_changed_stroke(i);
            }
            continue;
        }
        if (pengeom::apply_sample_tagged(&g_erase_iv[i], geom,
                                         prev, {sx, sy, 0.0}, r_e, bucket)) {
            dirty = true;
            mark_region_for_changed_stroke(i);
        }
    }
    // 履歴 push 判定フラグ。ここで立てないと確定時に ReplaceStrokes が
    // 積まれず消しゴムが Undo できなくなる
    if (dirty) g_erase_dirty = true;
    return dirty;
}

static void begin_erase(const POINT& pt_raw) {
    if (!pt_in_canvas(pt_raw)) return; // キャンバス外開始は無効
    double sx, sy;
    client_to_scene(pt_raw, &sx, &sy);
    // 対象レイヤー確定 (ペンと同じ規約: 開始時の現在レイヤー。旧構成は L1)
    g_erase_layer = g_cur_layer;
    if (g_legacy_object && g_erase_layer > 1) {
        g_erase_layer = 1;
        if (!g_legacy_warned && logger) {
            logger->warn(logger,
                         L"[CJF PenTool] legacy object: erasing on layer >=2 is applied to L1");
            g_legacy_warned = true;
        }
    }
    if (!g_eraser_size_ready) {
        // 初期サイズ = 現在レイヤーのペンの太さ ×3
        g_eraser_size = std::max(ERASER_SIZE_MIN,
                                 g_layer_styles[g_cur_layer].w * 3.0);
        g_eraser_size_ready = true;
    }
    // 新しい編集操作で redo は無効 (描画と同じ規約)
    redo_stack.clear();
    g_erase_base = strokes;
    g_erase_geom.assign(g_erase_base.size(), {});
    for (size_t i = 0; i < g_erase_base.size(); ++i) {
        const auto& b = g_erase_base[i];
        // 対象: 対象レイヤーの非ゴースト・非空ストロークのみ
        if (!b.empty() && b.front().vanish == 0 &&
            clamp_int(b.front().layer, 1, PEN_LAYER_MAX) == g_erase_layer)
            g_erase_geom[i] = stroke_to_geom(b);
    }
    g_erase_iv.assign(g_erase_base.size(), {});
    g_erase_dot.assign(g_erase_base.size(), false);
    // 今回ジェスチャの基準時刻。追記型 t 採番により既存全 t (と全消失時刻) より後になる
    g_erase_dtime = g_new_t_offset + (GetTickCount() - pen_mode_start);
    g_erase_path.assign(1, pengeom::GeomPoint{
                               sx, sy,
                               static_cast<double>(erase_bucket_ms(g_erase_dtime)) });
    g_erase_dirty = false;
    g_eraser_outside = false;
    g_erase_need_rebuild = false;
    g_live_from = strokes.size(); // 消去中はライブ分なし (断片は部分更新で反映)
    erase_down = true;
    SetCapture(frame_window);
}

static void extend_erase(const POINT& pt_raw) {
    if (!erase_down || g_erase_path.empty()) return;
    if (!pt_in_canvas(pt_raw)) {
        // キャンバス外は何もしない (端で消えない)。戻ってきたら
        // 前サンプルと直結せず新しい掃き出し点から再開する
        g_eraser_outside = true;
        return;
    }
    double sx, sy;
    client_to_scene(pt_raw, &sx, &sy);
    const DWORD now_ms =
        erase_bucket_ms(g_new_t_offset + (GetTickCount() - pen_mode_start));
    if (g_eraser_outside) {
        // 外→内: パスを区切る (外の間の移動でカプセルがキャンバス内を横断しないように)
        g_eraser_outside = false;
        g_erase_path.assign(1, pengeom::GeomPoint{ sx, sy, static_cast<double>(now_ms) });
        return;
    }
    const pengeom::GeomPoint prev = g_erase_path.back();
    const double dx = sx - prev.x, dy = sy - prev.y;
    if (dx * dx + dy * dy < 1.0) return; // ≥1 シーン px 間隔
    g_erase_path.push_back({ sx, sy, static_cast<double>(now_ms) });
    if (apply_erase_sample(prev, sx, sy, now_ms)) {
        // 再構築はここでは行わず合成周期 (~60Hz) へ deferred。
        // サンプル毎の全体再構築が消去中の重さの原因だったため
        g_erase_need_rebuild = true;
        redraw_frame();
    }
}

// 最終サンプルを反映する (移動していなければ no-op)。キャンバス外での解放は無視。
static void reflect_erase_final_sample(const POINT& pt_raw) {
    if (g_erase_path.empty() || !pt_in_canvas(pt_raw)) return;
    double sx, sy;
    client_to_scene(pt_raw, &sx, &sy);
    const pengeom::GeomPoint prev = g_erase_path.back();
    const double dx = sx - prev.x, dy = sy - prev.y;
    if (dx * dx + dy * dy < 1.0) return;
    const DWORD now_ms =
        erase_bucket_ms(g_new_t_offset + (GetTickCount() - pen_mode_start));
    g_erase_path.push_back({ sx, sy, static_cast<double>(now_ms) });
    if (apply_erase_sample(prev, sx, sy, now_ms))
        g_erase_need_rebuild = true; // 最終再構築は end 側で即時実行
}

// 消しゴムジェスチャの確定: dirty なら履歴 1 オペへ積み、状態を解放する
static void finish_erase_state() {
    g_eraser_outside = false;
    if (g_erase_dirty) {
        // 変更があったときだけ履歴へ (: Ctrl+Z 1 回で完全復帰)
        HistoryOp op;
        op.kind = HistoryOp::Kind::ReplaceStrokes;
        capture_layer_state(op.styles_before, op.vis_before, op.pbh_before);
        op.before = g_erase_base;
        op.after = strokes;
        capture_layer_state(op.styles_after, op.vis_after, op.pbh_after);
        history_push(undo_stack, std::move(op));
        if (logger) {
            logger->log(logger, L"[CJF PenTool] erase committed");
        }
    }
    // 状態解放
    g_erase_base.clear();
    g_erase_geom.clear();
    g_erase_path.clear();
    g_erase_iv.clear();
    g_erase_dot.clear();
    g_erase_dirty = false;
    redraw_frame();
}

// 消しゴム終了時に保留中の再構築と部分更新を即時反映する
static void flush_erase_cache_update() {
    if (g_erase_need_rebuild) {
        g_erase_need_rebuild = false;
        rebuild_erase_preview();
        g_live_from = strokes.size();
    }
    const RECT& ir = g_view.image_rect;
    const bool sig_ok = (canvas_sig() == g_cc_sig);
    if (!g_cc_dirty && sig_ok && g_cc_part_valid && g_cc_dc &&
        update_committed_cache_region(ir, g_cc_part_rect)) {
        g_cc_part_valid = false;
        return;
    }
    if ((g_cc_dirty || !sig_ok || g_cc_part_valid) &&
        ensure_committed_cache(ir)) {
        g_cc_part_valid = false;
    }
}

static void end_erase(const POINT& pt_raw) {
    if (!erase_down) return;
    // ReleaseCapture が WM_CAPTURECHANGED を同期的に送出するため先に落とす
    erase_down = false;
    ReleaseCapture();
    reflect_erase_final_sample(pt_raw);
    flush_erase_cache_update();
    finish_erase_state();
}

// レイヤー選択切替 (定義はレイヤー編集操作節)。スライダーと色 UI を付け替える。
static void select_layer(int L);

// Ctrl+Z: 最後の操作を取り消す (HistoryOp 汎化版)
static void apply_undo() {
    if (mode != Mode::PenDraw || pen_down || erase_down || undo_stack.empty()) return;
    g_erase_need_rebuild = false; // 衛生: 消去保留フラグの持ち越しを禁止
    HistoryOp op = std::move(undo_stack.back());
    undo_stack.pop_back();
    capture_layer_state(op.styles_after, op.vis_after, op.pbh_after); // 現在状態を redo 用に保持
    strokes = op.before;
    restore_layer_state(op.styles_before, op.vis_before, op.pbh_before);
    g_live_from = strokes.size();
    mark_canvas_dirty();
    redo_stack.push_back(std::move(op));
    redraw_frame();
}

// Ctrl+Y: 取り消した操作をやり直す
static void apply_redo() {
    if (mode != Mode::PenDraw || pen_down || erase_down || redo_stack.empty()) return;
    g_erase_need_rebuild = false;
    HistoryOp op = std::move(redo_stack.back());
    redo_stack.pop_back();
    strokes = op.after;
    restore_layer_state(op.styles_after, op.vis_after, op.pbh_after);
    g_live_from = strokes.size();
    mark_canvas_dirty();
    history_push(undo_stack, std::move(op));
    redraw_frame();
}

//-----------------------------------------------------------------------------
// レイヤー編集操作。いずれも ReplaceStrokes 履歴 1 オペレーション。
// レイヤーは「ストローク + 設定 + 表示状態」の単位で扱う (設計確定)。
//-----------------------------------------------------------------------------

// 2 レイヤーの内容と設定を丸ごと入れ替える (ドラッグ並べ替え R4)
// → 挿入方式へ一般化 (apply_layer_permutation)。設計確定:
// 「レイヤーは設定を含む単位で移動」「挿入位置に差し込み、他はシフト」
static void apply_layer_permutation(int src, int gap) {
    if (src < 1 || src > PEN_LAYER_MAX || gap < 0 || gap > PEN_LAYER_MAX) return;
    // seq[j] = 新しいスロット j+1 に入る旧スロット番号
    std::vector<int> seq = { 1, 2, 3, 4, 5 };
    seq.erase(seq.begin() + (src - 1));
    int k = clamp_int(gap - (gap > src - 1 ? 1 : 0), 0, PEN_LAYER_MAX - 1);
    seq.insert(seq.begin() + k, src);
    // 旧→新の写像へ変換 (seq は「新スロット←旧スロット」なので逆引きが必要。
    // これを間違えると 1 スロットずれの回転になり、過去に発見)
    int map_[PEN_LAYER_MAX + 1];
    bool changed = false;
    for (int j = 0; j < PEN_LAYER_MAX; ++j) {
        map_[seq[j]] = j + 1;
        if (seq[j] != j + 1) changed = true;
    }
    if (!changed) return;

    HistoryOp op;
    op.kind = HistoryOp::Kind::ReplaceStrokes;
    op.before = strokes;
    capture_layer_state(op.styles_before, op.vis_before, op.pbh_before);

    for (auto& st : strokes)
        for (auto& p : st)
            p.layer = map_[p.layer];
    // 設定・表示状態・動画出力もスロット間で移動する (設定ごと移動)。
    // 注意: g_style_init (確定時スタイル差分の基準) は移動しない。
    // 基準は「オブジェクトに現状保存されている値」であり、内部ビューの
    // 入替えで動かすと差分がゼロになり確定時に何も書かれず、pts の層番号
    // だけ新配置になって色と内容が分離する (実機チェックリスト で発覚)
    PenLayerStyle ns[PEN_LAYER_MAX + 1] = {};
    bool nv[PEN_LAYER_MAX + 1] = {};
    bool np[PEN_LAYER_MAX + 1] = {};
    for (int oldL = 1; oldL <= PEN_LAYER_MAX; ++oldL) {
        int newL = map_[oldL];
        ns[newL] = g_layer_styles[oldL];
        nv[newL] = g_layer_visible[oldL];
        np[newL] = g_layer_playback_hidden[oldL];
    }
    for (int L = 1; L <= PEN_LAYER_MAX; ++L) {
        g_layer_styles[L] = ns[L];
        g_layer_visible[L] = nv[L];
        g_layer_playback_hidden[L] = np[L];
    }
    op.after = strokes;
    capture_layer_state(op.styles_after, op.vis_after, op.pbh_after);
    redo_stack.clear();
    history_push(undo_stack, std::move(op));
    // 選択中レイヤーの内容が移動した先へ追従する
    g_cur_layer = map_[g_cur_layer];
    rebind_layer_sliders();
    sync_hsv_from_color();
    g_live_from = strokes.size();
    mark_canvas_dirty();
    redraw_frame();
}

// 右パネル行間のドラッグ挿入位置を求める (0=L1の上 .. 5=L5の下、-1=範囲外)
static int gap_from_pt(const POINT& pt) {
    if (pt.x < client_w - right_panel_px()) return -1;
    for (int i = 0; i < PEN_LAYER_MAX; ++i) {
        int mid = (g_layer_rows[i].top + g_layer_rows[i].bottom) / 2;
        if (pt.y < mid) return i;
    }
    return PEN_LAYER_MAX;
}

// ドラッグ中のオーバーレイ: ソース行の薄暗化 + 挿入インジケータ線 +
// カーソル追従フロートサムネイル (設計確定)。
// redraw_frame の最後 (最上位) から呼ぶ。
static void draw_drag_overlay(HDC dc) {
    if (g_drag_layer_src < 1 || !frame_window || !IsWindow(frame_window)) return;
    int rx = client_w - right_panel_px();

    // ソース行を薄暗く (移動中であることを示す)
    fill_rect_dc(dc, g_layer_rows[g_drag_layer_src - 1], COL_BG_RAIL);

    // 挿入インジケータ線 (青 3px)
    if (g_drop_gap >= 0) {
        int ly;
        if (g_drop_gap == 0) {
            ly = g_layer_rows[0].top - dpi_s(2);
        } else if (g_drop_gap >= PEN_LAYER_MAX) {
            ly = g_layer_rows[PEN_LAYER_MAX - 1].bottom + dpi_s(1);
        } else {
            const RECT& a = g_layer_rows[g_drop_gap - 1];
            ly = (a.bottom + g_layer_rows[g_drop_gap].top) / 2 - dpi_s(1);
        }
        fill_rect_dc(dc, { rx + dpi_s(8), ly, rx + right_panel_px() - dpi_s(8), ly + dpi_s(3) },
                     COL_SEL_ACCENT);
    }

    // フロートサムネイル (カーソル追従。全体図表示のサムネイルをそのまま使う)
    POINT cpt;
    if (GetCursorPos(&cpt) && ScreenToClient(frame_window, &cpt)) {
        int fw_ = dpi_s(96), fh_ = dpi_s(54);
        RECT flt = { cpt.x - fw_ / 2, cpt.y - fh_ / 2, cpt.x + fw_ / 2, cpt.y + fh_ / 2 };
        RECT client = { 0, 0, client_w, client_h };
        RECT inter;
        if (IntersectRect(&inter, &client, &flt)) {
            draw_layer_thumb(dc, flt, g_drag_layer_src);
            draw_accent_frame(dc, flt);
        }
    }
}

// 統合 : 直下 (番号大側) の最初の非空レイヤーの内容を現在レイヤーへ取り込む。
// 取り込まれた線は現在レイヤーの設定で描かれる (設計確定:
// 「L2 で統合 → L2+L3 が L2 に収まる」)。
static void merge_below_into_current() {
    int dst = g_cur_layer;
    int src = next_nonempty_layer(dst);
    if (src < 0) return;
    HistoryOp op;
    op.kind = HistoryOp::Kind::ReplaceStrokes;
    op.before = strokes;
    capture_layer_state(op.styles_before, op.vis_before, op.pbh_before);
    bool changed = false;
    for (auto& st : strokes) {
        if (st.empty() || st[0].layer != src) continue;
        for (auto& p : st) p.layer = dst;
        changed = true;
    }
    if (!changed) return;
    op.after = strokes;
    capture_layer_state(op.styles_after, op.vis_after, op.pbh_after);
    redo_stack.clear();
    history_push(undo_stack, std::move(op));
    g_live_from = strokes.size();
    mark_canvas_dirty();
    redraw_frame(); // 選択位置は不変
}

// クリア : 選択中レイヤーのストロークを全削除
static void clear_current_layer() {
    if (!layer_has_strokes(g_cur_layer)) return;
    HistoryOp op;
    op.kind = HistoryOp::Kind::ReplaceStrokes;
    op.before = strokes;
    capture_layer_state(op.styles_before, op.vis_before, op.pbh_before);
    strokes.erase(std::remove_if(strokes.begin(), strokes.end(),
                                 [&](const std::vector<PenPoint>& s) {
                                     return s.empty() || s[0].layer == g_cur_layer;
                                 }),
                  strokes.end());
    op.after = strokes;
    capture_layer_state(op.styles_after, op.vis_after, op.pbh_after);
    redo_stack.clear();
    history_push(undo_stack, std::move(op));
    g_live_from = strokes.size();
    mark_canvas_dirty();
    redraw_frame();
}

// レイヤー選択切替。スライダーと色 UI を現在レイヤーへ付け替える。
static void select_layer(int L) {
    if (L < 1 || L > PEN_LAYER_MAX || L == g_cur_layer) return;
    g_cur_layer = L;
    rebind_layer_sliders();
    sync_hsv_from_color(); // ホイール/SV 四角/RGB/HEX を新レイヤーの色へ同期
    redraw_frame();
}

// 設定コピー (要望): レイヤー src の設定を現在選択中レイヤーへ複写する。
// スタイル編集は Undo 対象外。確定時に差分として書き出される。
static void copy_layer_settings(int src) {
    if (src < 1 || src > PEN_LAYER_MAX || src == g_cur_layer) return;
    PenLayerStyle& dst = g_layer_styles[g_cur_layer];
    const PenLayerStyle& s = g_layer_styles[src];
    dst.col = s.col;
    dst.w = s.w;
    dst.ti = s.ti;
    dst.twi = s.twi;
    dst.to = s.to;
    dst.two_ = s.two_;
    rebind_layer_sliders();
    sync_hsv_from_color();
    update_color_box_texts();
    redraw_frame();
}

//-----------------------------------------------------------------------------
// 確定: ストロークをペンツール.obj2 へ反映
//-----------------------------------------------------------------------------

struct PenApplyCtx {
    std::string pts;              // 書込む最終座標列 "x,y,t,l;...|x,y,t,l;..."
    int pos_x, pos_y;             // 全ストローク中心のオブジェクト座標 (シーン中心原点・Y下向き)
    DWORD frame;                  // トラック読出用フレーム (新旧判別プローブに使用)
    OBJECT_HANDLE candidates[32]; // 適用対象候補
    int candidate_num;
    int applied;          // 更新できたペンツールエフェクト数
    int checked;          // ペンツールとして識別できたエフェクト数
    int selected;         // 選択中オブジェクト数 (診断用)
    int focus;            // フォーカスオブジェクトを使ったか (診断用)
    int effect_total;     // count_object_effect(ペンツール) の合計 (診断用)
    int pos_failed;       // 標準描画 X/Y の書込に失敗した数 (診断用)
    int style_failed;     // スタイル差分書込に失敗した数 (診断用)
    int fmtver_failed;    // fmtver 書込に失敗した数 (診断用)
    int lhide_failed;     // lhide 書込に失敗した数 (診断用)
    int legacy_merged;    // 旧構成のため L2 以降を L1 へ統合したオブジェクト数
    int crashed;          // コールバック内でアクセス違反を捕捉したか (SEH 保護)
    int crash_stage;      // クラッシュ発生ステージ (診断用)
};

// 適用対象の候補オブジェクトを収集する。
// 優先順位: ① 右クリックメニュー/パネルで保存した対象 (pen_trigger_object)
// ② 選択中オブジェクト ③ 無ければオブジェクト設定ウィンドウのフォーカスオブジェクト
static void collect_apply_candidates(PenApplyCtx* ctx, EDIT_SECTION* edit) {
    ctx->candidate_num = 0;
    if (pen_trigger_object) {
        ctx->candidates[ctx->candidate_num++] = pen_trigger_object;
        ctx->focus = 2; // 診断: トリガー由来
    }
    int n = edit->get_selected_object_num();
    ctx->selected = n;
    for (int i = 0; i < n && ctx->candidate_num < 32; ++i) {
        OBJECT_HANDLE obj = edit->get_selected_object(i);
        if (obj) ctx->candidates[ctx->candidate_num++] = obj;
    }
    if (ctx->candidate_num == 0) {
        OBJECT_HANDLE focus = edit->get_focus_object();
        if (focus) {
            ctx->candidates[ctx->candidate_num++] = focus;
            ctx->focus = 1;
        }
    }
}

// 適用ステップ0 (読み取り・Undo なし): 候補オブジェクトを収集する。
static void apply_scan_edit(void* param, EDIT_SECTION* edit) {
    PenApplyCtx* ctx = static_cast<PenApplyCtx*>(param);
    __try {
        ctx->crash_stage = 1;
        collect_apply_candidates(ctx, edit);
        ctx->crash_stage = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ctx->crashed = 1;
    }
}

// 適用ステップ2 (Undo エントリ): 線の座標 (ペンツール)、fmtver、標準設定 X/Y (標準描画)、
// レイヤー別スタイル差分を書く。位置の書込先は効果名「標準描画」・キー X/Y。
static void apply_stroke_edit(void* param, EDIT_SECTION* edit) {
    PenApplyCtx* ctx = static_cast<PenApplyCtx*>(param);
    char xbuf[32] = {}, ybuf[32] = {};
    snprintf(xbuf, sizeof(xbuf), "%.2f", static_cast<double>(ctx->pos_x));
    snprintf(ybuf, sizeof(ybuf), "%.2f", static_cast<double>(ctx->pos_y));

    __try {
        for (int ci = 0; ci < ctx->candidate_num; ++ci) {
            OBJECT_HANDLE obj = ctx->candidates[ci];
            ctx->crash_stage = 3;
            int count = edit->count_object_effect(obj, L"ペンツール");
            if (count > 0) ctx->effect_total += count;
            if (count <= 0) continue;
            ++ctx->checked;

            // 新旧判別 : 新構成のみが持つ L2 トラックの読出成否。
            // fmtver は未確定オブジェクトでも空文字列のため判別に使えない点に注意。
            double probe = 0.0;
            bool modern = edit->get_object_track_value(
                obj, L"ペンツール", L"ペンの太さ(L2)(px)", static_cast<int>(ctx->frame), &probe);

            ctx->crash_stage = 4;
            bool pts_ok = edit->set_object_item_value(obj, L"ペンツール", L"線の座標(シーンpx・ミリ秒)", ctx->pts.c_str());
            if (modern) {
                // : 形式判別用隠し項目へ "2" を書込
                if (!edit->set_object_item_value(obj, L"ペンツール", L"fmtver", "2"))
                    ++ctx->fmtver_failed;
                // 再生時非表示レイヤー (lhide) の書込。"01000" 形式。
                char lb[8];
                for (int i = 0; i < PEN_LAYER_MAX; ++i)
                    lb[i] = g_layer_playback_hidden[i + 1] ? '1' : '0';
                lb[PEN_LAYER_MAX] = '\0';
                if (!edit->set_object_item_value(obj, L"ペンツール", L"lhide", lb))
                    ++ctx->lhide_failed;
            } else {
                ++ctx->legacy_merged; // 座標は全点 l=1 で書かれるため統合済み扱い
            }
            bool pos_ok =
                edit->set_object_item_value(obj, L"標準描画", L"X", xbuf) &&
                edit->set_object_item_value(obj, L"標準描画", L"Y", ybuf);
            if (pts_ok) {
                ++ctx->applied;
                if (!pos_ok) ++ctx->pos_failed;
            }

            // スタイル差分書込 (レイヤー別): GUI で変更した色・太さ・入り抜きを保存する。
            // 開始時スナップショットとの差分のみ書く。旧構成は L1 のみ (ガード)。
            for (int L = 1; L <= PEN_LAYER_MAX; ++L) {
                if (!modern && L > 1) break;
                const StyleSnapshot& ini = g_style_init[L];
                if (!ini.valid) continue;
                const PenLayerStyle& cur = g_layer_styles[L];
                bool ok = true;
                wchar_t nb[48];
                if (cur.col != ini.col) {
                    char sbuf[8];
                    snprintf(sbuf, sizeof(sbuf), "%02x%02x%02x",
                             static_cast<unsigned>(GetRValue(cur.col)),
                             static_cast<unsigned>(GetGValue(cur.col)),
                             static_cast<unsigned>(GetBValue(cur.col)));
                    ok &= edit->set_object_item_value(obj, L"ペンツール",
                                                      layer_color_name(L, nb, 48), sbuf);
                }
                auto write_track_if_changed = [&](const wchar_t* base, double cur_v, double init_v) {
                    if (static_cast<long>(std::lround(cur_v)) ==
                        static_cast<long>(std::lround(init_v)))
                        return true;
                    char b2[32];
                    snprintf(b2, sizeof(b2), "%d", static_cast<int>(std::lround(cur_v)));
                    return edit->set_object_item_value(obj, L"ペンツール",
                                                       layer_track_name(L, base, nb, 48), b2);
                };
                if (static_cast<long>(std::lround(cur.w)) !=
                    static_cast<long>(std::lround(ini.w))) {
                    char b2[32];
                    snprintf(b2, sizeof(b2), "%d", static_cast<int>(std::lround(cur.w)));
                    ok &= edit->set_object_item_value(obj, L"ペンツール",
                                                      layer_width_name(L, nb, 48), b2);
                }
                ok &= write_track_if_changed(L"入り長さ", cur.ti, ini.ti);
                ok &= write_track_if_changed(L"入り太さ", cur.twi, ini.twi);
                ok &= write_track_if_changed(L"抜き長さ", cur.to, ini.to);
                ok &= write_track_if_changed(L"抜き太さ", cur.two_, ini.two_);
                if (!ok) ++ctx->style_failed;
            }
        }
        ctx->crash_stage = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ctx->crashed = 1;
    }
}

static void log_apply_failed(const wchar_t* why) {
    if (!logger) return;
    wchar_t m[256] = {};
    swprintf_s(m, L"[CJF PenTool] %s; stroke not applied", why);
    logger->log(logger, m);
}

// "x,y,t[,l];x,y,t[,l];...|..." を座標の配列に変換 (読込用)。
// 3/4 フィールド混在を許容。属性トークンはストローク先頭で解釈する:
// xi/xo : テーパー無効フラグ / d<ms> : 消失時刻 (ゴースト断片、 のため保持)
// それ以外の不正セグメントは黙ってスキップ (旧実装互換・クラッシュしない)。
static void parse_pts_string(const std::string& s, std::vector<std::vector<PenPoint>>& out) {
    out.clear();
    if (s.empty()) return;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t bar = s.find('|', pos);
        std::string st = (bar == std::string::npos) ? s.substr(pos) : s.substr(pos, bar - pos);
        std::vector<PenPoint> pts;
        bool tok_in = true, tok_out = true; // xi/xo 未指定 = テーパー有効
        DWORD tok_vanish = 0;
        DWORD tok_oshift = 0;
        size_t q = 0;
        while (q <= st.size()) {
            size_t semi = st.find(';', q);
            std::string seg = (semi == std::string::npos) ? st.substr(q) : st.substr(q, semi - q);
            double x = 0, y = 0;
            long t_signed = 0;
            int layer = 1;
            // 第4フィールドは省略可。sscanf は変換できた数を返す (3 or 4)。
            // t を符号付きで読むのは、手編集由来の "-5" 等が %lu で巨大値に
            // ラップされるのを防ぐため (レビュー L1)。負は無効扱い。
            int n = sscanf(seg.c_str(), "%lf,%lf,%ld,%d", &x, &y, &t_signed, &layer);
            if (n >= 3 && t_signed >= 0) {
                layer = clamp_int(layer, 1, PEN_LAYER_MAX);
                PenPoint pp;
                pp.sx = x;
                pp.sy = y;
                pp.t = static_cast<DWORD>(t_signed);
                pp.acc = 0.0; // acc は後続の累積長計算で埋める
                pp.layer = layer;
                pp.taper_in = tok_in;
                pp.taper_out = tok_out;
                pp.vanish = tok_vanish;
                pp.oshift = tok_oshift;
                pts.push_back(pp);
            } else if (seg == "xi") {
                tok_in = false;
            } else if (seg == "xo") {
                tok_out = false;
            } else if (!seg.empty() && seg[0] == 'd' &&
                       strspn(seg.c_str() + 1, "0123456789") == seg.size() - 1) {
                tok_vanish = static_cast<DWORD>(strtoul(seg.c_str() + 1, nullptr, 10));
            } else if (!seg.empty() && seg[0] == 'u' &&
                       strspn(seg.c_str() + 1, "0123456789") == seg.size() - 1) {
                tok_oshift = static_cast<DWORD>(strtoul(seg.c_str() + 1, nullptr, 10));
            }
            // その他の未知トークンは黙ってスキップ
            if (semi == std::string::npos) break;
            q = semi + 1;
        }
        if (!pts.empty()) out.push_back(std::move(pts));
        if (bar == std::string::npos) break;
        pos = bar + 1;
    }
}

// 全ストローク (既存読込分 + 新規分 + 消しゴム断片) を t 安定ソートして座標列にする
// (: 再生順完全保存)。第4フィールド (レイヤー) は常に書く
// (書き手規約: 全点に同じ l、冗長だが堅牢)。
// 断片にはストローク先頭に属性トークンを書く:
// d<ms> : ゴースト断片の消失時刻 (「引かれた後に消える」)
// xi/xo : 元ストローク始端/終端を含まない断片 (テーパー無効)
// u<ms> : 圧縮前の本来の先頭絶対時刻 (P4 タイムライン圧縮)。
// 圧縮は毎回「原本時刻 (t + shift)」(shift = oshift - front.t, oshift==u) から
// 現在の ▶ 状態で再計算し、結果の先頭 t が原本より早ければ u<原本> を書き出す。
// 次回読込時に oshift==u として復元されるため、後から再生対象へ戻した
// レイヤーも正しい時刻関係へ復帰する。
static bool pen_layer_playback_hidden(int L) {
    return L >= 1 && L <= PEN_LAYER_MAX && g_layer_playback_hidden[L];
}

static std::string build_pts_string() {
    std::vector<const std::vector<PenPoint>*> order;
    order.reserve(strokes.size());
    for (const auto& st : strokes)
        if (!st.empty()) order.push_back(&st);

    // タイムライン圧縮 : GAP_CAP 超過の空白と ▶ 再生対象外レイヤーの
    // 描画時間を切り詰める。strokes 本体は書き換えない (Undo スナップショットと
    // 描き足し offset の基準を守るため、コピーに対して適用する)
    size_t total_pts = 0;
    for (const auto* stp : order) total_pts += stp->size();
    std::vector<pentl::TimelinePoint> tp(total_pts);
    std::vector<pentl::TimelineStroke> entries;
    std::vector<double> orig_start(order.size()); // 圧縮前の原本先頭 t
    entries.reserve(order.size());
    {
        size_t w = 0;
        for (size_t i = 0; i < order.size(); ++i) {
            const auto& st = *order[i];
            const size_t base = w;
            // oshift は u<orig> (=原本先頭) をそのまま保持する。原本復元は
            // shift = orig_start - compressed_start を全点に加算する。
            // 旧実装の p.t + p.oshift は orig+compressed の二重加算で非冪等だった。
            double shift = 0.0;
            if (!st.empty() && st.front().oshift != 0) {
                shift = static_cast<double>(st.front().oshift) -
                        static_cast<double>(st.front().t);
            }
            for (const auto& p : st) {
                tp[w].t = static_cast<double>(p.t) + shift;
                tp[w].vanish =
                    p.vanish > 0 ? static_cast<double>(p.vanish) + shift : 0.0;
                ++w;
            }
            orig_start[i] = tp[base].t;
            entries.push_back(pentl::TimelineStroke{
                clamp_int(st.front().layer, 1, PEN_LAYER_MAX), i,
                tp.data() + base, st.size() });
        }
    }
    pentl::compress_timeline(entries, &pen_layer_playback_hidden,
                             static_cast<double>(g_gap_cap_ms));

    std::string pts;
    char buf[96];
    for (const auto& en : entries) {
        if (!pts.empty()) pts += "|";
        const auto& st = *order[en.src_index];
        // ストローク属性トークン (先頭点が代表)
        if (en.pts[0].vanish > 0.0) {
            snprintf(buf, sizeof(buf), "d%lu;",
                     static_cast<unsigned long>(en.pts[0].vanish + 0.5));
            pts += buf;
        }
        if (!st[0].taper_in) pts += "xi;";
        if (!st[0].taper_out) pts += "xo;";
        if (orig_start[en.src_index] - en.pts[0].t >= 0.5) {
            snprintf(buf, sizeof(buf), "u%lu;",
                     static_cast<unsigned long>(orig_start[en.src_index] + 0.5));
            pts += buf;
        }
        for (size_t i = 0; i < st.size(); ++i) {
            if (i) pts += ";";
            snprintf(buf, sizeof(buf), "%.1f,%.1f,%lu,%d",
                     st[i].sx, st[i].sy,
                     static_cast<unsigned long>(en.pts[i].t + 0.5),
                     clamp_int(st[i].layer, 1, PEN_LAYER_MAX));
            pts += buf;
        }
    }
    return pts;
}

// strokes (既存読込分 + 新規) 全体の中心をオブジェクト座標（シーン中心原点・Y下向き）で計算
// 動画出力対象外 (▶OFF) のレイヤーは中心計算から除外する (レビュー指摘 R3)。
static void compute_combined_center(int* pos_x, int* pos_y) {
    double min_x = 0, max_x = 0, min_y = 0, max_y = 0;
    bool first = true;
    auto feed = [&](const std::vector<PenPoint>& st) {
        if (st.empty() || st[0].vanish > 0)
            return; // ゴースト断片は最終静止画に出ないので位置基準に含めない
        if (g_layer_playback_hidden[clamp_int(st[0].layer, 1, PEN_LAYER_MAX)])
            return; // 再生に出ない線は位置基準に含めない
        for (const auto& p : st) {
            if (first) {
                min_x = max_x = p.sx;
                min_y = max_y = p.sy;
                first = false;
            } else {
                min_x = std::min(min_x, p.sx);
                max_x = std::max(max_x, p.sx);
                min_y = std::min(min_y, p.sy);
                max_y = std::max(max_y, p.sy);
            }
        }
    };
    for (const auto& st : strokes) feed(st);
    if (first) {
        *pos_x = 0;
        *pos_y = 0;
        return;
    }
    int px = static_cast<int>(std::lroundf(static_cast<float>((min_x + max_x) * 0.5 - frame_w * 0.5)));
    int py = static_cast<int>(std::lroundf(static_cast<float>((min_y + max_y) * 0.5 - frame_h * 0.5)));
    *pos_x = clamp_int(px, -8192, 8192);
    *pos_y = clamp_int(py, -8192, 8192);
}

static bool apply_strokes_to_pen_tool() {
    if (!edit_handle) return false;
    if (strokes.empty()) return false;

    PenApplyCtx ctx = {};

    if (logger) {
        size_t total_pts = 0;
        int per_layer[PEN_LAYER_MAX + 1] = {};
        for (const auto& st : strokes) {
            for (const auto& p : st) { (void)p; total_pts++; }
            if (!st.empty())
                per_layer[clamp_int(st[0].layer, 1, PEN_LAYER_MAX)]++;
        }
        wchar_t m[512] = {};
        swprintf_s(m, L"[CJF PenTool] confirm: %d stroke(s), %zu point(s) (L1=%d L2=%d L3=%d L4=%d L5=%d)",
                   static_cast<int>(strokes.size()), total_pts,
                   per_layer[1], per_layer[2], per_layer[3], per_layer[4], per_layer[5]);
        logger->log(logger, m);
    }

    // 読み取りスキャン（候補収集）
    if (!edit_handle->call_read_section_param(&ctx, apply_scan_edit)) {
        log_apply_failed(L"could not enter read section (playback/output in progress?)");
        return false;
    }
    if (ctx.crashed) {
        log_apply_failed(L"access violation in scan");
        return false;
    }

    // 座標列合成と中心計算（編集セクション外の純計算）
    ctx.pts = build_pts_string();
    compute_combined_center(&ctx.pos_x, &ctx.pos_y);
    EDIT_INFO info = {};
    edit_handle->get_edit_info(&info, sizeof(info));
    ctx.frame = info.frame;

    // 座標列 + fmtver + 標準描画 X/Y + スタイル差分の書込
    if (!edit_handle->call_edit_section_param(&ctx, apply_stroke_edit)) {
        log_apply_failed(L"could not enter edit section for stroke (playback/output in progress?)");
        return false;
    }

    if (logger) {
        wchar_t m[384] = {};
        if (ctx.crashed) {
            swprintf_s(m, L"[CJF PenTool] access violation caught in edit callback at stage %d (protected); stroke not applied", ctx.crash_stage);
        } else if (ctx.applied > 0) {
            if (ctx.pos_failed > 0 || ctx.style_failed > 0 || ctx.fmtver_failed > 0 ||
                ctx.lhide_failed > 0) {
                swprintf_s(m, L"[CJF PenTool] applied stroke to %d CJF ペンツール effect(s) (pts len=%zu, pos %d,%d), but write failed (style=%d pos=%d fmtver=%d lhide=%d)",
                           ctx.applied, ctx.pts.size(), ctx.pos_x, ctx.pos_y,
                           ctx.style_failed, ctx.pos_failed, ctx.fmtver_failed, ctx.lhide_failed);
            } else {
                swprintf_s(m, L"[CJF PenTool] applied stroke to %d CJF ペンツール effect(s) (pts len=%zu, pos %d,%d, legacy merged: %d)",
                           ctx.applied, ctx.pts.size(), ctx.pos_x, ctx.pos_y, ctx.legacy_merged);
            }
        } else if (ctx.checked == 0) {
            if (ctx.selected == 0 && ctx.focus == 0) {
                swprintf_s(m, L"[CJF PenTool] no object selected or focused; stroke not applied");
            } else {
                swprintf_s(m, L"[CJF PenTool] %d object(s) found, total ペンツール effect count=%d, but no CJF ペンツール object identified; stroke not applied",
                           ctx.selected + ctx.focus, ctx.effect_total);
            }
        } else {
            swprintf_s(m, L"[CJF PenTool] failed to apply stroke (set_object_item_value returned false)");
        }
        logger->log(logger, m);
    }
    return ctx.applied > 0;
}

static void retry_pen_confirm() {
    // 描画/消去ドラッグ中・モーダル中は適用しない (消去途中の strokes が
    // 確定されモードを強制終了させられるのを防ぐ)。リトライ自体は継続し、
    // ドラッグ終了後の発火で確定する
    if (mode != Mode::PenDraw || pen_down || erase_down || in_modal_dialog) return;
    if (apply_strokes_to_pen_tool()) {
        KillTimer(frame_window, CONFIRM_RETRY_TIMER_ID);
        confirm_retry_count = 0;
        pen_trigger_object = nullptr;
        hide_frame();
        return;
    }
    if (++confirm_retry_count >= CONFIRM_RETRY_MAX) {
        KillTimer(frame_window, CONFIRM_RETRY_TIMER_ID);
        confirm_retry_count = 0;
        if (logger) logger->warn(logger, L"[CJF PenTool] confirm retry limit reached; strokes kept for manual retry");
    }
}

static void confirm_stroke() {
    if (mode != Mode::PenDraw) return;
    // 描画中に確定された場合、現在位置でストロークを閉じてから適用する
    if (pen_down) {
        POINT cpt;
        GetCursorPos(&cpt);
        ScreenToClient(frame_window, &cpt);
        pen_down = false;
        g_pen_outside = false;
        ReleaseCapture();
        if (!strokes.empty()) append_end_point(cpt);
        g_gesture_dirty = false;
        g_gesture_before.clear();
    }
    if (strokes.empty()) {
        // 何も描かずに確定: 変更なしで閉じる。
        if (logger) logger->log(logger, L"[CJF PenTool] confirmed with no stroke (no change)");
        pen_trigger_object = nullptr;
        hide_frame();
        return;
    }
    if (!apply_strokes_to_pen_tool()) {
        // 1段目だけ成功する場合があるため、UIイベントを返してから自動再試行する。
        confirm_retry_count = 0;
        SetTimer(frame_window, CONFIRM_RETRY_TIMER_ID, CONFIRM_RETRY_INTERVAL_MS, nullptr);
        return;
    }
    pen_trigger_object = nullptr;
    hide_frame();
}

//-----------------------------------------------------------------------------
// パネル起動・対象解決 (現行ロジック踏襲)
//-----------------------------------------------------------------------------

// パネル起動時の対象解決。該当オブジェクトが無ければ、現在フレームから
// 3 秒分のペンツールを空きレイヤーへ作成し、そのハンドルを対象にする。
static bool prepare_panel_pen_target() {
    if (!edit_handle) return false;
    EDIT_INFO info = {};
    edit_handle->get_edit_info(&info, sizeof(info));
    // エイリアスファイルからオブジェクト定義を読む（.object が正規の定義）。
    std::string alias = load_alias_file(pen_alias_file_name);
    if (alias.empty()) {
        if (logger) logger->warn(logger,
                                 L"[CJF PenTool] alias file not found: Alias\\ペンツール@推奨.object");
        return false;
    }
    struct PanelCtx {
        EDIT_INFO info;
        OBJECT_HANDLE target;
        const char* alias; // エイリアスデータ (呼び出し側で保持)
    } ctx = { info, nullptr, alias.c_str() };
    if (!edit_handle->call_edit_section_param(&ctx, [](void* param, EDIT_SECTION* edit) {
        PanelCtx* p = static_cast<PanelCtx*>(param);
        auto is_target = [](EDIT_SECTION* ed, OBJECT_HANDLE obj) {
            return obj && ed->count_object_effect(obj, L"ペンツール") > 0;
        };
        int n = edit->get_selected_object_num();
        for (int i = 0; i < n && !p->target; ++i) {
            OBJECT_HANDLE obj = edit->get_selected_object(i);
            if (is_target(edit, obj)) p->target = obj;
        }
        if (!p->target) {
            OBJECT_HANDLE obj = edit->get_focus_object();
            if (is_target(edit, obj)) p->target = obj;
        }
        if (p->target) return;

        int fps = p->info.scale > 0 ? p->info.rate / p->info.scale : p->info.rate;
        int length = std::max(1, static_cast<int>(std::lround(3.0 * std::max(1, fps))));
        // 空きレイヤー探索。
        // layer_max は「オブジェクトが存在する最大のレイヤー番号」であり
        // 総レイヤー数ではないため、剰余による折り返しは使えない
        // （埋まっている範囲内を周回して空きレイヤーへ到達できない）。
        // 現在レイヤーから上方向へ、レイヤーが存在しなくなる（create が
        // 失敗する）まで探索し、見つからなければ下方向（0 まで）も試す。
        int first = std::max(0, p->info.layer);
        auto try_create = [&](int layer) {
            p->target = edit->create_object_from_alias(
                p->alias, layer, p->info.frame, length);
            if (p->target) {
                // 線の座標が真に空であることを保証（引用符などの混入を防ぐ）
                edit->set_object_item_value(
                    p->target, L"ペンツール", L"線の座標(シーンpx・ミリ秒)", "");
                edit->set_focus_object(p->target);
            }
        };
        for (int layer = first; !p->target && layer < first + 4096; ++layer) {
            if (edit->find_object(layer, p->info.frame)) continue; // 埋まっている
            try_create(layer);
            if (!p->target) break; // create 失敗 = レイヤーが存在しない → 上方向は終了
        }
        for (int layer = first - 1; layer >= 0 && !p->target; --layer) {
            if (edit->find_object(layer, p->info.frame)) continue;
            try_create(layer);
        }
        })) {
        return false;
    }
    if (!ctx.target) return false;
    pen_trigger_object = ctx.target;
    return true;
}

// パネル「クリア」: 選択中のペンツールを対象に線をクリアする。
// オブジェクトは生成しない（対象解決は選択中→フォーカスのみ）。
// 対象は右クリックメニューの「線をクリア」と同じく clear_request_object に
// 設定して遅延実行する。
static void handle_panel_clear_request() {
    if (!edit_handle) return;
    struct ClearCtx {
        OBJECT_HANDLE target;
    } ctx = { nullptr };
    if (!edit_handle->call_edit_section_param(&ctx, [](void* param, EDIT_SECTION* edit) {
        ClearCtx* c = static_cast<ClearCtx*>(param);
        auto is_target = [](EDIT_SECTION* ed, OBJECT_HANDLE obj) {
            return obj && ed->count_object_effect(obj, L"ペンツール") > 0;
        };
        int n = edit->get_selected_object_num();
        for (int i = 0; i < n && !c->target; ++i) {
            OBJECT_HANDLE obj = edit->get_selected_object(i);
            if (is_target(edit, obj)) c->target = obj;
        }
        if (!c->target) {
            OBJECT_HANDLE obj = edit->get_focus_object();
            if (is_target(edit, obj)) c->target = obj;
        }
        })) {
        return;
    }
    if (!ctx.target) {
        if (logger) logger->log(logger, L"[CJF PenTool] clear: no ペンツール object selected");
        return;
    }
    clear_request_object = ctx.target;
    PostMessageW(frame_window, WM_APP + 2, 0, 0);
}

//-----------------------------------------------------------------------------
// マウスホイール (WH_MOUSE_LL フック)
// WS_EX_NOACTIVATE のため WM_MOUSEWHEEL が自ウィンドウへ届かない。
// ペンモード中のみ低レベルフックを設置し、カーソルがスライダー上にあるときの
// ホイールを横取りして値を ±1 変動させる。それ以外は素通し。
//-----------------------------------------------------------------------------

static HHOOK g_mouse_hook = nullptr;
static int g_wheel_accum = 0;

static LRESULT CALLBACK wheel_mouse_hook_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0 && wparam == WM_MOUSEWHEEL && frame_window &&
        mode == Mode::PenDraw && !in_modal_dialog && !pen_down && !erase_down) {
        MSLLHOOKSTRUCT* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lparam);
        POINT pt = info->pt;
        if (ScreenToClient(frame_window, &pt)) {
            for (int i = 0; i < SLIDER_NUM; ++i) {
                if (point_in_rect(pt, g_sliders[i].rect)) {
                    short delta = static_cast<short>(HIWORD(info->mouseData));
                    // 本体側のホイール処理 (スクロール等) を起こさせないため握り潰す
                    PostMessageW(frame_window, WM_APP + 3,
                                 static_cast<WPARAM>(i), static_cast<LPARAM>(delta));
                    return 1;
                }
            }
            if (point_in_rect(pt, g_eraser_slider.rect)) {
                short delta = static_cast<short>(HIWORD(info->mouseData));
                PostMessageW(frame_window, WM_APP + 3,
                             static_cast<WPARAM>(SLIDER_NUM), static_cast<LPARAM>(delta));
                return 1;
            }
        }
    }
    return CallNextHookEx(g_mouse_hook, code, wparam, lparam);
}

static void install_wheel_hook() {
    if (!g_mouse_hook && module_instance) {
        g_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, wheel_mouse_hook_proc,
                                         module_instance, 0);
        g_wheel_accum = 0;
    }
}

static void remove_wheel_hook() {
    if (g_mouse_hook) {
        UnhookWindowsHookEx(g_mouse_hook);
        g_mouse_hook = nullptr;
    }
    g_wheel_accum = 0;
}

// スライダー上でのホイール/ボタンによる変動の共通処理。
// idx == SLIDER_NUM は消しゴムサイズ行。Shift 同時押しで ±5
// (太さのようにレンジが広い項目の中距離移動用。フィードバック)。
static void slider_step(int idx, double dir) {
    if (idx < 0 || idx > SLIDER_NUM) return;
    MiniSlider& s = *slider_at(idx);
    const double step = (GetKeyState(VK_SHIFT) & 0x8000) ? 5.0 : 1.0;
    double v = clampd(*s.value + dir * step, s.vmin, s.vmax);
    if (v != *s.value) {
        *s.value = v;
        redraw_frame();
    }
}

//-----------------------------------------------------------------------------
// カラーピッカー・ツール操作
//-----------------------------------------------------------------------------


static void handle_tool_click(int idx) {
    switch (idx) {
    case 0: // ペン
        g_active_tool = ToolKind::Pen;
        redraw_frame(); // リングカーソル消去
        break;
    case 1: // 消しゴム (P3)
        if (!g_eraser_size_ready) {
            // 初期サイズ = 現在レイヤーのペンの太さ ×3。セッション内で一度だけ
            g_eraser_size = std::max(4.0, g_layer_styles[g_cur_layer].w * 3.0);
            g_eraser_size_ready = true;
        }
        g_active_tool = ToolKind::Eraser;
        redraw_frame();
        break;
    case 2:
        apply_undo();
        break;
    case 3:
        apply_redo();
        break;
    }
}

// スライダーの x 座標から値を設定する
static void slider_set_from_x(MiniSlider& s, int x) {
    double span_w = static_cast<double>(s.track.right - s.track.left);
    double u = span_w > 0 ? (x - s.track.left) / span_w : 0.0;
    *s.value = std::lround(slider_u_to_value(s, u));
}

static void request_pen_mode() {
    if (logger) logger->log(logger, L"[CJF PenTool] pen mode requested (deferred to message loop)");
    if (frame_window) PostMessageW(frame_window, WM_APP + 1, 0, 0);
}

// レイヤー行の右クリックメニュー (設計確定:
// 個別コピーはこちらへ移設)。「この行の設定 → 現在のレイヤー」へ複写する。
static void show_layer_row_menu(int src_row, const POINT& client_pt) {
    if (!frame_window || !IsWindow(frame_window)) return;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    wchar_t txt[96];
    swprintf_s(txt, L"L%d の設定を現在のレイヤー(L%d)へコピー", src_row, g_cur_layer);
    BOOL ok = AppendMenuW(menu, MF_STRING | (src_row != g_cur_layer ? 0 : MF_GRAYED), 1, txt);
    if (!ok) { DestroyMenu(menu); return; }
    POINT sp = client_pt;
    ClientToScreen(frame_window, &sp);
    // NOACTIVATE ウィンドウでの TrackPopupMenu 定番対策 (フォアグラウンド切替 + WM_NULL)
    SetForegroundWindow(frame_window);
    int cmd = TrackPopupMenuEx(menu,
                               TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN,
                               sp.x, sp.y, frame_window, nullptr);
    PostMessageW(frame_window, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (cmd == 1) copy_layer_settings(src_row);
}

static void sync_hover(HitTarget h) {
    if (h.zone == HitZone::ToolBtn &&
        ((h.index == static_cast<int>(ToolKind::Undo) && undo_stack.empty()) ||
         (h.index == static_cast<int>(ToolKind::Redo) && redo_stack.empty())))
        h = {};
    for (int i = 0; i < SLIDER_NUM; ++i) {
        MiniSlider& s = g_sliders[i];
        s.hover = (h.zone == HitZone::Slider && h.index == i);
        s.hover_minus = (h.zone == HitZone::SliderMinus && h.index == i);
        s.hover_plus = (h.zone == HitZone::SliderPlus && h.index == i);
    }
    g_eraser_slider.hover = (h.zone == HitZone::Slider && h.index == SLIDER_NUM);
    g_eraser_slider.hover_minus = (h.zone == HitZone::SliderMinus && h.index == SLIDER_NUM);
    g_eraser_slider.hover_plus = (h.zone == HitZone::SliderPlus && h.index == SLIDER_NUM);
    if (h.zone != g_hover.zone || h.index != g_hover.index) {
        g_hover = h;
        redraw_frame();
    }
}

//-----------------------------------------------------------------------------
// ウィンドウプロシージャ (入力ルーティング: hit_test ベース)
//-----------------------------------------------------------------------------

static LRESULT CALLBACK frame_wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    static UINT panel_message = RegisterWindowMessageW(panel_message_name);
    static UINT panel_clear_message = RegisterWindowMessageW(panel_clear_message_name);
    if (message == panel_message) {
        if (prepare_panel_pen_target()) request_pen_mode();
        return 0;
    }
    if (message == panel_clear_message) {
        handle_panel_clear_request();
        return 0;
    }
    switch (message) {
    case WM_CREATE: {
        rebuild_fonts(GetDpiForWindow(hwnd));
        return 0;
    }
    case WM_TIMER: {
        if (wparam == ESC_POLL_TIMER_ID) {
            // モーダルダイアログ (色選択) 表示中・数値入力ボックス編集中は Esc を拾わない。
            // さらに前景/カーソルゲート (pen_input_context) により alt-tab 先での
            // Esc 押下がペンモード破棄に直結しないようにする
            HWND f = GetFocus();
            bool editing_box = false;
            for (auto e : g_rgb_edits) if (f == e) editing_box = true;
            if (f == g_hex_edit) editing_box = true;
            if (!in_modal_dialog && !editing_box && pen_esc_context() &&
                (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
                cancel_pen_mode();
            }
            // ツール切替ホットキー: P/B = ペン、E = 消しゴム (要望)。
            // WS_EX_NOACTIVATE でフォーカスを持たないため GetAsyncKeyState ポーリング。
            // Esc と同じく pen_input_context ゲートで alt-tab 先への影響を防ぎ、
            // 押しっぱなしでの連続切替をエッジ検出で防ぐ。
            static bool prev_p = false, prev_b = false, prev_e = false;
            if (mode == Mode::PenDraw && !in_modal_dialog && !editing_box &&
                !pen_down && !erase_down &&
                frame_window && IsWindow(frame_window) && pen_input_context()) {
                bool kp = (GetAsyncKeyState('P') & 0x8000) != 0;
                bool kb = (GetAsyncKeyState('B') & 0x8000) != 0;
                bool ke = (GetAsyncKeyState('E') & 0x8000) != 0;
                if ((kp || kb) && !prev_p && !prev_b) handle_tool_click(0);
                if (ke && !prev_e) handle_tool_click(1);
                prev_p = kp; prev_b = kb; prev_e = ke;
            } else {
                prev_p = prev_b = prev_e = false;
            }
        }
        if (wparam == CONFIRM_RETRY_TIMER_ID) {
            if (!in_modal_dialog) retry_pen_confirm();
        }
        if (wparam == HIDDEN_RENDER_TIMER_ID) {
            // 透過レンダリング: レイヤー非表示がシーンキャッシュに反映されたはず
            KillTimer(hwnd, HIDDEN_RENDER_TIMER_ID);
            if (!render_in_progress) return 0;
            EDIT_INFO info = {};
            if (edit_handle) edit_handle->get_edit_info(&info, sizeof(info));
            if (info.width < 1 || info.height < 1 ||
                !render_current_frame(info.frame, frame_rgba.data(), info.width, info.height)) {
                frame_rgba.clear();
                render_in_progress = false;
                restore_hidden_layer();
            }
            return 0;
        }
        if (wparam == RESIZE_DEBOUNCE_TIMER_ID) {
            KillTimer(hwnd, RESIZE_DEBOUNCE_TIMER_ID);
            build_canvas_image();
            redraw_frame();
        }
        return 0;
    }
    case WM_APP + 3: {
        // 確認ダイアログ等モーダル中は無視する
        if (in_modal_dialog) return 0;
        // スライダー上のホイール (WH_MOUSE_LL フックから転送)。1 ノッチ = ±1
        int idx = static_cast<int>(wparam);
        short delta = static_cast<short>(static_cast<long>(lparam));
        g_wheel_accum += delta;
        bool changed = false;
        while (g_wheel_accum >= WHEEL_DELTA) {
            slider_step(idx, +1.0);
            g_wheel_accum -= WHEEL_DELTA;
            changed = true;
        }
        while (g_wheel_accum <= -WHEEL_DELTA) {
            slider_step(idx, -1.0);
            g_wheel_accum += WHEEL_DELTA;
            changed = true;
        }
        (void)changed;
        return 0;
    }
    case WM_COMMAND: {
        // 数値入力ボックス: 編集中の列を追跡し、フォーカス喪失時に値を適用
        int id = LOWORD(wparam);
        int notif = HIWORD(wparam);
        bool is_rgb = (id >= IDC_EDIT_RGB0 && id <= IDC_EDIT_RGB0 + 2);
        if (is_rgb || id == IDC_EDIT_HEX) {
            if (notif == EN_CHANGE) {
                g_last_color_source = is_rgb ? 0 : 1;
            }
            if (notif == EN_KILLFOCUS) {
                apply_color_boxes();
            }
            return 0;
        }
        break;
    }
    case WM_CTLCOLOREDIT: {
        // 数値入力ボックスをダークテーマへ (白背景黒文字の解消)
        HDC hdc_edit = reinterpret_cast<HDC>(wparam);
        SetTextColor(hdc_edit, COL_TEXT_MAIN);
        SetBkColor(hdc_edit, RGB(28, 28, 32));
        return reinterpret_cast<LRESULT>(get_cached_brush(RGB(28, 28, 32)));
    }
    case WM_CJF_RENDER_COMPLETE: {
        RenderCtx* ctx = reinterpret_cast<RenderCtx*>(lparam);
        if (ctx) finish_pen_mode(ctx);
        return 0;
    }
    case WM_APP + 1: {
        begin_pen_mode();
        return 0;
    }
    case WM_APP + 2: {
        // 線をクリア（右クリックメニューから遅延実行）
        handle_clear_request();
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (g_dib_dc && g_dib && ps.rcPaint.right > ps.rcPaint.left &&
            ps.rcPaint.bottom > ps.rcPaint.top) {
            HGDIOBJ old_bmp = SelectObject(g_dib_dc, g_dib);
            BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top,
                   ps.rcPaint.right - ps.rcPaint.left,
                   ps.rcPaint.bottom - ps.rcPaint.top,
                   g_dib_dc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
            SelectObject(g_dib_dc, old_bmp);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE: {
        client_w = LOWORD(lparam);
        client_h = HIWORD(lparam);
        if (client_w < 1 || client_h < 1) break;
        do_layout();
        // canvas_img は重いので間引き再構築 (レイアウト/合成は即時)
        SetTimer(hwnd, RESIZE_DEBOUNCE_TIMER_ID, RESIZE_DEBOUNCE_MS, nullptr);
        redraw_frame();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lparam);
        mmi->ptMinTrackSize.x = 900;
        // プリセットグリッド + ホイールが収まる最低高
        mmi->ptMinTrackSize.y = 700;
        return 0;
    }
    case WM_DPICHANGED: {
        rebuild_fonts(HIWORD(wparam));
        // 子 EDIT へも新しいフォントを再適用
        for (auto e : g_rgb_edits) if (e) SendMessageW(e, WM_SETFONT, reinterpret_cast<WPARAM>(g_font_small), TRUE);
        if (g_hex_edit) SendMessageW(g_hex_edit, WM_SETFONT, reinterpret_cast<WPARAM>(g_font_small), TRUE);
        RECT* suggested = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        RECT crc = {};
        GetClientRect(hwnd, &crc);
        client_w = crc.right - crc.left;
        client_h = crc.bottom - crc.top;
        do_layout();
        build_canvas_image();
        redraw_frame();
        return 0;
    }
    case WM_SYSCOMMAND: {
        // 最小化は廃止 (アイコン化 + WM_NCCALCSIZE カスタム処理の組合せで
        // メッセージストームが発生するため)。タスクバー等からの要求も遮断。
        int cmd = static_cast<int>(wparam) & 0xFFF0;
        if (cmd == SC_MINIMIZE) return 0;
        break;
    }
    case WM_NCLBUTTONDOWN: {
        // タイトルバー等 NC 領域の操作でも前景を取り戻す。クライアント側の
        // WM_LBUTTONDOWN と同じ目的。既定処理は継続させる
        ensure_pen_foreground();
        break;
    }
    case WM_NCACTIVATE: {
        // 標準フレーム除去後のキャプション再描画 (白バー残存) を抑止する
        return DefWindowProcW(hwnd, message, wparam, -1);
    }
    case WM_NCCALCSIZE: {
        // 標準フレーム (タイトルバー/枠) を除去しクライアントを全領域へ拡張。
        // カスタムタイトルバーは WM_PAINT 側で描く。リサイズ枠は WM_NCHITTEST で自前処理。
        // ※ アイコン化 (最小化) 状態では既定処理に委ねること。カスタム戻値を
        // 返すとシステムとレイアウトが衝突しメッセージストームになる。
        if (!wparam || IsIconic(hwnd)) break;
        {
            RECT* rc = reinterpret_cast<RECT*>(lparam);
            if (IsZoomed(hwnd)) {
                // 最大化時はモニター外にはみ出す枠分を差し引く
                int bx = GetSystemMetrics(SM_CXSIZEFRAME) +
                         GetSystemMetrics(SM_CXPADDEDBORDER);
                int by = GetSystemMetrics(SM_CYSIZEFRAME) +
                         GetSystemMetrics(SM_CXPADDEDBORDER);
                rc->left += bx;
                rc->right -= bx;
                rc->top += by;
                rc->bottom -= by;
            }
            return 0;
        }
    }
    case WM_NCHITTEST: {
        // リサイズ帯 + タイトルドラッグ帯の自前ヒット判定
        if (IsIconic(hwnd)) break; // アイコン化中は既定処理
        POINT pt = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        ScreenToClient(hwnd, &pt);
        RECT crc;
        GetClientRect(hwnd, &crc);
        const int e = dpi_s(7);
        bool zoomed = IsZoomed(hwnd) != 0;
        if (!zoomed && !IsIconic(hwnd)) {
            bool lft = pt.x < e;
            bool rgt = pt.x >= crc.right - e;
            bool top = pt.y < e;
            bool btm = pt.y >= crc.bottom - e;
            if (top && lft)   return HTTOPLEFT;
            if (top && rgt)   return HTTOPRIGHT;
            if (btm && lft)   return HTBOTTOMLEFT;
            if (btm && rgt)   return HTBOTTOMRIGHT;
            if (lft)  return HTLEFT;
            if (rgt)  return HTRIGHT;
            if (top)  return HTTOP;
            if (btm)  return HTBOTTOM;
        }
        if (pt.y < title_px()) {
            for (int i = 0; i < 3; ++i)
                if (point_in_rect(pt, g_title_btn_rects[i]))
                    return HTCLIENT; // ボタンは自前クリック処理へ
            return HTCAPTION; // ドラッグ移動・ダブルクリック最大化は DefWindowProc
        }
        break;
    }
    case WM_SETCURSOR: {
        if (LOWORD(lparam) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            HitTarget h = hit_test(pt);
            SetCursor(h.zone == HitZone::Canvas ? LoadCursorW(nullptr, MAKEINTRESOURCEW(IDC_CROSS))
                                                : LoadCursorW(nullptr, MAKEINTRESOURCEW(IDC_ARROW)));
            return TRUE;
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        ensure_pen_foreground();
        POINT pt = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        if (in_modal_dialog) {
            // 確認オーバーレイ中: [はい] のみ確定。いいえ・外側クリックはキャンセル
            if (g_confirm_active) {
                HitTarget ch = hit_test(pt);
                if (ch.zone == HitZone::ConfirmYes) g_confirm_result = true;
                g_confirm_done = true;
                redraw_frame();
            }
            return 0;
        }
        if (mode != Mode::PenDraw || pen_down || erase_down) break;
        HitTarget h = hit_test(pt);
        // クリック到達時点のカーソル位置でホバーを再同期 (MOUSEMOVE 間引き対策)
        sync_hover(h);
        switch (h.zone) {
        case HitZone::Canvas:
            if (g_active_tool == ToolKind::Eraser)
                begin_erase(pt);
            else
                begin_stroke(pt);
            return 0;
        case HitZone::Slider:
            g_drag_slider = h.index;
            slider_at(h.index)->drag = true;
            SetCapture(hwnd);
            slider_set_from_x(*slider_at(h.index), pt.x);
            redraw_frame();
            return 0;
        case HitZone::SliderMinus:
            slider_step(h.index, -1.0);
            return 0;
        case HitZone::SliderPlus:
            slider_step(h.index, +1.0);
            return 0;
        case HitZone::WheelRing:
            update_hue_from_pt(pt);
            g_drag_wheel = true;
            SetCapture(hwnd);
            return 0;
        case HitZone::WheelSquare:
            update_sv_from_pt(pt);
            g_drag_square = true;
            SetCapture(hwnd);
            return 0;
        case HitZone::SepH1:
            g_drag_sep = 0; SetCapture(hwnd); return 0;
        case HitZone::SepH2:
            g_drag_sep = 1; SetCapture(hwnd); return 0;
        case HitZone::SepVL:
            g_drag_sep = 2; SetCapture(hwnd); return 0;
        case HitZone::SepVR:
            g_drag_sep = 3; SetCapture(hwnd); return 0;
        case HitZone::ToolBtn:
            handle_tool_click(h.index);
            return 0;
        case HitZone::LayerCheck:
            // 表示チェック。プレビューのみ非表示。データは保持される
            if (!pen_down) {
                g_layer_visible[h.index + 1] = !g_layer_visible[h.index + 1];
                redraw_frame();
            }
            return 0;
        case HitZone::LayerPlay:
            // 再生時非表示トグル (設計確定)。確定時に lhide へ書込。
            // プレビュー中は通常どおり見える (描けなくなるため)。
            if (!pen_down) {
                g_layer_playback_hidden[h.index + 1] = !g_layer_playback_hidden[h.index + 1];
                redraw_frame();
            }
            return 0;
        case HitZone::LayerRow:
            if (pen_down) return 0; // 描画中は切替・並べ替え無効
            select_layer(h.index + 1);
            // ドラッグで並べ替え開始の可能性を保持 (放した位置が別行なら入れ替え)
            g_drag_layer_src = h.index + 1;
            SetCapture(hwnd);
            return 0;
        case HitZone::MergeBtn:
            if (!pen_down) merge_below_into_current();
            return 0;
        case HitZone::ClearBtn:
            if (!pen_down) clear_current_layer();
            return 0;
        case HitZone::ApplyAllBtn:
            if (!pen_down) apply_settings_to_all_layers();
            return 0;
        case HitZone::PresetBtn:
            if (!pen_down) apply_style_preset(h.index + 1);
            return 0;
        case HitZone::PresetConfirmChk:
            // 確認ダイアログの抑制トグル (設定はアプリ起動中保持)
            g_preset_confirm_disable = !g_preset_confirm_disable;
            redraw_frame();
            return 0;
        case HitZone::TitleMax:
            ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
            return 0;
        case HitZone::TitleClose:
            cancel_pen_mode();
            return 0;
        case HitZone::Confirm:
            confirm_stroke();
            return 0;
        case HitZone::Cancel:
            cancel_pen_mode();
            return 0;
        default:
            break;
        }
        break;
    }
    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        // カラーホイールドラッグ中
        if (g_drag_wheel) { update_hue_from_pt(pt); return 0; }
        if (g_drag_square) { update_sv_from_pt(pt); return 0; }
        // セパレータドラッグ中 (領域境界・パネル幅を調整)
    if (g_drag_sep >= 0) {
        // 水平区切りはタイトルバー分を除いた y を保存値とする。
        // do_layout は dpi_s(保存値) + title_px で描画するため、ここで除いて
        // おかないと掴んだ瞬間に +タイトル分ジャンプし、掴むたびに保存値が
        // 膨張して再起動後に意図しない位置になっていた。
        int raw = (g_drag_sep <= 1) ? (pt.y - title_px()) : pt.x;
        int base = MulDiv(raw, 96,
                          static_cast<int>(std::max(96u, g_current_dpi)));
            if (g_drag_sep == 0) g_sep_tool_y = base;
            else if (g_drag_sep == 1) g_sep_wheel_y = base;
            else if (g_drag_sep == 2) g_left_panel_w = clamp_int(base, 150, 320);
            else g_right_panel_w = clamp_int(client_w - base, 150, 320);
            do_layout();
            // 重い再サンプルは debounce (タイマー経由で最終反映)
            SetTimer(hwnd, RESIZE_DEBOUNCE_TIMER_ID, RESIZE_DEBOUNCE_MS, nullptr);
            redraw_frame();
            return 0;
        }
        // スライダードラッグ中
        if (g_drag_slider >= 0 && g_drag_slider <= SLIDER_NUM) {
            slider_set_from_x(*slider_at(g_drag_slider), pt.x);
            redraw_frame();
            return 0;
        }
        // レイヤー行ドラッグ (挿入並べ替え): 現在の挿入位置 (gap) を追跡
        if (g_drag_layer_src >= 0) {
            int ng = gap_from_pt(pt);
            if (ng != g_drop_gap) {
                g_drop_gap = ng;
                redraw_frame();
            }
            return 0;
        }
        // リングカーソル用のカーソル位置追跡 (消しゴム時のみ再描画)
        {
            bool over_canvas =
                pt.x >= g_view.area.left && pt.x < g_view.area.right &&
                pt.y >= g_view.area.top && pt.y < g_view.area.bottom;
            bool moved = !g_cursor_valid ||
                         g_cursor_client.x != pt.x || g_cursor_client.y != pt.y;
            g_cursor_client = pt;
            g_cursor_valid = true;
            if (moved && mode == Mode::PenDraw &&
                g_active_tool == ToolKind::Eraser &&
                (over_canvas || erase_down)) {
                redraw_frame();
            }
        }
        // 描画中
        if (pen_down) {
            extend_stroke(pt);
            return 0;
        }
        // 消しゴム中
        if (erase_down) {
            extend_erase(pt);
            return 0;
        }
        // ホバー更新 (変化時のみ再描画)
        sync_hover(hit_test(pt));
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        // レイヤー行ドラッグの確定: 挿入位置へ差し込む (設定ごと移動、他はシフト)
        if (g_drag_layer_src >= 0) {
            int src = g_drag_layer_src;
            int gap = g_drop_gap;
            g_drag_layer_src = -1;
            g_drop_gap = -1;
            if (GetCapture() == hwnd) ReleaseCapture();
            if (!pen_down && gap >= 0) {
                apply_layer_permutation(src, gap);
            } else {
                redraw_frame();
            }
            return 0;
        }
        if (g_drag_slider >= 0 || g_drag_wheel || g_drag_square || g_drag_sep >= 0) {
            if (g_drag_slider >= 0 && g_drag_slider <= SLIDER_NUM)
                slider_at(g_drag_slider)->drag = false;
            g_drag_slider = -1;
            g_drag_wheel = false;
            g_drag_square = false;
            g_drag_sep = -1;
            if (GetCapture() == hwnd) ReleaseCapture();
            redraw_frame();
            return 0;
        }
        if (pen_down) {
            end_stroke(pt);
            return 0;
        }
        if (erase_down) {
            end_erase(pt);
            return 0;
        }
        break;
    }
    case WM_RBUTTONDOWN: {
        if (in_modal_dialog) break;
        if (mode != Mode::PenDraw) break;
        POINT pt = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        HitTarget h = hit_test(pt);
        if (h.zone == HitZone::Canvas) {
            if (!erase_down) confirm_stroke();
            return 0;
        }
        if (h.zone == HitZone::PresetBtn) {
            // プリセット保存 (右クリック)。確認オーバーレイを出す。描画中は拒否
            if (!pen_down) save_style_preset(h.index + 1);
            return 0;
        }
        if (h.zone == HitZone::LayerRow) {
            // レイヤー行の右クリックメニュー (個別コピー)
            if (!pen_down) show_layer_row_menu(h.index + 1, pt);
            return 0;
        }
        break;
    }
    case WM_HOTKEY: {
        // 確認ダイアログ等モーダル中の Undo/Redo は無視する
        if (in_modal_dialog) return 0;
        // Ctrl+Z / Ctrl+Y（RegisterHotKey で登録。フォーカスが裏にあっても届く）
        if (wparam == HOTKEY_UNDO_ID) {
            apply_undo();
            return 0;
        }
        if (wparam == HOTKEY_REDO_ID) {
            apply_redo();
            return 0;
        }
        break;
    }
    case WM_KEYDOWN: {
        if (wparam == VK_ESCAPE || wparam == VK_RETURN) {
            // 確認オーバーレイ中 (フレームにフォーカスがある場合のみ届く)
            if (g_confirm_active) {
                g_confirm_result = (wparam == VK_RETURN);
                g_confirm_done = true;
                redraw_frame();
                return 0;
            }
            if (wparam == VK_ESCAPE && !in_modal_dialog) {
                cancel_pen_mode();
                return 0;
            }
            return 0;
        }
        // RegisterHotKey 失敗時のフォールバック（フォーカスがフレームウィンドウにある場合）
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            if (wparam == 'Z') {
                apply_undo();
                return 0;
            }
            if (wparam == 'Y') {
                apply_redo();
                return 0;
            }
        }
        break;
    }
    case WM_CAPTURECHANGED: {
        // キャプチャが奪われたら（Alt+Tab 等）現在位置でストロークを閉じる。
        // 閉じないままだと終端未確定のストロークが strokes に残り、確定時に途中で切れた線になる。
        g_drag_slider = -1;
        g_drag_layer_src = -1;
        g_drop_gap = -1;
        for (auto& s : g_sliders) s.drag = false;
    g_eraser_slider.drag = false;
        if (pen_down && mode == Mode::PenDraw && !strokes.empty()) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            append_end_point(pt);
            pen_down = false;
            g_pen_outside = false;
            commit_gesture_history();
            g_gesture_before.clear();
            g_live_from = strokes.size();
            mark_canvas_dirty();
            redraw_frame();
        } else if (erase_down && mode == Mode::PenDraw) {
            // 消しゴムジェスチャも現在位置で閉じる。capture 被奪時に
            // erase_down が残留するとボタン無しのホバー移動で消え続け、
            // 次のクリックが 1 回捨てられる
            erase_down = false;
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            reflect_erase_final_sample(pt);
            flush_erase_cache_update();
            finish_erase_state();
        } else {
            // キャプチャが奪われたら描画を止める（モード自体は維持）
            pen_down = false;
            redraw_frame();
        }
        return 0;
    }
    case WM_CLOSE: {
        // ×ボタンは「キャンセル」として扱う (DestroyWindow はしない。プラグインは
        // ホストプロセス内で動作しており、PostQuitMessage 等は禁止)
        cancel_pen_mode();
        return 0;
    }
    }
    return DefWindowProc(hwnd, message, wparam, lparam);
}

//-----------------------------------------------------------------------------
// プラグインエントリポイント (aviutl2_sdk/plugin2.h の規約に準拠)
//-----------------------------------------------------------------------------

EXTERN_C __declspec(dllexport) DWORD RequiredVersion() {
    return 2003300;
}

EXTERN_C __declspec(dllexport) void InitializeLogger(LOG_HANDLE* handle) {
    // チェックリスト検証・パフォーマンス計測のため有効化。
    // 高頻度ログは [UI] PerfLog=1 のときのみ出す (perf_frame 参照)。
    logger = handle;
}
// アプリケーションデータフォルダのパス取得（エイリアスファイル読み込み用）。
// InitializePlugin より前に呼ばれる。
EXTERN_C __declspec(dllexport) void InitializeConfig(CONFIG_HANDLE* config) {
    config_handle = config;
}

EXTERN_C __declspec(dllexport) bool InitializePlugin(DWORD version) {
    return version >= RequiredVersion();
}

EXTERN_C __declspec(dllexport) void UninitializePlugin() {
    if (frame_window) {
        DestroyWindow(frame_window);
        frame_window = nullptr;
    }
    remove_wheel_hook();
    destroy_fonts();
    pen_trigger_object = nullptr;
    clear_request_object = nullptr;
    frame_rgba.clear();
    canvas_img.clear();
    strokes.clear();
    undo_stack.clear();
    redo_stack.clear();
    if (g_dib) { DeleteObject(g_dib); g_dib = nullptr; }
    if (g_dib_dc) { DeleteDC(g_dib_dc); g_dib_dc = nullptr; }
    g_dib_bits = nullptr;
    g_dib_w = g_dib_h = 0;
    // 異常系の安全網: ペンモード中にアンロードされた場合でもホスト入力を復帰させる
    if (host_window && IsWindow(host_window)) EnableWindow(host_window, TRUE);
    host_window = nullptr;
    edit_handle = nullptr;
    logger = nullptr;
    mode = Mode::Idle;
    pen_down = false;
}

EXTERN_C __declspec(dllexport) COMMON_PLUGIN_TABLE* GetCommonPluginTable(void) {
    return &common_plugin_table;
}

EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host) {
    if (!host) return;

    load_ui_settings();
    edit_handle = host->create_edit_handle();
    if (!edit_handle) return;

    host_window = edit_handle->get_host_app_window();
    if (!host_window) return;

    if (!module_instance) {
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&RegisterPlugin), &module_instance);
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpszClassName = frame_class_name;
    wc.lpfnWndProc = frame_wnd_proc;
    wc.hInstance = module_instance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(IDC_CROSS));
    wc.hbrBackground = nullptr;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;

    // 検証ウィンドウ (通常ウィンドウ + NOACTIVATE。TOOLWINDOW は付けない = D-1 確定事項)。
    // 非表示で生成し、ペンモード開始時に表示する。
    frame_window = CreateWindowExW(
        WS_EX_NOACTIVATE,
        frame_class_name, L"ペンモード",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, dpi_s(g_win_w), dpi_s(g_win_h),
        host_window, nullptr, module_instance, nullptr);
    if (!frame_window) return;

    // カスタムタイトルバー (WM_NCCALCSIZE での標準フレーム除去) を初期状態から
    // 確実に反映させる。これが無いと初回表示時に標準の白いキャプションが
    // 残り、リサイズするまで消えない。
    SetWindowPos(frame_window, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                 SWP_NOZORDER | SWP_NOACTIVATE);

    rebuild_fonts(GetDpiForWindow(frame_window));
    create_color_edits(frame_window);

    // 編集セクションを渡さないメニュー登録 (_param 版) + PostMessage 遅延
    host->register_edit_menu_param(L"CJF\\ペンで描く", nullptr, [](void*) {
        request_pen_mode();
    });

    // オブジェクト設定ウィンドウの右クリックメニューに登録。
    // コールバックに OBJECT_HANDLE が直接渡るため、対象オブジェクトを保存して
    // ペンモードの適用時に選択状態へ依存しない。
    // ※ SDK の登録 API に表示フィルタは無く、メニューは全オブジェクトに表示される。
    // 誤動作防止のため ペンツール 効果以外では何もしない。
    host->register_object_item_menu_param(
        L"ペンで描く", true, nullptr, [](void*, OBJECT_HANDLE object, LPCWSTR effect, LPCWSTR item) {
            if (!effect || wcscmp(effect, L"ペンツール") != 0) return;
            pen_trigger_object = object;
            if (logger) {
                wchar_t m[256] = {};
                swprintf_s(m, L"[CJF PenTool] context menu: object=%p effect=%s item=%s",
                    object, effect ? effect : L"(effect)", item ? item : L"(all)");
                logger->log(logger, m);
            }
            request_pen_mode();
        });

    // 「線をクリア」も右クリックメニューに登録（遅延実行で座標列を空にする）。
    host->register_object_item_menu_param(
        L"線をクリア", true, nullptr, [](void*, OBJECT_HANDLE object, LPCWSTR effect, LPCWSTR item) {
            if (!effect || wcscmp(effect, L"ペンツール") != 0) return;
            clear_request_object = object;
            if (frame_window) PostMessageW(frame_window, WM_APP + 2, 0, 0);
        });

    if (logger) {
        logger->log(logger,
            L"[CJF PenTool] initialized. Use 編集>CJF>ペンで描く / オブジェクト設定の右クリック / CJF パネル to start.");
    }
}
