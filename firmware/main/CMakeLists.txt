# Compiled sources. Font .c and cat_icon.c define lv_font_t / lv_image_dsc_t
# symbols, so they must be SRCS (compiled), not EMBED.
set(app_srcs
    "main.cpp"
    "display_st7305.cpp"
    "lvgl_port.cpp"
    "rtc_pcf85063.cpp"
    "shtc3.cpp"
    "adc_battery.cpp"
    "net_time.cpp"
    "quote_store.cpp"
    "ui.cpp"
    "cat_icon.c"
    "cat_icon_48.c"
    "env_icons.c"
)

# Font C arrays are tools/build_fonts.sh output. Fail early with guidance
# if they are missing so the build error is actionable.
foreach(f font_ko_28.c font_ko_22.c font_ko_18.c font_ko_44.c font_digits_96.c quotes_data.c)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/${f}")
        list(APPEND app_srcs "${f}")
    else()
        message(FATAL_ERROR
            "Missing generated font ${f}. Run: cd firmware/tools && python3 build_data.py && bash build_fonts.sh && python3 embed_quotes.py, then copy font_ko_28.c font_ko_22.c font_ko_18.c font_ko_44.c font_digits_96.c into firmware/main/")
    endif()
endforeach()

idf_component_register(
    SRCS ${app_srcs}
    INCLUDE_DIRS "."
    REQUIRES esp_lcd driver esp_adc esp_wifi esp_netif esp_timer nvs_flash json lvgl esp_common lwip
)
