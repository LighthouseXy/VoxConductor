"""Device integration source guards; optical/input latency requires device measurement."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]/"firmware/main"
view=(root/"app_games.c").read_text()
main=(root/"main.c").read_text()
ui=(root/"app_ui.c").read_text()
desktop=(root/"app_desktop.c").read_text()
button=(root/"app_button.c").read_text()
assert "xQueueCreateStatic(12" in view and "xQueueSend(queue, &input, 0)" in view
assert "return event != APP_BUTTON_EVENT_PRESS_DOWN" in view
assert "app_button_press_time_ms()" in view and "LV_EVENT_FLUSH_START" in view
assert "lv_display_remove_event_cb_with_user_data" in view and "lv_obj_delete(root)" in view
assert "app_motion_set_enabled(false)" in view and "app_motion_set_enabled(true)" in view
assert "app_games_tick()" in desktop and "app_games_open(s_desktop_parent, index - 4)" in desktop
assert "if (s_menu == MENU_ARCADE) enter(MENU_ROOT)" in desktop
assert "if (app_games_active()) app_desktop_back()" in ui
assert "if (app_games_active())" in main and "return app_games_button(event)" in main
tick=view.split("bool app_games_tick(void)",1)[1]
assert "lv_obj_create" not in tick and "malloc(" not in tick
assert "app_audio_" not in view and "app_cloud_" not in view
assert button.index("s_press_edge_ms =") < button.index("s_press_consumed = emit_event(APP_BUTTON_EVENT_PRESS_DOWN)")
assert "commit_text(title,title_text)" in view
print('PASS: arcade input queue/lifetime/voice priority/no per-frame allocation and reaction timestamp wiring; scope=offline checks; pending=device timing validation')
