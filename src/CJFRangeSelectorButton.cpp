// CJFRangeSelectorButton.cpp
// AviUtl2 (AviUtl ExEdit2) filter plugin (.auf2)
// FILTER_ITEM_BUTTON（ボタン項目）による図形ツール操作フィルタ。
//
// フィルタ名: 図形ツールボタン
//   - オブジェクトに「フィルタ」として追加すると、オブジェクト設定ウィンドウに
//     「プレビューから範囲指定」ボタンが表示される。
//   - ボタンを押すと、既存の CJF パネル「矩形」ボタンと同じ処理を
//     RangeSelector へ依頼する（ポーリングなし・イベント駆動）。
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
void on_range_select_button(EDIT_SECTION* edit);

FILTER_ITEM_BUTTON range_select_button(L"プレビューから範囲指定", on_range_select_button);

void* items[] = {
    &range_select_button,
    nullptr,
};

// ------------------------------------------------------------------
// フィルタプラグインテーブル
// ------------------------------------------------------------------

bool func_proc_video(FILTER_PROC_VIDEO* video);

FILTER_PLUGIN_TABLE filter_plugin_table = {
    FILTER_PLUGIN_TABLE::FLAG_VIDEO,          // フラグ（画像フィルタとして登録）
    L"図形ツールボタン",                      // フィルタ名（フィルタ追加メニューに表示）
    L"CJF",                                   // ラベル
    L"CJF range selector control filter (FILTER_ITEM_BUTTON)",
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
    if (logger) logger->log(logger, L"[CJF RangeSelectorButton] initialized");
    return true;
}

EXTERN_C __declspec(dllexport) void UninitializePlugin() {
    if (logger) logger->log(logger, L"[CJF RangeSelectorButton] uninitialized");
}

EXTERN_C __declspec(dllexport) FILTER_PLUGIN_TABLE* GetFilterPluginTable(void) {
    return &filter_plugin_table;
}

// ------------------------------------------------------------------
// ボタンコールバック
// ------------------------------------------------------------------

// RangeSelector のフレームウィンドウクラス名・パネルメッセージ名は
// PreviewRangeSelector.cpp と同じ文字列を使う。
static constexpr wchar_t range_frame_class_name[] = L"CJFPreviewRangeFrame";
static constexpr wchar_t panel_range_message_name[] = L"CJF.PreviewRangeSelector.Panel.Range";

void on_range_select_button(EDIT_SECTION* edit) {
    if (logger) logger->log(logger, L"[CJF RangeSelectorButton] プレビューから範囲指定 clicked");
    HWND range_window = FindWindowW(range_frame_class_name, nullptr);
    if (!range_window) {
        if (logger) logger->warn(logger, L"[CJF RangeSelectorButton] range frame window not found");
        return;
    }
    UINT message_id = RegisterWindowMessageW(panel_range_message_name);
    PostMessageW(range_window, message_id, 0, 0);
}

// ------------------------------------------------------------------
// 画像フィルタ処理（素通し: 画像を変更しない）
// ------------------------------------------------------------------

bool func_proc_video(FILTER_PROC_VIDEO* video) {
    (void)video;
    return true;
}
