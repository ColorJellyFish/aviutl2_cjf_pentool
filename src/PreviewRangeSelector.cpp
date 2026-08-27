// CJFPreviewRangeSelector.cpp
// AviUtl2 (AviUtl ExEdit2) generic plugin (.aux2)
// Phase 2 (Option B): 現在フレームの出力画像を専用ウィンドウに表示し、
// その上でドラッグによる矩形範囲選択を行う。
//
// 設計:
//   1. EDIT_HANDLE::get_edit_info() で現在フレーム番号・シーン解像度を取得
//   2. EDIT_HANDLE::rendering_scene_video(frame, ...) で現在フレームの
//      出力画像バッファ (PIXEL_RGBA = r,g,b,a の順) を取得 (非同期)
//   3. 専用のレイヤードウィンドウにフレームをスケーリング表示
//   4. ウィンドウ上でドラッグ → 矩形確定 → シーン座標へ変換して保持
//
// プレビュー領域 (位置・倍率) に依存しないため、AviUtl2 のレイアウトや
// プレビュー倍率が変わっても座標変換は常に正確。

#define NOMINMAX // windows.h の min/max マクロで std::min/std::max が壊れるのを防ぐ
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <new>
#include <string>
#include <vector>

#include "config2.h"
#include "plugin2.h"
#include "logger2.h"

COMMON_PLUGIN_TABLE common_plugin_table = {
    L"CJF Preview Range Selector",
    L"AviUtl2 frame range selection (Phase 2 / Option B)",
};

static LOG_HANDLE* logger = nullptr;
static CONFIG_HANDLE* config_handle = nullptr; // InitializeConfig で取得 (Alias パス用)
static HWND host_window = nullptr;
static HINSTANCE module_instance = nullptr;
static EDIT_HANDLE* edit_handle = nullptr; // RegisterPlugin で取得、以後も有効 (SDK サンプル準拠)

static HWND frame_window = nullptr;
static const wchar_t frame_class_name[] = L"CJFPreviewRangeFrame";

// Phase 5 UI: オブジェクト設定ウィンドウの右クリックメニューから保存した対象オブジェクト。
// 範囲指定の適用時に選択状態に依存せず、このオブジェクトへ直接反映するために使う。
static OBJECT_HANDLE ctx_menu_object = nullptr;

static void request_range_select(); // 前方宣言 (RegisterPlugin のメニュー登録から呼ぶ)

// Phase 5 UI: ドッキング可能な専用パネル (register_window_client で登録)
static HWND panel_window = nullptr;
static const wchar_t panel_class_name[] = L"CJFPreviewRangePanel";
#define IDC_RANGE_BUTTON 2001
#define IDC_PEN_BUTTON 2002
#define IDC_CLEAR_BUTTON 2003
// パネル自動作成で読み込むエイリアスファイル (Alias\図形ツール@推奨.object)。
static const wchar_t range_alias_file_name[] = L"Alias\\図形ツール@推奨.object";

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
static constexpr wchar_t pen_frame_class_name[] = L"CJFPreviewPenFrame";
static constexpr wchar_t pen_panel_message_name[] = L"CJF.PreviewRangeSelector.Panel.Pen";
static constexpr wchar_t pen_panel_clear_message_name[] = L"CJF.PreviewRangeSelector.Panel.PenClear";
// ボタンフィルタ（CJFRangeSelectorButton.auf2）から範囲指定を依頼するメッセージ。
static constexpr wchar_t panel_range_message_name[] = L"CJF.PreviewRangeSelector.Panel.Range";

static constexpr COLORREF CJF_UI_BORDER = RGB(0x60, 0x60, 0x60); // パネルボタンの枠線
#define CONFIRM_RETRY_TIMER_ID 4
#define CONFIRM_RETRY_INTERVAL_MS 50
#define CONFIRM_RETRY_MAX 20
static int confirm_retry_count = 0;

// レンダリングで取得した現在フレーム (シーン解像度, RGBA, r が先頭)
static std::vector<unsigned char> frame_rgba;
static int frame_w = 0;
static int frame_h = 0;

// ウィンドウサイズにスケーリングした表示バッファ (BGRA 不透明, alpha=255)
static std::vector<unsigned char> display_bg;
static int client_w = 0;
static int client_h = 0;
// ウィンドウ 1px あたりのシーン px 数 (シーン→ウィンドウの縮尺の逆数)
static float scene_per_window = 1.0f;

enum class Mode { Idle, RangeSelect };
static Mode mode = Mode::Idle;

// レンダリング中の再入を防ぐ。
static bool render_in_progress = false;

static bool dragging = false;
static POINT drag_start = { 0, 0 };
static POINT drag_cur = { 0, 0 };

// 最後に確定した選択結果 (シーン座標 / シーン px)。Phase 4 で図形ツールへ渡す。
struct RangeSelectResult {
    int x1, y1, x2, y2;
    int width, height;
    int pos_x, pos_y; // 図形中心のオブジェクト座標 (シーン中心原点 px・Y下向き) ※後で算出
};
static RangeSelectResult last_result = { 0, 0, 0, 0, 0, 0, 0, 0 };

//-----------------------------------------------------------------------------
// フレームレンダリング (EDIT_HANDLE::rendering_scene_video)
//-----------------------------------------------------------------------------

struct RenderCtx {
    unsigned char* dst; // RGBA コピー先 (frame_rgba.data())
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
    bool accepted = edit_handle->rendering_scene_video(frame, ctx, on_rendered);
    if (accepted) {
        // wait_rendering_task は無制限待機のため、ハング時の切り分け用ログ
        if (logger) logger->log(logger, L"[CJF RangeSelector] render task submitted, waiting...");
        // 完了コールバックがUIスレッドへ通知するため、ここでは待たない。
        return true;
    }
    delete ctx;
    return false;
}

//-----------------------------------------------------------------------------
// 表示バッファ生成
//-----------------------------------------------------------------------------

static int clamp_int(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// frame_rgba (RGBA, シーン解像度) をウィンドウサイズへバイリニア補間で縮小し、
// display_bg (BGRA, alpha=255) を作る。ウィンドウ開いた時に 1 回だけ実行。
static void build_display_buffer() {
    display_bg.assign(static_cast<size_t>(client_w) * client_h * 4, 0);
    if (frame_w < 1 || frame_h < 1) return;

    for (int y = 0; y < client_h; ++y) {
        float sy = (static_cast<float>(y) + 0.5f) * scene_per_window - 0.5f;
        int y0 = clamp_int(static_cast<int>(std::floorf(sy)), 0, frame_h - 1);
        int y1 = clamp_int(y0 + 1, 0, frame_h - 1);
        float fy = std::max(0.0f, std::min(1.0f, sy - y0));
        for (int x = 0; x < client_w; ++x) {
            float sx = (static_cast<float>(x) + 0.5f) * scene_per_window - 0.5f;
            int x0 = clamp_int(static_cast<int>(std::floorf(sx)), 0, frame_w - 1);
            int x1 = clamp_int(x0 + 1, 0, frame_w - 1);
            float fx = std::max(0.0f, std::min(1.0f, sx - x0));

            const unsigned char* p00 = frame_rgba.data() + (static_cast<size_t>(y0) * frame_w + x0) * 4;
            const unsigned char* p10 = frame_rgba.data() + (static_cast<size_t>(y0) * frame_w + x1) * 4;
            const unsigned char* p01 = frame_rgba.data() + (static_cast<size_t>(y1) * frame_w + x0) * 4;
            const unsigned char* p11 = frame_rgba.data() + (static_cast<size_t>(y1) * frame_w + x1) * 4;

            // RGBA → BGRA 変換も兼ねる
            unsigned char* d = display_bg.data() + (static_cast<size_t>(y) * client_w + x) * 4;
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
// オーバーレイ描画 (ウィンドウ == 表示バッファ)
//-----------------------------------------------------------------------------

static RECT normalized_selection() {
    RECT r;
    r.left = drag_start.x < drag_cur.x ? drag_start.x : drag_cur.x;
    r.right = drag_start.x > drag_cur.x ? drag_start.x : drag_cur.x;
    r.top = drag_start.y < drag_cur.y ? drag_start.y : drag_cur.y;
    r.bottom = drag_start.y > drag_cur.y ? drag_start.y : drag_cur.y;
    return r;
}

static void redraw_frame() {
    if (!frame_window || !IsWindowVisible(frame_window)) return;
    if (client_w < 1 || client_h < 1) return;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = client_w;
    bmi.bmiHeader.biHeight = -client_h; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC memdc = CreateCompatibleDC(nullptr);
    HBITMAP dib = CreateDIBSection(memdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        DeleteDC(memdc);
        return;
    }
    HBITMAP old_bmp = reinterpret_cast<HBITMAP>(SelectObject(memdc, dib));

    // ベース: 表示バッファ (BGRA 不透明) をコピー
    memcpy(bits, display_bg.data(), static_cast<size_t>(client_w) * client_h * 4);

    RECT sel = normalized_selection();
    bool has_sel = dragging && sel.right > sel.left && sel.bottom > sel.top;

    // 選択外 (または未選択時は全面) を暗転 (BGRA プリマルチプライ値に係数を乗算)
    const int DIM_FACTOR = 150; // 255 の約 0.59
    unsigned char* px = static_cast<unsigned char*>(bits);
    if (!has_sel) {
        for (size_t i = 0; i < static_cast<size_t>(client_w) * client_h * 4; i += 4) {
            px[i]     = static_cast<unsigned char>(px[i] * DIM_FACTOR / 255);
            px[i + 1] = static_cast<unsigned char>(px[i + 1] * DIM_FACTOR / 255);
            px[i + 2] = static_cast<unsigned char>(px[i + 2] * DIM_FACTOR / 255);
        }
    } else {
        for (int y = 0; y < client_h; ++y) {
            for (int x = 0; x < client_w; ++x) {
                bool inside = x >= sel.left && x < sel.right && y >= sel.top && y < sel.bottom;
                if (inside) continue;
                unsigned char* p = px + (static_cast<size_t>(y) * client_w + x) * 4;
                p[0] = static_cast<unsigned char>(p[0] * DIM_FACTOR / 255);
                p[1] = static_cast<unsigned char>(p[1] * DIM_FACTOR / 255);
                p[2] = static_cast<unsigned char>(p[2] * DIM_FACTOR / 255);
            }
        }
    }

    if (has_sel) {
        // 選択枠 (オレンジのハイライト)
        const int BORDER = 2;
        for (int y = sel.top; y <= sel.bottom; ++y) {
            for (int x = sel.left; x <= sel.right; ++x) {
                if (x < 0 || y < 0 || x >= client_w || y >= client_h) continue;
                bool on_border = (x - sel.left < BORDER) || (sel.right - x < BORDER) ||
                                 (y - sel.top < BORDER) || (sel.bottom - y < BORDER);
                if (!on_border) continue;
                unsigned char* p = px + (static_cast<size_t>(y) * client_w + x) * 4;
                p[0] = 80;   // B
                p[1] = 160;  // G
                p[2] = 255;  // R
                p[3] = 255;
            }
        }

        // 左上にサイズ表示 (シーン px)
        int sw = static_cast<int>(std::lroundf((sel.right - sel.left) * scene_per_window));
        int sh = static_cast<int>(std::lroundf((sel.bottom - sel.top) * scene_per_window));
        wchar_t label[64] = {};
        swprintf_s(label, L"%d x %d", sw, sh);
        RECT lr = { sel.left + 8, sel.top + 8, sel.right - 8, sel.bottom - 8 };
        if (lr.right > lr.left && lr.bottom > lr.top) {
            HGDIOBJ old_font = SelectObject(memdc, GetStockObject(DEFAULT_GUI_FONT));
            SetBkMode(memdc, TRANSPARENT);
            SetTextColor(memdc, RGB(255, 255, 255));
            DrawTextW(memdc, label, -1, &lr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(memdc, old_font);
        }
    }

    RECT win_rc = {};
    GetWindowRect(frame_window, &win_rc);
    POINT pt_src = { 0, 0 };
    POINT pt_dst = { win_rc.left, win_rc.top };
    SIZE size = { client_w, client_h };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    // ULW 失敗はモード中 1 回だけログ
    static bool ulw_failed = false;
    bool ulw_ok = UpdateLayeredWindow(frame_window, nullptr, &pt_dst, &size, memdc, &pt_src, 0, &blend, ULW_ALPHA);
    if (!ulw_ok) {
        if (!ulw_failed && logger) {
            ulw_failed = true;
            wchar_t m[256] = {};
            swprintf_s(m, L"[CJF RangeSelector] UpdateLayeredWindow failed (error=%lu)", GetLastError());
            logger->warn(logger, m);
        }
    } else {
        ulw_failed = false;
    }

    SelectObject(memdc, old_bmp);
    DeleteObject(dib);
    DeleteDC(memdc);
}

//-----------------------------------------------------------------------------
// モード制御
//-----------------------------------------------------------------------------

static void hide_frame() {
    if (frame_window && IsWindow(frame_window)) {
        KillTimer(frame_window, 2);
        KillTimer(frame_window, CONFIRM_RETRY_TIMER_ID);
        ShowWindow(frame_window, SW_HIDE);
    }
    mode = Mode::Idle;
    dragging = false; // WM_CAPTURECHANGED での二重キャンセル防止 (先にクリア)
    if (GetCapture() == frame_window) ReleaseCapture();
    if (host_window && IsWindow(host_window)) SetFocus(host_window);
}

static void finish_range_select(RenderCtx* ctx) {
    DWORD render_ms = GetTickCount() - ctx->started_at;
    if (!ctx->copied) {
        frame_rgba.clear();
        render_in_progress = false;
        delete ctx;
        return;
    }
    frame_w = ctx->w;
    frame_h = ctx->h;
    const float max_w = 1280.0f, max_h = 800.0f;
    float window_per_scene = std::min(1.0f, max_w / static_cast<float>(frame_w));
    window_per_scene = std::min(window_per_scene, max_h / static_cast<float>(frame_h));
    scene_per_window = 1.0f / window_per_scene;
    client_w = std::max(1, static_cast<int>(std::lroundf(frame_w * window_per_scene)));
    client_h = std::max(1, static_cast<int>(std::lroundf(frame_h * window_per_scene)));
    build_display_buffer();
    RECT host_rc = {};
    GetWindowRect(host_window, &host_rc);
    int x = host_rc.left + (host_rc.right - host_rc.left - client_w) / 2;
    int y = host_rc.top + (host_rc.bottom - host_rc.top - client_h) / 2;
    SetWindowPos(frame_window, HWND_TOPMOST, x, y, client_w, client_h,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    mode = Mode::RangeSelect;
    dragging = false;
    redraw_frame();
    SetTimer(frame_window, 2, 33, nullptr);
    render_in_progress = false;
    if (logger) {
        wchar_t m[256] = {};
        swprintf_s(m, L"[CJF RangeSelector] frame rendered in %lu ms", render_ms);
        logger->log(logger, m);
    }
    delete ctx;
}

static void begin_range_select() {
    if (mode != Mode::Idle || render_in_progress || !frame_window || !edit_handle) return;
    render_in_progress = true;

    // 現在の編集状態 (フレーム番号・シーン解像度) を取得
    EDIT_INFO info = {};
    edit_handle->get_edit_info(&info, sizeof(info));
    if (info.width < 1 || info.height < 1) {
        render_in_progress = false;
        // チェック経由の起動失敗時のみチェックを OFF に戻す (他経路では書込まない)
        if (logger) logger->warn(logger, L"[CJF RangeSelector] no scene (invalid size), range select aborted");
        return;
    }

    // 現在フレームをレンダリング (wait_rendering_task は参照ロック・更新ロック中だと
    // デッドロックするため、この関数はメニューコールバックではなく
    // PostMessage で遅延されたメッセージループ側から呼ばれる)
    frame_rgba.assign(static_cast<size_t>(info.width) * info.height * 4, 0);
    if (logger) {
        wchar_t m[256] = {};
        swprintf_s(m, L"[CJF RangeSelector] rendering frame=%d (%dx%d)...", info.frame, info.width, info.height);
        logger->log(logger, m);
    }
    if (!render_current_frame(info.frame, frame_rgba.data(), info.width, info.height)) {
        frame_rgba.clear();
        render_in_progress = false;
        return;
    }
    // 完了後の表示処理はWM_CJF_RENDER_COMPLETEから続行する。
    return;
#if 0
    bool ok = render_current_frame(info.frame, frame_rgba.data(), info.width, info.height);
    DWORD render_ms = GetTickCount() - t0;
    if (!ok) {
        frame_rgba.clear();
        render_in_progress = false;
        // チェック経由の起動失敗時のみチェックを OFF に戻す (他経路では書込まない)
        if (logger) {
            wchar_t m[256] = {};
            swprintf_s(m, L"[CJF RangeSelector] rendering_scene_video failed (frame=%d, %lu ms). Output in progress?", info.frame, render_ms);
            logger->warn(logger, m);
        }
        return;
    }
    if (logger) {
        wchar_t m[256] = {};
        swprintf_s(m, L"[CJF RangeSelector] frame rendered in %lu ms", render_ms);
        logger->log(logger, m);
    }
    frame_w = info.width;
    frame_h = info.height;

    // ウィンドウサイズ: シーンを最大 1280x800 に収める (等倍以下は 1:1)
    const float max_w = 1280.0f, max_h = 800.0f;
    float window_per_scene = std::min(1.0f, max_w / static_cast<float>(frame_w));
    window_per_scene = std::min(window_per_scene, max_h / static_cast<float>(frame_h));
    // グローバルは「ウィンドウ 1px あたりのシーン px」= 縮尺の逆数
    scene_per_window = 1.0f / window_per_scene;
    client_w = std::max(1, static_cast<int>(std::lroundf(frame_w * window_per_scene)));
    client_h = std::max(1, static_cast<int>(std::lroundf(frame_h * window_per_scene)));
    build_display_buffer();

    // ホストウィンドウ中央に配置
    RECT host_rc = {};
    GetWindowRect(host_window, &host_rc);
    int x = host_rc.left + (host_rc.right - host_rc.left - client_w) / 2;
    int y = host_rc.top + (host_rc.bottom - host_rc.top - client_h) / 2;

    SetWindowPos(frame_window, HWND_TOPMOST, x, y, client_w, client_h,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);

    mode = Mode::RangeSelect;
    dragging = false;
    redraw_frame();
    SetTimer(frame_window, 2, 33, nullptr); // Esc ポーリング

    render_in_progress = false;

    if (logger) {
        wchar_t m[512] = {};
        swprintf_s(m,
            L"[CJF RangeSelector] frame window opened: scene=%dx%d frame=%d window=%dx%d scale=%.4f visible=%d (drag to select, Esc to cancel)",
            frame_w, frame_h, info.frame, client_w, client_h, scene_per_window,
            IsWindowVisible(frame_window) ? 1 : 0);
        logger->log(logger, m);
    }
#endif
}

static void cancel_range_select() {
    if (logger) logger->log(logger, L"[CJF RangeSelector] canceled (Esc)");
    hide_frame();
}

//-----------------------------------------------------------------------------
// Phase 4: 選択中の CJF 図形ツール.obj2 オブジェクトへ W/H を反映
//-----------------------------------------------------------------------------

struct ApplyResultCtx {
    RangeSelectResult result;
    OBJECT_HANDLE candidates[32]; // 適用対象候補 (ステップ0で収集し各ステップで使う)
    int candidate_num;            // 候補数
    int applied;  // 更新できた図形ツールエフェクト数
    int checked;  // 図形ツールとして識別できたエフェクト数
    int selected; // 選択中オブジェクト数 (診断用)
    int focus;    // フォーカスオブジェクトを使ったか (診断用)
    int effect_total; // count_object_effect(図形ツール) の合計 (診断用)
    int pos_failed;   // 標準描画 X/Y の書込に失敗した数 (診断用)
    int stale_script; // 旧スクリプト (X位置/Y位置 トラック) を検出した数 (診断用)
    int crashed;  // コールバック内でアクセス違反を捕捉したか (SEH 保護)
    int crash_stage; // クラッシュ発生ステージ (診断用)
    // 1:選択列挙 2:フォーカス 3:識別(count_object_effect) 4:書込 5:診断(get 幅(px))
    char cur_w[32]; // 書込前の w の現在値 (診断用)
    wchar_t items[512]; // enum_effect_item で取得した項目名一覧 (診断用)
    int items_used;     // items の現在の長さ
};

// enum_effect_item のコールバック (項目名を収集)
static void enum_item_cb(void* param, LPCWSTR name, int type) {
    ApplyResultCtx* ctx = static_cast<ApplyResultCtx*>(param);
    int room = static_cast<int>(sizeof(ctx->items) / sizeof(ctx->items[0])) - ctx->items_used;
    if (room <= 0) return;
    int w = swprintf_s(ctx->items + ctx->items_used, room, L"%s(%d), ", name ? name : L"(null)", type);
    if (w > 0) ctx->items_used += w;
}

// 適用対象の候補オブジェクトを収集する (読み取りスキャンで 1 回だけ実行)。
// 優先順位: ① 右クリックメニューで保存した対象 (ctx_menu_object、使い切り)
//          ② 選択中オブジェクト ③ 無ければオブジェクト設定ウィンドウのフォーカスオブジェクト
static void collect_apply_candidates(ApplyResultCtx* ctx, EDIT_SECTION* edit) {
    ctx->candidate_num = 0;
    if (ctx_menu_object) {
        ctx->candidates[ctx->candidate_num++] = ctx_menu_object;
        ctx->focus = 2; // 診断: 右クリックメニュー由来
        ctx_menu_object = nullptr; // 使い切り。以降は選択/フォーカスで解決
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
// 編集セクション (Undo) を開かずに済むため、メニュー/パネル/右クリック起動で
// 空の Undo エントリが積まれない。
static void apply_scan_edit(void* param, EDIT_SECTION* edit) {
    ApplyResultCtx* ctx = static_cast<ApplyResultCtx*>(param);
    __try {
        ctx->crash_stage = 1;
        collect_apply_candidates(ctx, edit);
        ctx->crash_stage = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ctx->crashed = 1;
    }
}

// 適用ステップ2 (Undo エントリ 2): 幅/高さ (図形ツール) と 標準設定 X/Y (標準描画) を書く。
// 候補はステップ0 で収集済みの ctx->candidates を使う。
// 位置の書込先は効果名「標準描画」・キー X/Y (get_object_alias と enum_effect_item の
// 実機確認で確定済み)。識別・書込はオブジェクトレベル API で行い、
// get_effect_list / get_effect_item_value (EFFECT_HANDLE 経由) は obj2 スクリプト効果で
// アクセス違反を起こすため使用しない (実機確認済み)。
static void apply_coords_edit(void* param, EDIT_SECTION* edit) {
    ApplyResultCtx* ctx = static_cast<ApplyResultCtx*>(param);

    // ctx->result は apply_result_to_shape_tools() 側でトラック範囲にクランプ済み
    char wbuf[32] = {}, hbuf[32] = {}, xbuf[32] = {}, ybuf[32] = {};
    snprintf(wbuf, sizeof(wbuf), "%d", ctx->result.width);
    snprintf(hbuf, sizeof(hbuf), "%d", ctx->result.height);
    snprintf(xbuf, sizeof(xbuf), "%d", ctx->result.pos_x);
    snprintf(ybuf, sizeof(ybuf), "%d", ctx->result.pos_y);

    __try {
        for (int ci = 0; ci < ctx->candidate_num; ++ci) {
            OBJECT_HANDLE obj = ctx->candidates[ci];

            // ステージ 3: 識別 (効果名「図形ツール」の存在で判定)
            // ※ obj2 スクリプトの項目名は @キーではなく表示名でアクセスするため、
            //    識別は count_object_effect (効果名の個数) で行う。
            ctx->crash_stage = 3;
            int count = edit->count_object_effect(obj, L"図形ツール");
            if (count > 0) ctx->effect_total += count;
            if (count <= 0) continue;
            ++ctx->checked;

            // 診断: 書込前の幅の現在値を取得 (表示名「幅(px)」でアクセス)
            ctx->crash_stage = 5;
            const char* cur_w = edit->get_object_item_value(obj, L"図形ツール", L"幅(px)");
            if (cur_w) {
                strncpy_s(ctx->cur_w, cur_w, _TRUNCATE);
            } else {
                ctx->cur_w[0] = '\0';
            }

            // ステージ 4: 書込
            //   図形ツール: 幅(px)/高さ(px) (整数)
            //   標準描画 (標準設定): X/Y (alias と同じ %.2f 形式)
            // ※ 標準設定の正体が効果名「標準描画」・キー X/Y であることは get_object_alias と
            //    enum_effect_item の実機確認で確定済み。
            ctx->crash_stage = 4;
            bool wh_ok =
                edit->set_object_item_value(obj, L"図形ツール", L"幅(px)", wbuf) &&
                edit->set_object_item_value(obj, L"図形ツール", L"高さ(px)", hbuf);
            char sx[32] = {}, sy[32] = {};
            snprintf(sx, sizeof(sx), "%.2f", static_cast<double>(ctx->result.pos_x));
            snprintf(sy, sizeof(sy), "%.2f", static_cast<double>(ctx->result.pos_y));
            bool pos_ok =
                edit->set_object_item_value(obj, L"標準描画", L"X", sx) &&
                edit->set_object_item_value(obj, L"標準描画", L"Y", sy);
            // 旧スクリプト検出: 旧 X位置/Y位置 トラックが残っているオブジェクトは
            // obj.ox/obj.oy で毎フレーム位置が上書きされるため、標準 X/Y が効かない。
            // オブジェクトを作り直せば解消する。
            if (edit->get_object_item_value(obj, L"図形ツール", L"X位置"))
                ++ctx->stale_script;
            if (wh_ok) {
                ++ctx->applied;
                if (!pos_ok) ++ctx->pos_failed;
            }
        }

        ctx->crash_stage = 0; // 正常完了
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ctx->crashed = 1;
    }
}

// 適用失敗のログ (診断用ヘルパー)
static void log_apply_section_failed(const wchar_t* why, int w, int h) {
    if (!logger) return;
    wchar_t m[256] = {};
    swprintf_s(m, L"[CJF RangeSelector] %s; size %dx%d not applied", why, w, h);
    logger->log(logger, m);
}

static void log_apply_crashed(const ApplyResultCtx& ctx, int w, int h) {
    if (!logger) return;
    wchar_t m[256] = {};
    swprintf_s(m,
        L"[CJF RangeSelector] access violation caught in edit callback at stage %d (protected); size %dx%d not applied",
        ctx.crash_stage, w, h);
    logger->log(logger, m);
}

static bool apply_result_to_shape_tools() {
    if (!edit_handle || last_result.width < 1 || last_result.height < 1) return false;

    // 図形ツール.obj2 のトラック範囲にクランプ
    const int kTrackMin = 1, kTrackMax = 16384; // 幅/高さ (--track@w:幅(px),1,16384)
    const int kPosMin = -8192, kPosMax = 8192;  // 標準設定 X/Y の範囲 (AviUtl 標準)
    RangeSelectResult clamped = last_result;
    if (clamped.width < kTrackMin) clamped.width = kTrackMin;
    if (clamped.width > kTrackMax) clamped.width = kTrackMax;
    if (clamped.height < kTrackMin) clamped.height = kTrackMin;
    if (clamped.height > kTrackMax) clamped.height = kTrackMax;
    // 選択範囲(シーン px・左上原点)の中心をオブジェクト座標(シーン中心原点・Y下向き)に変換
    // ※クランプ後の width/height を使う (実際に描画されるサイズの中心に合わせる)
    if (frame_w > 0 && frame_h > 0) {
        clamped.pos_x = clamped.x1 + clamped.width / 2 - frame_w / 2;
        clamped.pos_y = clamped.y1 + clamped.height / 2 - frame_h / 2;
    } else {
        clamped.pos_x = 0;
        clamped.pos_y = 0;
    }
    if (clamped.pos_x < kPosMin) clamped.pos_x = kPosMin;
    if (clamped.pos_x > kPosMax) clamped.pos_x = kPosMax;
    if (clamped.pos_y < kPosMin) clamped.pos_y = kPosMin;
    if (clamped.pos_y > kPosMax) clamped.pos_y = kPosMax;

    ApplyResultCtx ctx = {};
    ctx.result = clamped;

    // 診断: 効果「図形ツール」の設定項目名を列挙 (項目名は表示名であることを確認)
    if (logger) {
        edit_handle->enum_effect_item(L"図形ツール", &ctx, enum_item_cb);
        wchar_t m[384] = {};
        swprintf_s(m,
            L"[CJF RangeSelector] enum_effect_item(図形ツール) items: %s",
            ctx.items_used > 0 ? ctx.items : L"(none/effect not found)");
        logger->log(logger, m);
        ctx.items_used = 0;

    }

    // 適用は「読み取りスキャン → 編集セクション」の構成にする:
    //   ステップ0 (読み取り・Undo なし): 候補収集
    //   ステップ1 (編集・Undo エントリ 1): 幅/高さ + 標準設定 X/Y
    // 戻り値: true なら成功 / 編集できない場合(出力中など)は失敗しコールバックは呼ばれない
    if (!edit_handle->call_read_section_param(&ctx, apply_scan_edit)) {
        log_apply_section_failed(L"could not enter read section (playback/output in progress?)", clamped.width, clamped.height);
        return false;
    }
    if (ctx.crashed) {
        log_apply_crashed(ctx, clamped.width, clamped.height);
        return false;
    }
    if (!edit_handle->call_edit_section_param(&ctx, apply_coords_edit)) {
        log_apply_section_failed(L"could not enter edit section for coords (playback/output in progress?)", clamped.width, clamped.height);
        return false;
    }

    if (logger) {
        wchar_t m[256] = {};
        if (ctx.crashed) {
            swprintf_s(m,
                L"[CJF RangeSelector] access violation caught in edit callback at stage %d (protected); size %dx%d not applied",
                ctx.crash_stage, clamped.width, clamped.height);
        } else if (ctx.applied > 0) {
            if (ctx.pos_failed > 0) {
                swprintf_s(m,
                    L"[CJF RangeSelector] applied size %dx%d to %d CJF 図形ツール effect(s) (w was %hs), but 標準 X/Y write failed",
                    clamped.width, clamped.height, ctx.applied, ctx.cur_w);
            } else if (ctx.stale_script > 0) {
                swprintf_s(m,
                    L"[CJF RangeSelector] applied size %dx%d pos %d,%d to %d CJF 図形ツール effect(s) (w was %hs), but %d stale object(s) use the OLD script - delete and re-create them for 標準 X/Y to take effect",
                    clamped.width, clamped.height, clamped.pos_x, clamped.pos_y,
                    ctx.applied, ctx.cur_w, ctx.stale_script);
            } else {
                swprintf_s(m,
                    L"[CJF RangeSelector] applied size %dx%d pos %d,%d to %d CJF 図形ツール effect(s) (w was %hs, now updated; 標準 X/Y set)",
                    clamped.width, clamped.height, clamped.pos_x, clamped.pos_y, ctx.applied, ctx.cur_w);
            }
        } else if (ctx.checked == 0) {
            // 診断: どの段階で候補が空になったかを明確に
            if (ctx.selected == 0 && ctx.focus == 0) {
                swprintf_s(m,
                    L"[CJF RangeSelector] no object selected or focused; size %dx%d not applied",
                    clamped.width, clamped.height);
            } else {
                swprintf_s(m,
                    L"[CJF RangeSelector] %d object(s) found, total 図形ツール effect count=%d, but no CJF 図形ツール object identified; size %dx%d not applied",
                    ctx.selected + ctx.focus, ctx.effect_total, clamped.width, clamped.height);
            }
        } else {
            swprintf_s(m,
                L"[CJF RangeSelector] failed to apply size %dx%d (set_object_item_value returned false)",
                clamped.width, clamped.height);
        }
        logger->log(logger, m);
    }
    return ctx.applied > 0;
}

static void retry_range_confirm() {
    if (mode != Mode::RangeSelect || last_result.width < 1 || last_result.height < 1)
        return;
    if (apply_result_to_shape_tools()) {
        KillTimer(frame_window, CONFIRM_RETRY_TIMER_ID);
        confirm_retry_count = 0;
        hide_frame();
        return;
    }
    if (++confirm_retry_count >= CONFIRM_RETRY_MAX) {
        KillTimer(frame_window, CONFIRM_RETRY_TIMER_ID);
        confirm_retry_count = 0;
        if (logger) logger->warn(logger, L"[CJF RangeSelector] confirm retry limit reached; selection kept for manual retry");
    }
}

//-----------------------------------------------------------------------------
// ウィンドウプロシージャ (入力捕捉)
//-----------------------------------------------------------------------------

static void clamp_to_client(POINT* pt) {
    if (pt->x < 0) pt->x = 0;
    if (pt->y < 0) pt->y = 0;
    if (pt->x > client_w) pt->x = client_w;
    if (pt->y > client_h) pt->y = client_h;
}

// 範囲指定を起動する (メニュー/右クリック/パネルボタンから共通で呼ぶ)。
// 参照ロック中に wait_rendering_task がデッドロックするため、実際の処理は
// PostMessage で遅延してメッセージループ側で実行する。
static void request_range_select() {
    if (logger) logger->log(logger, L"[CJF RangeSelector] range select requested (deferred to message loop)");
    if (frame_window) PostMessageW(frame_window, WM_APP + 1, 0, 0);
}

// パネル起動時の対象解決。該当オブジェクトが無ければ、現在フレームから
// 3 秒分の図形ツールを空きレイヤーへ作成し、そのハンドルを適用対象にする。
static bool prepare_panel_range_target() {
    if (!edit_handle) return false;
    EDIT_INFO info = {};
    edit_handle->get_edit_info(&info, sizeof(info));
    // エイリアスファイルからオブジェクト定義を読む（.object が正規の定義）。
    std::string alias = load_alias_file(range_alias_file_name);
    if (alias.empty()) {
        if (logger) logger->warn(logger,
            L"[CJF RangeSelector] alias file not found: Alias\\図形ツール@推奨.object");
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
            return obj && ed->count_object_effect(obj, L"図形ツール") > 0;
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
            if (p->target) edit->set_focus_object(p->target);
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
    ctx_menu_object = ctx.target;
    return true;
}

// Phase 5 UI: ドッキングパネルのウィンドウプロシージャ。
// ペン／矩形の2つの起動ボタンを配置する。
// 配色は AviUtl2 のダークテーマ (#202020 背景 + 白文字、テーマ切替なし) に合わせて
// 固定で描画する。ボタンはオーナードロー (BS_OWNERDRAW) でホバー/押下の状態も描く。
#define PANEL_BG_COLOR   RGB(0x20, 0x20, 0x20) // パネル/ボタン背景
#define PANEL_BG_HOVER   RGB(0x2a, 0x2a, 0x2a) // ボタン ホバー
#define PANEL_BG_PRESSED RGB(0x16, 0x16, 0x16) // ボタン 押下中
#define PANEL_TEXT_COLOR RGB(0xff, 0xff, 0xff) // ボタン文字

static LRESULT CALLBACK panel_wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_COMMAND: {
        if (LOWORD(wparam) == IDC_RANGE_BUTTON) {
            if (prepare_panel_range_target()) request_range_select();
            SetFocus(nullptr);
            return 0;
        }
        if (LOWORD(wparam) == IDC_PEN_BUTTON) {
            HWND pen_window = FindWindowW(pen_frame_class_name, nullptr);
            if (pen_window) {
                UINT message_id = RegisterWindowMessageW(pen_panel_message_name);
                PostMessageW(pen_window, message_id, 0, 0);
            }
            SetFocus(nullptr); // ボタンのフォーカスを外す (サンプル準拠)
            return 0;
        }
        if (LOWORD(wparam) == IDC_CLEAR_BUTTON) {
            // パネル「クリア」: 選択中のペンツールの線をクリアする。
            // オブジェクト生成は行わない（対象解決は PenTool 側で選択中→フォーカスのみ）。
            HWND pen_window = FindWindowW(pen_frame_class_name, nullptr);
            if (pen_window) {
                UINT message_id = RegisterWindowMessageW(pen_panel_clear_message_name);
                PostMessageW(pen_window, message_id, 0, 0);
            }
            SetFocus(nullptr);
            return 0;
        }
        break;
    }
    case WM_SIZE: {
        // パネルのリサイズに合わせてボタンをフィットさせる (矩形/ペン/クリアの3分割)
        RECT rc = {};
        GetClientRect(hwnd, &rc);
        int width = static_cast<int>(std::max<LONG>(1, (rc.right - rc.left) / 3));
        HWND range_btn = GetDlgItem(hwnd, IDC_RANGE_BUTTON);
        HWND pen_btn = GetDlgItem(hwnd, IDC_PEN_BUTTON);
        HWND clear_btn = GetDlgItem(hwnd, IDC_CLEAR_BUTTON);
        if (range_btn) MoveWindow(range_btn, 0, 0, width, rc.bottom, TRUE);
        if (pen_btn) MoveWindow(pen_btn, width, 0, width, rc.bottom, TRUE);
        if (clear_btn) MoveWindow(clear_btn, width * 2, 0, rc.right - width * 2, rc.bottom, TRUE);
        return 0;
    }
    case WM_ERASEBKGND: {
        // パネル背景 (ボタンの隙間) をダークテーマ色で塗りつぶす
        RECT rc = {};
        GetClientRect(hwnd, &rc);
        HBRUSH br = CreateSolidBrush(PANEL_BG_COLOR);
        FillRect(reinterpret_cast<HDC>(wparam), &rc, br);
        DeleteObject(br);
        return 1;
    }
    case WM_DRAWITEM: {
        // オーナードローされたパネルボタンの描画
        LPDRAWITEMSTRUCT dis = reinterpret_cast<LPDRAWITEMSTRUCT>(lparam);
        if (dis->CtlType != ODT_BUTTON ||
            (dis->CtlID != IDC_RANGE_BUTTON && dis->CtlID != IDC_PEN_BUTTON && dis->CtlID != IDC_CLEAR_BUTTON)) break;
        HDC dc = dis->hDC;
        RECT rc = dis->rcItem;
        COLORREF bg = PANEL_BG_COLOR;
        if (dis->itemState & ODS_SELECTED) bg = PANEL_BG_PRESSED;
        else if (dis->itemState & ODS_HOTLIGHT) bg = PANEL_BG_HOVER;
        HBRUSH br = CreateSolidBrush(bg);
        FillRect(dc, &rc, br);
        DeleteObject(br);
        HBRUSH border = CreateSolidBrush(CJF_UI_BORDER);
        FrameRect(dc, &rc, border);
        DeleteObject(border);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, PANEL_TEXT_COLOR);
        HGDIOBJ old_font = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
        LPCWSTR label = L"";
        if (dis->CtlID == IDC_PEN_BUTTON) label = L"ペン";
        else if (dis->CtlID == IDC_RANGE_BUTTON) label = L"矩形";
        else if (dis->CtlID == IDC_CLEAR_BUTTON) label = L"クリア";
        DrawTextW(dc, label, -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old_font);
        if (dis->itemState & ODS_FOCUS) DrawFocusRect(dc, &rc);
        return TRUE;
    }
    }
    return DefWindowProc(hwnd, message, wparam, lparam);
}

static LRESULT CALLBACK frame_wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    // ボタンフィルタ（CJFRangeSelectorButton）からの範囲指定依頼。
    // パネル「矩形」ボタンと同じく、対象が無ければ自動作成してから開始する。
    static UINT panel_range_message = RegisterWindowMessageW(panel_range_message_name);
    if (message == panel_range_message) {
        if (prepare_panel_range_target()) request_range_select();
        return 0;
    }
    switch (message) {
    case WM_TIMER: {
        if (wparam == 2 && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
            cancel_range_select();
        }
        if (wparam == CONFIRM_RETRY_TIMER_ID) {
            retry_range_confirm();
        }
        return 0;
    }
    case WM_CJF_RENDER_COMPLETE: {
        RenderCtx* ctx = reinterpret_cast<RenderCtx*>(lparam);
        if (ctx) finish_range_select(ctx);
        return 0;
    }
    case WM_APP + 1: {
        // メニューコールバック (参照ロック中) から PostMessage で遅延された
        // 実際のレンダリング処理。ロックが解放されたメッセージループ側で実行される。
        begin_range_select();
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (mode == Mode::Idle) break;
        SetCapture(hwnd);
        dragging = true;
        drag_start.x = drag_cur.x = GET_X_LPARAM(lparam);
        drag_start.y = drag_cur.y = GET_Y_LPARAM(lparam);
        clamp_to_client(&drag_cur);
        drag_start = drag_cur;
        redraw_frame();
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!dragging) break;
        drag_cur.x = GET_X_LPARAM(lparam);
        drag_cur.y = GET_Y_LPARAM(lparam);
        clamp_to_client(&drag_cur);
        redraw_frame();
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!dragging) break;
        dragging = false;
        ReleaseCapture();

        RECT sel = normalized_selection();
        int ww = sel.right - sel.left;
        int wh = sel.bottom - sel.top;
        if (ww < 1 || wh < 1) {
            if (logger) logger->log(logger, L"[CJF RangeSelector] selection too small, ignored");
            hide_frame();
            return 0;
        }

        // ウィンドウ px → シーン px
        int sx1 = static_cast<int>(std::lroundf(sel.left * scene_per_window));
        int sy1 = static_cast<int>(std::lroundf(sel.top * scene_per_window));
        int sx2 = static_cast<int>(std::lroundf(sel.right * scene_per_window));
        int sy2 = static_cast<int>(std::lroundf(sel.bottom * scene_per_window));
        int sw = static_cast<int>(std::lroundf(ww * scene_per_window));
        int sh = static_cast<int>(std::lroundf(wh * scene_per_window));

        last_result = { sx1, sy1, sx2, sy2, sw, sh };
        if (logger) {
            wchar_t m[512] = {};
            swprintf_s(m,
                L"[CJF RangeSelector] RESULT scene=(%d,%d)-(%d,%d) size=%dx%d window=%dx%d (scene px)",
                sx1, sy1, sx2, sy2, sw, sh, ww, wh);
            logger->log(logger, m);
        }

        // Phase 4: 選択中の CJF 図形ツールオブジェクトへ W/H を反映
        if (!apply_result_to_shape_tools()) {
            // 最初の編集セクションだけ成功した場合があるため、UIイベントを返してから再試行する。
            confirm_retry_count = 0;
            SetTimer(frame_window, CONFIRM_RETRY_TIMER_ID, CONFIRM_RETRY_INTERVAL_MS, nullptr);
            return 0;
        }
        hide_frame();
        return 0;
    }
    case WM_KEYDOWN: {
        if (wparam == VK_ESCAPE) {
            cancel_range_select();
            return 0;
        }
        break;
    }
    case WM_CAPTURECHANGED: {
        if (dragging) {
            dragging = false;
            cancel_range_select();
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
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
    // ログ出力は無効化（AviUtl2 のログウィンドウに表示しない）。
    // デバッグ時に復活させる場合は logger = handle; を有効にする。
    // logger = handle;
}

// アプリケーションデータフォルダのパス取得（エイリアスファイル読み込み用）。
// InitializePlugin() より前に呼ばれる。
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
    if (panel_window) {
        DestroyWindow(panel_window);
        panel_window = nullptr;
    }
    ctx_menu_object = nullptr;
    frame_rgba.clear();
    display_bg.clear();
    host_window = nullptr;
    edit_handle = nullptr;
    logger = nullptr;
    mode = Mode::Idle;
    dragging = false;
}

EXTERN_C __declspec(dllexport) COMMON_PLUGIN_TABLE* GetCommonPluginTable(void) {
    return &common_plugin_table;
}

EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host) {
    if (!host) return;

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
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;

    // 専用ウィンドウ (レイヤード・オーナー付き・アクティブ化しない)
    frame_window = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        frame_class_name, L"CJF Preview Range", WS_POPUP,
        0, 0, 1, 1,
        host_window, nullptr, module_instance, nullptr);
    if (!frame_window) return;

    // 編集セクションを渡さないメニュー登録 (_param 版): 参照ロック状態で呼ばれないようにする。
    // さらに処理は PostMessage で遅延し、wait_rendering_task のデッドロックを確実に回避する。
    host->register_edit_menu_param(L"CJF\\プレビューから範囲指定", nullptr, [](void*) {
        request_range_select();
    });

    // Phase 5 UI (A): オブジェクト設定ウィンドウの右クリックメニューに登録。
    // コールバックに OBJECT_HANDLE が直接渡るため、対象オブジェクトを保存して
    // 範囲指定の適用時に選択状態へ依存しない。
    // ※ SDK の登録 API に表示フィルタは無く、メニューは全オブジェクトに表示される。
    //    誤動作防止のため 図形ツール 効果以外では何もしない。
    host->register_object_item_menu_param(
        L"プレビューから範囲指定", true, nullptr, [](void*, OBJECT_HANDLE object, LPCWSTR effect, LPCWSTR item) {
            if (!effect || wcscmp(effect, L"図形ツール") != 0) return;
            ctx_menu_object = object;
            if (logger) {
                wchar_t m[256] = {};
                swprintf_s(m, L"[CJF RangeSelector] context menu: object=%p effect=%s item=%s",
                    object, effect ? effect : L"(effect)", item ? item : L"(all)");
                logger->log(logger, m);
            }
            request_range_select();
        });

    // Phase 5 UI (B): ドッキング可能な専用パネル (SDK サンプル WindowClient.cpp 準拠)。
    WNDCLASSEXW pwc = {};
    pwc.cbSize = sizeof(pwc);
    pwc.lpszClassName = panel_class_name;
    pwc.lpfnWndProc = panel_wnd_proc;
    pwc.hInstance = module_instance;
    pwc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    if (RegisterClassExW(&pwc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        panel_window = CreateWindowExW(
            0, panel_class_name, L"CJF ツール", WS_POPUP,
            CW_USEDEFAULT, CW_USEDEFAULT, 330, 40,
            nullptr, nullptr, module_instance, nullptr);
        if (panel_window) {
            // パネルいっぱいのボタン
            CreateWindowExW(
            0, L"BUTTON", L"図形",
                WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
                0, 0, 110, 40,
                panel_window, reinterpret_cast<HMENU>(IDC_RANGE_BUTTON), module_instance, nullptr);
            CreateWindowExW(
                0, L"BUTTON", L"ペン",
                WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
                110, 0, 110, 40,
                panel_window, reinterpret_cast<HMENU>(IDC_PEN_BUTTON), module_instance, nullptr);
            CreateWindowExW(
                0, L"BUTTON", L"クリア",
                WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
                220, 0, 110, 40,
                panel_window, reinterpret_cast<HMENU>(IDC_CLEAR_BUTTON), module_instance, nullptr);
            host->register_window_client(L"CJF ツール", panel_window);
        }
    }

    if (logger) {
        logger->log(logger,
            L"[CJF RangeSelector] initialized. Use the CJF panel buttons or 編集>CJF>プレビューから範囲指定 to start.");
    }
}
