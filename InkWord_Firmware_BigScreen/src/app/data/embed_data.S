/**
 * @file embed_data.S
 * @brief 内嵌资产汇编捆绑（cjk_font_data.bin + default_words.json
 *        + demo_book.txt）
 *
 * pio espidf 的 EMBED_FILES 存在 SCons 路径双前缀 bug（生成目标
 * .pio/build/<env>/.pio/build/<env>/xxx.bin.S "Source not found"，
 * 2026-09-16 实测），改用 .incbin 自包含方案：手工导出与 IDF EMBED
 * 命名规则一致的全局符号（路径斜线→下划线 _binary_<path>_start/end，
 * 段名 .rodata.embedded 同 IDF 惯例），cjk_font.c / app_main.c 的
 * extern 声明零改动。
 *
 * .incbin 搜索序：汇编进程 cwd（= build 目录）+ -I 路径——裸文件名
 * 经 CMakeLists set_source_files_properties 传 -I 指向本文件所在
 * app/data/，资产与本文件同目录，增删资产只动本文件。
 */
    .section .rodata.embedded
    .align 4

    .global _binary_app_data_cjk_font_data_bin_start
    .global _binary_app_data_cjk_font_data_bin_end
_binary_app_data_cjk_font_data_bin_start:
    .incbin "cjk_font_data.bin"
_binary_app_data_cjk_font_data_bin_end:

    .global _binary_app_data_default_words_json_start
    .global _binary_app_data_default_words_json_end
_binary_app_data_default_words_json_start:
    .incbin "default_words.json"
_binary_app_data_default_words_json_end:

    .global _binary_app_data_demo_book_txt_start
    .global _binary_app_data_demo_book_txt_end
_binary_app_data_demo_book_txt_start:
    .incbin "demo_book.txt"
_binary_app_data_demo_book_txt_end:
