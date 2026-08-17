cmake_minimum_required(VERSION 3.24)

project(Aviutl2_CJF_PenTool LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "AviUtl2 plugins are Windows-only.")
endif()

option(CJF_BUILD_PREVIEW_RANGE_SELECTOR "Build the Phase 2 preview range selector plugin" ON)
option(CJF_BUILD_PREVIEW_PEN_TOOL "Build the Phase 6 pen tool plugin" ON)
option(CJF_BUILD_PEN_TOOL_BUTTON "Build the pen tool button filter plugin (FILTER_ITEM_BUTTON)" ON)
option(CJF_BUILD_RANGE_SELECTOR_BUTTON "Build the range selector button filter plugin (FILTER_ITEM_BUTTON)" ON)

if(CJF_BUILD_PREVIEW_RANGE_SELECTOR)
    add_library(CJFPreviewRangeSelector SHARED
        src/PreviewRangeSelector.cpp
    )

    target_include_directories(CJFPreviewRangeSelector PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/aviutl2_sdk
    )

    target_compile_features(CJFPreviewRangeSelector PRIVATE cxx_std_17)

    # SDK/plugin sources contain UTF-8 Japanese strings.
    if(MSVC)
        target_compile_options(CJFPreviewRangeSelector PRIVATE /utf-8)
        # The SDK headers themselves are Shift-JIS; suppress the resulting
        # C4828 (charset) warnings so the build log stays clean.
        target_compile_options(CJFPreviewRangeSelector PRIVATE /wd4828)
    endif()

    target_link_libraries(CJFPreviewRangeSelector PRIVATE user32 gdi32)

    set_target_properties(CJFPreviewRangeSelector PROPERTIES
        PREFIX ""
        OUTPUT_NAME "CJFPreviewRangeSelector"
        SUFFIX ".aux2"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/aviutl2/Plugin/CJF"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/aviutl2/Plugin/CJF"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    )
endif()

if(CJF_BUILD_PREVIEW_PEN_TOOL)
    add_library(CJFPreviewPenTool SHARED
        src/PreviewPenTool.cpp
    )

    target_include_directories(CJFPreviewPenTool PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/aviutl2_sdk
    )

    target_compile_features(CJFPreviewPenTool PRIVATE cxx_std_17)

    # SDK/plugin sources contain UTF-8 Japanese strings.
    if(MSVC)
        target_compile_options(CJFPreviewPenTool PRIVATE /utf-8)
        # The SDK headers themselves are Shift-JIS; suppress the resulting
        # C4828 (charset) warnings so the build log stays clean.
        target_compile_options(CJFPreviewPenTool PRIVATE /wd4828)
    endif()

    target_link_libraries(CJFPreviewPenTool PRIVATE user32 gdi32)

    set_target_properties(CJFPreviewPenTool PROPERTIES
        PREFIX ""
        OUTPUT_NAME "CJFPreviewPenTool"
        SUFFIX ".aux2"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/aviutl2/Plugin/CJF"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/aviutl2/Plugin/CJF"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    )
endif()

# FILTER_ITEM_BUTTON（ボタン項目）のフィルタプラグイン群。
# オブジェクト設定ウィンドウにボタンを表示し、押下イベントで既存の
# PenTool / RangeSelector へメッセージを送る（ポーリングなし・イベント駆動）。
if(CJF_BUILD_PEN_TOOL_BUTTON)
    add_library(CJFPenToolButton SHARED
        src/CJFPenToolButton.cpp
    )

    target_include_directories(CJFPenToolButton PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/aviutl2_sdk
    )

    target_compile_features(CJFPenToolButton PRIVATE cxx_std_17)

    # SDK/plugin sources contain UTF-8 Japanese strings.
    if(MSVC)
        target_compile_options(CJFPenToolButton PRIVATE /utf-8)
        # The SDK headers themselves are Shift-JIS; suppress the resulting
        # C4828 (charset) warnings so the build log stays clean.
        target_compile_options(CJFPenToolButton PRIVATE /wd4828)
    endif()

    target_link_libraries(CJFPenToolButton PRIVATE user32)

    # AviUtl2 loads filter plugins with the .auf2 extension.
    set_target_properties(CJFPenToolButton PROPERTIES
        PREFIX ""
        OUTPUT_NAME "CJFPenToolButton"
        SUFFIX ".auf2"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/aviutl2/Plugin/CJF"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/aviutl2/Plugin/CJF"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    )
endif()

if(CJF_BUILD_RANGE_SELECTOR_BUTTON)
    add_library(CJFRangeSelectorButton SHARED
        src/CJFRangeSelectorButton.cpp
    )

    target_include_directories(CJFRangeSelectorButton PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/aviutl2_sdk
    )

    target_compile_features(CJFRangeSelectorButton PRIVATE cxx_std_17)

    # SDK/plugin sources contain UTF-8 Japanese strings.
    if(MSVC)
        target_compile_options(CJFRangeSelectorButton PRIVATE /utf-8)
        # The SDK headers themselves are Shift-JIS; suppress the resulting
        # C4828 (charset) warnings so the build log stays clean.
        target_compile_options(CJFRangeSelectorButton PRIVATE /wd4828)
    endif()

    target_link_libraries(CJFRangeSelectorButton PRIVATE user32)

    # AviUtl2 loads filter plugins with the .auf2 extension.
    set_target_properties(CJFRangeSelectorButton PROPERTIES
        PREFIX ""
        OUTPUT_NAME "CJFRangeSelectorButton"
        SUFFIX ".auf2"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/aviutl2/Plugin/CJF"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/aviutl2/Plugin/CJF"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    )
endif()
