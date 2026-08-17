// CJFPenToolButton.cpp
// AviUtl2 (AviUtl ExEdit2) filter plugin (.auf2)
// FILTER_ITEM_BUTTON（ボタン項目）によるペンツール操作フィルタ。
//
// フィルタ名: ペンツールボタン
//   - オブジェクトに「フィルタ」として追加すると、オブジェクト設定ウィンドウに
//     「ペンモード起動」「線をクリア」ボタンが表示される。
//   - ボタンを押すと、既存の CJF パネル「ペン」ボタンと同じメッセージ経路で
//     PenTool に依頼する（ポーリングなし・イベント駆動）。
//   - 描画処理は素通し（画像を変更しない）。

#define NOMINMAX
#include <windows.h>

#include <cstdint>

#include "filter2.h"
#include "logger2.h"
#include "plugin2.h"

// ------------------------------------------------------------------
// 設定項目の定義
// ------------------------------------------------------------------

// ボタンが押された時のコールバック
void on_pen_mode_button(EDIT_SECTION* edit);
void on_clear_button(EDIT_SECTION* edit);

FILTER_ITEM_BUTTON pen_mode_button(L"ペンモード起動", on_pen_mode_button);
FILTER_ITEM_BUTTON clear_button(L"線をクリア", on_clear_button);

void* items[] = {
    &pen_mode_button,
    &clear_button,
    nullptr,
};

// ------------------------------------------------------------------
// フィルタプラグインテーブル
// ------------------------------------------------------------------

bool func_proc_video(FILTER_PROC_VIDEO* video);

FILTER_PLUGIN_TABLE filter_plugin_table = {
    FILTER_PLUGIN_TABLE::FLAG_VIDEO,          // フラグ（画像フィルタとして登録）
    L"ペンツールボタン",                      // フィルタ名（フィルタ追加メニューに表示）
    L"CJF",                                   // ラベル
    L"CJF pen tool control filter (FILTER_ITEM_BUTTON)",
    items,                                    // 設定項目
    func_proc_video,                          // 画像フィルタ処理（素通し）
    nullptr,                                  // 音声フィルタ処理（なし）
};

// ------------------------------------------------------------------
// DLL エントリポイント
// ------------------------------------------------------------------

static LOG_HANDLE* logger = nullptr;

// ログ出力機能の初期化（任意）
EXTERN_C __declspec(dllexport) void InitializeLogger(LOG_HANDLE* handle) {
    logger = handle;
}

EXTERN_C __declspec(dllexport) bool InitializePlugin(DWORD version) {
    if (logger) logger->log(logger, L"[CJF PenToolButton] initialized");
    return true;
}

EXTERN_C __declspec(dllexport) void UninitializePlugin() {
    if (logger) logger->log(logger, L"[CJF PenToolButton] uninitialized");
}

EXTERN_C __declspec(dllexport) FILTER_PLUGIN_TABLE* GetFilterPluginTable(void) {
    return &filter_plugin_table;
}

// ------------------------------------------------------------------
// ボタンコールバック
// ------------------------------------------------------------------

// PenTool のフレームウィンドウクラス名・パネルメッセージ名は
// PreviewPenTool.cpp / PreviewRangeSelector.cpp と同じ文字列を使う。
static constexpr wchar_t pen_frame_class_name[] = L"CJFPreviewPenFrame";
static constexpr wchar_t panel_message_name[] = L"CJF.PreviewRangeSelector.Panel.Pen";
static constexpr wchar_t panel_clear_message_name[] = L"CJF.PreviewRangeSelector.Panel.PenClear";

// PenTool のフレームウィンドウへメッセージを送る共通処理
static void post_to_pen_frame(LPCWSTR message_name, LPCWSTR log_label) {
    HWND pen_window = FindWindowW(pen_frame_class_name, nullptr);
    if (!pen_window) {
        if (logger) logger->warn(logger, log_label);
        return;
    }
    UINT message_id = RegisterWindowMessageW(message_name);
    PostMessageW(pen_window, message_id, 0, 0);
}

void on_pen_mode_button(EDIT_SECTION* edit) {
    if (logger) logger->log(logger, L"[CJF PenToolButton] ペンモード起動 clicked");
    post_to_pen_frame(panel_message_name, L"[CJF PenToolButton] pen frame window not found");
}

void on_clear_button(EDIT_SECTION* edit) {
    if (logger) logger->log(logger, L"[CJF PenToolButton] 線をクリア clicked");
    post_to_pen_frame(panel_clear_message_name, L"[CJF PenToolButton] pen frame window not found");
}

// ------------------------------------------------------------------
// 画像フィルタ処理（素通し: 画像を変更しない）
// ------------------------------------------------------------------

bool func_proc_video(FILTER_PROC_VIDEO* video) {
    (void)video;
    return true;
}
