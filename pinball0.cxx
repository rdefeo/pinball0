#include <furi.h>

#include <notification/notification.h>
#include <cstring>
#include "pinball0.h"
#include "table.h"
#include "notifications.h"
#include "settings.h"

/* generated by fbt from .png files in images folder */
#include <pinball0_icons.h>

// Gravity should be lower than 9.8 m/s^2 since the ball is on
// an angled table. We could calc this and derive the actual
// vertical vector based on the angle of the table yadda yadda yadda
#define GRAVITY           3.0f // 9.8f
#define PHYSICS_SUB_STEPS 5
#define GAME_FPS          30
#define MANUAL_ADJUSTMENT 20
#define IDLE_TIMEOUT      120 * 1000 // 120 seconds * 1000 ticks/sec
#define BUMP_DELAY        2 * 1000 // 2 seconds
#define BUMP_MAX          3

void solve(PinballApp* pb, float dt) {
    Table* table = pb->table;

    float sub_dt = dt / PHYSICS_SUB_STEPS;
    for(int ss = 0; ss < PHYSICS_SUB_STEPS; ss++) {
        // apply gravity (and any other forces?)
        // FURI_LOG_I(TAG, "Applying gravity");
        if(table->balls_released) {
            float bump_amt = 1.0f;
            if(pb->keys[InputKeyUp]) {
                bump_amt = -1.04f;
            }
            for(auto& b : table->balls) {
                // We multiply GRAVITY by dt since gravity is based on seconds
                b.accelerate(Vec2(0, GRAVITY * bump_amt * sub_dt));
            }
        }

        // apply collisions (among moving objects)
        // only needed for multi-ball! - is this true? what about flippers...
        for(size_t b1 = 0; b1 < table->balls.size(); b1++) {
            for(size_t b2 = b1 + 1; b2 < table->balls.size(); b2++) {
                if(b1 != b2) {
                    auto& ball1 = table->balls[b1];
                    auto& ball2 = table->balls[b2];

                    Vec2 axis = ball1.p - ball2.p;
                    float dist2 = axis.mag2();
                    float dist = sqrtf(dist2);
                    float rr = ball1.r + ball2.r;
                    if(dist < rr) {
                        Vec2 v1 = ball1.p - ball1.prev_p;
                        Vec2 v2 = ball2.p - ball2.prev_p;

                        float factor = (dist - rr) / dist;
                        ball1.p -= axis * factor * 0.5f;
                        ball2.p -= axis * factor * 0.5f;

                        float damping = 1.01f;
                        float f1 = (damping * (axis.x * v1.x + axis.y * v1.y)) / dist2;
                        float f2 = (damping * (axis.x * v2.x + axis.y * v2.y)) / dist2;

                        v1.x += f2 * axis.x - f1 * axis.x;
                        v2.x += f1 * axis.x - f2 * axis.x;
                        v1.y += f2 * axis.y - f1 * axis.y;
                        v2.y += f1 * axis.y - f2 * axis.y;

                        ball1.prev_p = ball1.p - v1;
                        ball2.prev_p = ball2.p - v2;
                    }
                }
            }
        }

        // collisions with static objects and flippers
        for(auto& b : table->balls) {
            for(auto& o : table->objects) {
                if(o->physical && o->collide(b)) {
                    if(pb->game_mode == GM_Tilted) {
                        continue;
                    }
                    if(o->notification) {
                        (*o->notification)(pb);
                    }
                    table->score.value += o->score;
                    o->reset_animation();
                    continue;
                }
            }
            for(auto& f : table->flippers) {
                if(f.collide(b)) {
                    if(pb->game_mode == GM_Tilted) {
                        continue;
                    }
                    if(f.notification) {
                        (*f.notification)(pb);
                    }
                    table->score.value += f.score;
                    continue;
                }
            }
        }

        // update positions - of balls AND flippers
        if(table->balls_released) {
            for(auto& b : table->balls) {
                b.update(sub_dt);
            }
        }
        for(auto& f : table->flippers) {
            f.update(sub_dt);
        }
    }

    // Did any balls fall off the table?
    if(table->balls.size()) {
        auto num_in_play = table->balls.size();
        auto i = table->balls.begin();
        while(i != table->balls.end()) {
            if(i->p.y > 1280 + 100) {
                FURI_LOG_I(TAG, "ball off table!");
                i = table->balls.erase(i);
                num_in_play--;
                notify_lost_life(pb);
            } else {
                ++i;
            }
        }
        if(num_in_play == 0) {
            table->balls_released = false;
            table->lives.value--;
            if(table->lives.value > 0) {
                // Reset our ball to it's starting position
                table->balls = table->balls_initial;
                if(pb->game_mode == GM_Tilted) {
                    pb->game_mode = GM_Playing;
                }
            } else {
                table->game_over = true;
            }
        }
    }
}

static void pinball_draw_callback(Canvas* const canvas, void* ctx) {
    furi_assert(ctx);
    PinballApp* pb = (PinballApp*)ctx;
    furi_mutex_acquire(pb->mutex, FuriWaitForever);

    // What are we drawing? table select / menu or the actual game?
    switch(pb->game_mode) {
    case GM_TableSelect: {
        canvas_draw_icon(canvas, 0, 0, &I_pinball0_logo); // our sweet logo
        // draw the list of table names: display it as a carousel - where the list repeats
        // and the currently selected item is always in the middle, surrounded by pinballs
        const TableList& list = pb->table_list;
        int32_t y = 25;
        auto half_way = list.display_size / 2;

        for(auto i = 0; i < list.display_size; i++) {
            int index =
                (list.selected - half_way + i + list.menu_items.size()) % list.menu_items.size();
            const auto& menu_item = list.menu_items[index];
            canvas_draw_str_aligned(
                canvas,
                LCD_WIDTH / 2,
                y,
                AlignCenter,
                AlignTop,
                furi_string_get_cstr(menu_item.name));
            if(i == half_way) {
                canvas_draw_disc(canvas, 8, y + 3, 2);
                canvas_draw_disc(canvas, 56, y + 3, 2);
            }
            y += 12;
        }

        pb->table->draw(canvas);
    } break;
    case GM_Playing:
        pb->table->draw(canvas);
        break;
    case GM_GameOver: {
        pb->table->draw(canvas);

        const int32_t y = 56;
        const size_t interval = 40;
        const float theta = (float)((pb->tick % interval) / (interval * 1.0f)) * (float)(M_PI * 2);
        const float sin_theta_4 = sinf(theta) * 4;

        const int border = 3;
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_box(
            canvas, 16 - border, y + sin_theta_4 - border, 32 + border * 2, 16 + border * 2);
        canvas_set_color(canvas, ColorBlack);

        canvas_draw_icon(canvas, 16, y + sin_theta_4, &I_Arcade_G);
        canvas_draw_icon(canvas, 24, y + sin_theta_4, &I_Arcade_A);
        canvas_draw_icon(canvas, 32, y + sin_theta_4, &I_Arcade_M);
        canvas_draw_icon(canvas, 40, y + sin_theta_4, &I_Arcade_E);

        canvas_draw_icon(canvas, 16, y + sin_theta_4 + 8, &I_Arcade_O);
        canvas_draw_icon(canvas, 24, y + sin_theta_4 + 8, &I_Arcade_V);
        canvas_draw_icon(canvas, 32, y + sin_theta_4 + 8, &I_Arcade_E);
        canvas_draw_icon(canvas, 40, y + sin_theta_4 + 8, &I_Arcade_R);
    } break;
    case GM_Error: {
        // pb->text contains error message
        canvas_draw_icon(canvas, 0, 10, &I_Arcade_E);
        canvas_draw_icon(canvas, 8, 10, &I_Arcade_R);
        canvas_draw_icon(canvas, 16, 10, &I_Arcade_R);
        canvas_draw_icon(canvas, 24, 10, &I_Arcade_O);
        canvas_draw_icon(canvas, 32, 10, &I_Arcade_R);

        int x = 10;
        int y = 30;
        // split the string on \n and display each line
        // strtok is disabled - whyyy
        char buf[256];
        strncpy(buf, pb->text, 256);
        char* str = buf;
        char* p = buf;
        bool at_end = false;
        while(str != NULL) {
            while(p && *p != '\n' && *p != '\0')
                p++;
            if(p && *p == '\0') at_end = true;
            *p = '\0';
            canvas_draw_str_aligned(canvas, x, y, AlignLeft, AlignTop, str);
            if(at_end) {
                str = NULL;
                break;
            }
            str = p + 1;
            p = str;
            y += 12;
        }

        pb->table->draw(canvas);
    } break;
    case GM_Settings: {
        // TODO: like... do better here. maybe vector of settings strings, etc
        canvas_draw_str_aligned(canvas, 2, 10, AlignLeft, AlignTop, "SETTINGS");

        int x = 55;
        int y = 30;

        canvas_draw_str_aligned(canvas, 10, y, AlignLeft, AlignTop, "Sound");
        canvas_draw_circle(canvas, x, y + 3, 4);
        if(pb->settings.sound_enabled) {
            canvas_draw_disc(canvas, x, y + 3, 2);
        }
        if(pb->settings.selected_setting == 0) {
            canvas_draw_triangle(canvas, 2, y + 3, 8, 5, CanvasDirectionLeftToRight);
        }
        y += 12;

        canvas_draw_str_aligned(canvas, 10, y, AlignLeft, AlignTop, "LED");
        canvas_draw_circle(canvas, x, y + 3, 4);
        if(pb->settings.led_enabled) {
            canvas_draw_disc(canvas, x, y + 3, 2);
        }
        if(pb->settings.selected_setting == 1) {
            canvas_draw_triangle(canvas, 2, y + 3, 8, 5, CanvasDirectionLeftToRight);
        }
        y += 12;

        canvas_draw_str_aligned(canvas, 10, y, AlignLeft, AlignTop, "Vibrate");
        canvas_draw_circle(canvas, x, y + 3, 4);
        if(pb->settings.vibrate_enabled) {
            canvas_draw_disc(canvas, x, y + 3, 2);
        }
        if(pb->settings.selected_setting == 2) {
            canvas_draw_triangle(canvas, 2, y + 3, 8, 5, CanvasDirectionLeftToRight);
        }
        y += 12;

        canvas_draw_str_aligned(canvas, 10, y, AlignLeft, AlignTop, "Debug");
        canvas_draw_circle(canvas, x, y + 3, 4);
        if(pb->settings.debug_mode) {
            canvas_draw_disc(canvas, x, y + 3, 2);
        }
        if(pb->settings.selected_setting == 3) {
            canvas_draw_triangle(canvas, 2, y + 3, 8, 5, CanvasDirectionLeftToRight);
        }

        // About information
        canvas_draw_str_aligned(canvas, 2, 88, AlignLeft, AlignTop, "Pinball0 " VERSION);
        canvas_draw_str_aligned(canvas, 2, 98, AlignLeft, AlignTop, "github.com/");
        canvas_draw_str_aligned(canvas, 2, 108, AlignLeft, AlignTop, "  rdefeo/");
        canvas_draw_str_aligned(canvas, 2, 118, AlignLeft, AlignTop, "    pinball0");

        pb->table->draw(canvas);
    } break;
    case GM_Tilted: {
        pb->table->draw(canvas);

        const int32_t y = 56;
        const int border = 8;
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_box(canvas, 16 - border, y - border, 32 + border * 2, 8 + border * 2);
        canvas_set_color(canvas, ColorBlack);

        bool display = furi_get_tick() % 1000 < 500;
        if(display) {
            canvas_draw_icon(canvas, 17, y, &I_Arcade_T);
            canvas_draw_icon(canvas, 25, y, &I_Arcade_I);
            canvas_draw_icon(canvas, 33, y, &I_Arcade_L);
            canvas_draw_icon(canvas, 40, y, &I_Arcade_T);
        }

        int dots = 5;
        int x_start = 16;
        int x_gap = (48 - 16) / (dots - 1);
        for(int x = 0; x < 5; x++, x_start += x_gap) {
            if(x % 2 != display) {
                canvas_draw_disc(canvas, x_start, 50, 2);
                canvas_draw_disc(canvas, x_start, 70, 2);
            } else {
                canvas_draw_dot(canvas, x_start, 50);
                canvas_draw_dot(canvas, x_start, 70);
            }
        }

    } break;
    default:
        FURI_LOG_E(TAG, "Unknown Game Mode");
        break;
    }

    furi_mutex_release(pb->mutex);
}

static void pinball_input_callback(InputEvent* input_event, void* ctx) {
    furi_assert(ctx);
    FuriMessageQueue* event_queue = (FuriMessageQueue*)ctx;
    // PinballEvent event = {.type = EventTypeKey, .input = *input_event};
    furi_message_queue_put(event_queue, input_event, FuriWaitForever);
}

PinballApp::PinballApp() {
    initialized = false;

    mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!mutex) {
        FURI_LOG_E(TAG, "Cannot create mutex!");
        return;
    }

    storage = (Storage*)furi_record_open(RECORD_STORAGE);
    notify = (NotificationApp*)furi_record_open(RECORD_NOTIFICATION);
    // notify_init();
    notification_message(notify, &sequence_display_backlight_enforce_on);

    table = NULL;
    tick = 0;

    game_mode = GM_TableSelect;
    keys[InputKeyUp] = false;
    keys[InputKeyDown] = false;
    keys[InputKeyRight] = false;
    keys[InputKeyLeft] = false;

    initialized = true;
}

PinballApp::~PinballApp() {
    furi_mutex_free(mutex);
    delete table;
    // notify_free();

    notification_message(notify, &sequence_display_backlight_enforce_auto);
    notification_message(notify, &sequence_reset_rgb);

    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_NOTIFICATION);
}

extern "C" int32_t pinball0_app(void* p) {
    UNUSED(p);

    PinballApp app;
    if(!app.initialized) {
        FURI_LOG_E(TAG, "Failed to initialize Pinball0! Exiting.");
        return 0;
    }

    pinball_load_settings(app);

    // read the list of tables from storage
    table_table_list_init(&app);

    table_load_table(&app, TABLE_SELECT);

    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    furi_timer_set_thread_priority(FuriTimerThreadPriorityElevated);

    ViewPort* view_port = view_port_alloc();
    view_port_set_orientation(view_port, ViewPortOrientationVertical);
    view_port_draw_callback_set(view_port, pinball_draw_callback, &app);
    view_port_input_callback_set(view_port, pinball_input_callback, event_queue);

    // Open the GUI and register view_port
    Gui* gui = (Gui*)furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    // TODO: Dolphin deed actions
    // dolphin_deed(DolphinDeedPluginGameStart);

    app.processing = true;

    float dt = 0.0f;
    uint32_t last_frame_time = furi_get_tick();
    app.idle_start = last_frame_time;

    // I'm not thrilled with this event loop - kinda messy but it'll do for now
    InputEvent event;
    while(app.processing) {
        FuriStatus event_status = furi_message_queue_get(event_queue, &event, 10);
        furi_mutex_acquire(app.mutex, FuriWaitForever);

        if(event_status == FuriStatusOk) {
            if(event.type == InputTypePress || event.type == InputTypeLong ||
               event.type == InputTypeRepeat) {
                switch(event.key) {
                case InputKeyBack: // navigate to previous screen or exit
                    switch(app.game_mode) {
                    case GM_TableSelect:
                        app.processing = false;
                        break;
                    case GM_Settings:
                        pinball_save_settings(app);
                        // fall through
                    default:
                        app.game_mode = GM_TableSelect;
                        table_load_table(&app, TABLE_SELECT);
                        break;
                    }
                    break;
                case InputKeyRight: {
                    if(app.game_mode == GM_Tilted) {
                        break;
                    }

                    app.keys[InputKeyRight] = true;

                    if(app.settings.debug_mode && app.table->balls_released == false) {
                        app.table->balls[0].p.x += MANUAL_ADJUSTMENT;
                        app.table->balls[0].prev_p.x += MANUAL_ADJUSTMENT;
                    }
                    bool flipper_pressed = false;
                    for(auto& f : app.table->flippers) {
                        if(f.side == Flipper::RIGHT) {
                            f.powered = true;
                            if(f.rotation != f.max_rotation) {
                                flipper_pressed = true;
                            }
                        }
                    }
                    if(flipper_pressed) {
                        notify_flipper(&app);
                    }
                } break;
                case InputKeyLeft: {
                    if(app.game_mode == GM_Tilted) {
                        break;
                    }

                    app.keys[InputKeyLeft] = true;

                    if(app.settings.debug_mode && app.table->balls_released == false) {
                        app.table->balls[0].p.x -= MANUAL_ADJUSTMENT;
                        app.table->balls[0].prev_p.x -= MANUAL_ADJUSTMENT;
                    }
                    bool flipper_pressed = false;
                    for(auto& f : app.table->flippers) {
                        if(f.side == Flipper::LEFT) {
                            f.powered = true;
                            if(f.rotation != f.max_rotation) {
                                flipper_pressed = true;
                            }
                        }
                    }
                    if(flipper_pressed) {
                        notify_flipper(&app);
                    }
                } break;
                case InputKeyUp:
                    switch(app.game_mode) {
                    case GM_Playing:
                        if(event.type == InputTypePress) {
                            // Table bump and Tilt tracking
                            uint32_t current_tick = furi_get_tick();
                            if(current_tick - app.table->last_bump >= BUMP_DELAY) {
                                app.table->bump_count++;
                                app.table->last_bump = current_tick;
                                if(!app.table->tilt_detect_enabled ||
                                   app.table->bump_count < BUMP_MAX) {
                                    app.keys[InputKeyUp] = true;
                                    notify_table_bump(&app);
                                } else {
                                    FURI_LOG_W(TAG, "TABLE TILTED!");
                                    app.game_mode = GM_Tilted;
                                    app.table->bump_count = 0;
                                    notify_table_tilted(&app);
                                }
                            }
                        }
                        if(app.settings.debug_mode && app.table->balls_released == false) {
                            app.table->balls[0].p.y -= MANUAL_ADJUSTMENT;
                            app.table->balls[0].prev_p.y -= MANUAL_ADJUSTMENT;
                        }
                        break;
                    case GM_TableSelect:
                        app.table_list.selected =
                            (app.table_list.selected - 1 + app.table_list.menu_items.size()) %
                            app.table_list.menu_items.size();
                        break;
                    case GM_Settings:
                        if(app.settings.selected_setting > 0) {
                            app.settings.selected_setting--;
                        }
                        break;
                    default:
                        FURI_LOG_W(TAG, "Table tilted, UP does nothing!");
                        break;
                    }
                    break;
                case InputKeyDown:
                    switch(app.game_mode) {
                    case GM_Playing:
                        app.keys[InputKeyDown] = true;
                        if(app.settings.debug_mode && app.table->balls_released == false) {
                            app.table->balls[0].p.y += MANUAL_ADJUSTMENT;
                            app.table->balls[0].prev_p.y += MANUAL_ADJUSTMENT;
                        }
                        break;
                    case GM_TableSelect:
                        app.table_list.selected =
                            (app.table_list.selected + 1 + app.table_list.menu_items.size()) %
                            app.table_list.menu_items.size();
                        break;
                    case GM_Settings:
                        if(app.settings.selected_setting < app.settings.max_settings - 1) {
                            app.settings.selected_setting++;
                        }
                        break;
                    default:
                        break;
                    }
                    break;
                case InputKeyOk:
                    switch(app.game_mode) {
                    case GM_Playing:
                        if(!app.table->balls_released) {
                            app.table->balls_released = true;
                            notify_ball_released(&app);
                        }
                        break;
                    case GM_TableSelect: {
                        size_t sel = app.table_list.selected;
                        if(sel == app.table_list.menu_items.size() - 1) {
                            app.game_mode = GM_Settings;
                            table_load_table(&app, TABLE_SETTINGS);
                        } else if(!table_load_table(&app, sel + TABLE_INDEX_OFFSET)) {
                            app.game_mode = GM_Error;
                            table_load_table(&app, TABLE_ERROR);
                            notify_error_message(&app);
                        } else {
                            app.game_mode = GM_Playing;
                        }
                    } break;
                    case GM_Settings:
                        switch(app.settings.selected_setting) {
                        case 0:
                            app.settings.sound_enabled = !app.settings.sound_enabled;
                            break;
                        case 1:
                            app.settings.led_enabled = !app.settings.led_enabled;
                            break;
                        case 2:
                            app.settings.vibrate_enabled = !app.settings.vibrate_enabled;
                            break;
                        case 3:
                            app.settings.debug_mode = !app.settings.debug_mode;
                            break;
                        default:
                            break;
                        }
                        break;
                    default:
                        break;
                    }
                    break;
                default:
                    break;
                }
            } else if(event.type == InputTypeRelease) {
                if(event.key != InputKeyOk && event.key != InputKeyBack) {
                    app.keys[event.key] = false;
                    for(auto& f : app.table->flippers) {
                        if(event.key == InputKeyLeft && f.side == Flipper::LEFT) {
                            f.powered = false;
                        } else if(event.key == InputKeyRight && f.side == Flipper::RIGHT) {
                            f.powered = false;
                        }
                    }
                }
            }
            // a key was pressed, reset idle counter
            app.idle_start = furi_get_tick();
        }

        // update physics / motion
        solve(&app, dt);
        for(auto& o : app.table->objects) {
            o->step_animation();
        }

        // check game state
        if(app.game_mode != GM_GameOver && app.table->game_over) {
            FURI_LOG_I(TAG, "GAME OVER!");
            app.game_mode = GM_GameOver;
            notify_game_over(&app);
        }

        // render
        view_port_update(view_port);
        furi_mutex_release(app.mutex);

        // game timing + idle check
        uint32_t current_tick = furi_get_tick();
        if(current_tick - app.idle_start >= IDLE_TIMEOUT) {
            FURI_LOG_W(TAG, "Idle timeout! Exiting Pinball0...");
            app.processing = false;
            break;
        }

        uint32_t time_lapsed = current_tick - last_frame_time;
        dt = time_lapsed / 1000.0f;
        while(dt < 1.0f / GAME_FPS) {
            time_lapsed = furi_get_tick() - last_frame_time;
            dt = time_lapsed / 1000.0f;
        }
        app.tick++;
        last_frame_time = furi_get_tick();
    }

    // general cleanup
    view_port_enabled_set(view_port, false);
    gui_remove_view_port(gui, view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(view_port);
    furi_message_queue_free(event_queue);

    furi_timer_set_thread_priority(FuriTimerThreadPriorityNormal);
    return 0;
}
