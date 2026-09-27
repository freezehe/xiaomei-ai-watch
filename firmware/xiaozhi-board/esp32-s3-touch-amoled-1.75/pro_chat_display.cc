#include "pro_chat_display.h"

#include "application.h"
#include "lvgl_theme.h"

#include <esp_log.h>
#include <cstring>
#include <string>

#define TAG "ProChatDisplay"

// Glass look over the nebula background: dark translucent fills, thin light borders
#define COLOR_GLASS        0x0B0B1E
#define COLOR_USER_BUBBLE  0x5E5CE6
#define COLOR_SYSTEM_TEXT  0xC7C7CC

// State colors (status dot + orb animation)
#define COLOR_IDLE         0xA78BFA
#define COLOR_CONNECTING   0x0A84FF
#define COLOR_LISTENING    0x30D158
#define COLOR_THINKING     0xFFB340
#define COLOR_SPEAKING     0x64D2FF
#define COLOR_OTHER        0x8E8E93

// Layout for the 466x466 circle. The half-width available at distance d from the centre is
// sqrt(233^2 - d^2); every box below stays inside it.
#define ORB_ZONE_SIZE      170
#define ORB_DISC_SIZE      104
#define RING_SIZE          120
#define ORB_UP_OFFSET      (-66)     // Orb centre moves up when there are chat bubbles
#define CHAT_Y             262
#define CHAT_W             300
#define CHAT_H             146
#define BUBBLE_MAX_W       250
#define MAX_BUBBLES        12
#define MAX_BUBBLE_TEXT    360       // Streaming sentences are merged into one bubble up to this

#define STATE_POLL_MS      150

static const char* const ROLE_USER = "user";
static const char* const ROLE_ASSISTANT = "assistant";
static const char* const ROLE_SYSTEM = "system";

static lv_obj_t* create_plain(lv_obj_t* parent) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

// ---------------------------------------------------------------- animation callbacks
static void anim_border_opa(void* obj, int32_t v) {
    lv_obj_set_style_border_opa(static_cast<lv_obj_t*>(obj), (lv_opa_t)v, 0);
}

// Ripple: the ring grows and fades out
static void anim_ripple(void* obj, int32_t v) {
    auto o = static_cast<lv_obj_t*>(obj);
    int32_t size = RING_SIZE - 12 + (ORB_ZONE_SIZE - 8 - (RING_SIZE - 12)) * v / 1000;
    lv_obj_set_size(o, size, size);
    lv_obj_set_style_border_opa(o, (lv_opa_t)(220 * (1000 - v) / 1000), 0);
}

static void anim_size(void* obj, int32_t v) {
    lv_obj_set_size(static_cast<lv_obj_t*>(obj), v, v);
}

// Spinning arc (lv_spinner is disabled in this LVGL config, so rotate a plain lv_arc)
static void anim_arc_rotation(void* obj, int32_t v) {
    lv_arc_set_rotation(static_cast<lv_obj_t*>(obj), v);
}

static void anim_orb_y(void* obj, int32_t v) {
    lv_obj_align(static_cast<lv_obj_t*>(obj), LV_ALIGN_CENTER, 0, v);
}

static void start_anim(lv_obj_t* obj, lv_anim_exec_xcb_t exec, int32_t from, int32_t to,
                       uint32_t duration, bool playback, uint32_t delay = 0) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, duration);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    if (playback) {
        lv_anim_set_reverse_duration(&a, duration);
    }
    lv_anim_start(&a);
}

// ---------------------------------------------------------------- setup
ProChatDisplay::ProChatDisplay(esp_lcd_panel_io_handle_t io_handle,
                               esp_lcd_panel_handle_t panel_handle, int width, int height,
                               int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                               bool swap_xy)
    : SpiLcdDisplay(io_handle, panel_handle, width, height, offset_x, offset_y, mirror_x, mirror_y,
                    swap_xy) {}

void ProChatDisplay::SetupUI() {
    if (setup_ui_called_) {
        return;
    }
    // Creates the standard objects (status/network/battery labels, emoji, ...) that the rest of
    // the firmware updates; they are restyled below
    SpiLcdDisplay::SetupUI();

    DisplayLockGuard lock(this);
    lv_obj_t* screen = lv_screen_active();

    // Orb: rings behind a glass disc that holds the emoji
    orb_zone_ = create_plain(screen);
    lv_obj_set_size(orb_zone_, ORB_ZONE_SIZE, ORB_ZONE_SIZE);
    lv_obj_align(orb_zone_, LV_ALIGN_CENTER, 0, 0);
    for (auto& ring : ring_) {
        ring = create_plain(orb_zone_);
        lv_obj_set_size(ring, RING_SIZE, RING_SIZE);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(ring, 3, 0);
        lv_obj_center(ring);
        lv_obj_add_flag(ring, LV_OBJ_FLAG_HIDDEN);
    }
    orb_disc_ = create_plain(orb_zone_);
    lv_obj_set_size(orb_disc_, ORB_DISC_SIZE, ORB_DISC_SIZE);
    lv_obj_set_style_radius(orb_disc_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(orb_disc_, lv_color_hex(COLOR_GLASS), 0);
    lv_obj_set_style_bg_opa(orb_disc_, LV_OPA_50, 0);
    lv_obj_set_style_border_width(orb_disc_, 1, 0);
    lv_obj_set_style_border_color(orb_disc_, lv_color_white(), 0);
    lv_obj_set_style_border_opa(orb_disc_, LV_OPA_30, 0);
    lv_obj_center(orb_disc_);
    lv_obj_set_parent(emoji_box_, orb_disc_);
    lv_obj_center(emoji_box_);

    // Status dot inside the pill
    status_dot_ = create_plain(status_bar_);
    lv_obj_set_size(status_dot_, 10, 10);
    lv_obj_set_style_radius(status_dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(status_dot_, LV_OPA_COVER, 0);
    lv_obj_align(status_dot_, LV_ALIGN_LEFT_MID, 18, 0);

    // Chat history
    chat_ = create_plain(screen);
    lv_obj_add_flag(chat_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(chat_, CHAT_W, CHAT_H);
    lv_obj_align(chat_, LV_ALIGN_TOP_MID, 0, CHAT_Y);
    lv_obj_set_flex_flow(chat_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(chat_, 8, 0);
    lv_obj_set_scroll_dir(chat_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(chat_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(chat_, LV_OBJ_FLAG_HIDDEN);

    ApplyStyles();

    // Keep the status layers and popups above the orb and the chat
    lv_obj_move_foreground(top_bar_);
    lv_obj_move_foreground(status_bar_);
    lv_obj_move_foreground(preview_image_);
    lv_obj_move_foreground(low_battery_popup_);

    state_timer_ = lv_timer_create(OnStateTimer, STATE_POLL_MS, this);
}

// Restyle the shared objects; runs again after every theme change (LcdDisplay::SetTheme
// resets some of these properties)
void ProChatDisplay::ApplyStyles() {
    lv_obj_set_width(top_bar_, 200);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_set_size(status_bar_, 230, 42);
    lv_obj_set_style_radius(status_bar_, 21, 0);
    lv_obj_set_style_bg_color(status_bar_, lv_color_hex(COLOR_GLASS), 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_60, 0);
    lv_obj_set_style_border_width(status_bar_, 1, 0);
    lv_obj_set_style_border_color(status_bar_, lv_color_white(), 0);
    lv_obj_set_style_border_opa(status_bar_, LV_OPA_20, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 54);
    lv_obj_set_width(status_label_, 170);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 12, 0);
    lv_obj_set_width(notification_label_, 210);
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);

    // The single-line subtitle bar is replaced by the bubbles
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
}

void ProChatDisplay::SetTheme(Theme* theme) {
    SpiLcdDisplay::SetTheme(theme);
    if (chat_ != nullptr) {
        DisplayLockGuard lock(this);
        ApplyStyles();
    }
}

// ---------------------------------------------------------------- state animation
void ProChatDisplay::OnStateTimer(lv_timer_t* timer) {
    auto self = static_cast<ProChatDisplay*>(lv_timer_get_user_data(timer));
    auto state = Application::GetInstance().GetDeviceState();
    if (state != kDeviceStateListening) {
        self->thinking_ = false;
    }

    Mode mode;
    if (self->sleeping_) {
        mode = Mode::SLEEP;
    } else {
        switch (state) {
            case kDeviceStateIdle:
                mode = Mode::IDLE;
                break;
            case kDeviceStateConnecting:
                mode = Mode::CONNECTING;
                break;
            case kDeviceStateListening:
                mode = self->thinking_ ? Mode::THINKING : Mode::LISTENING;
                break;
            case kDeviceStateSpeaking:
            case kDeviceStateNotifying:
                mode = Mode::SPEAKING;
                break;
            default:
                mode = Mode::OTHER;
                break;
        }
    }
    if (mode != self->mode_) {
        self->ApplyMode(mode);
    }
}

void ProChatDisplay::StopAnimations() {
    for (auto ring : ring_) {
        lv_anim_delete(ring, nullptr);
        lv_obj_add_flag(ring, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(ring, RING_SIZE, RING_SIZE);
        lv_obj_set_style_border_width(ring, 3, 0);
        lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);
    }
    if (spinner_ != nullptr) {
        lv_anim_delete(spinner_, nullptr);
        lv_obj_delete(spinner_);
        spinner_ = nullptr;
    }
}

void ProChatDisplay::ApplyMode(Mode mode) {
    StopAnimations();
    mode_ = mode;

    uint32_t color;
    switch (mode) {
        case Mode::IDLE: color = COLOR_IDLE; break;
        case Mode::CONNECTING: color = COLOR_CONNECTING; break;
        case Mode::LISTENING: color = COLOR_LISTENING; break;
        case Mode::THINKING: color = COLOR_THINKING; break;
        case Mode::SPEAKING: color = COLOR_SPEAKING; break;
        default: color = COLOR_OTHER; break;
    }
    lv_obj_set_style_bg_color(status_dot_, lv_color_hex(color), 0);
    for (auto ring : ring_) {
        lv_obj_set_style_border_color(ring, lv_color_hex(color), 0);
    }

    switch (mode) {
        case Mode::IDLE:                // Slow breathing
            lv_obj_remove_flag(ring_[0], LV_OBJ_FLAG_HIDDEN);
            start_anim(ring_[0], anim_border_opa, 40, 170, 1800, true);
            break;
        case Mode::LISTENING:           // Two ripples, half a period apart
            for (int i = 0; i < 2; i++) {
                lv_obj_remove_flag(ring_[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_border_opa(ring_[i], LV_OPA_TRANSP, 0);
                start_anim(ring_[i], anim_ripple, 0, 1000, 1500, false, i * 750);
            }
            break;
        case Mode::SPEAKING:            // Fast pulse + glowing outer ring
            lv_obj_remove_flag(ring_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_border_width(ring_[0], 5, 0);
            start_anim(ring_[0], anim_size, RING_SIZE - 8, RING_SIZE + 22, 320, true);
            lv_obj_remove_flag(ring_[1], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(ring_[1], ORB_ZONE_SIZE - 14, ORB_ZONE_SIZE - 14);
            lv_obj_set_style_border_width(ring_[1], 2, 0);
            start_anim(ring_[1], anim_border_opa, 30, 130, 520, true);
            break;
        case Mode::CONNECTING:
        case Mode::THINKING:            // Spinning arc
            spinner_ = lv_arc_create(orb_zone_);
            lv_obj_set_size(spinner_, RING_SIZE + 16, RING_SIZE + 16);
            lv_obj_center(spinner_);
            lv_obj_remove_flag(spinner_, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_opa(spinner_, LV_OPA_TRANSP, LV_PART_KNOB);
            lv_obj_set_style_pad_all(spinner_, 0, LV_PART_KNOB);
            lv_arc_set_bg_angles(spinner_, 0, 360);
            lv_arc_set_angles(spinner_, 0, 80);
            lv_obj_set_style_arc_width(spinner_, 4, LV_PART_MAIN);
            lv_obj_set_style_arc_width(spinner_, 4, LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(spinner_, lv_color_white(), LV_PART_MAIN);
            lv_obj_set_style_arc_opa(spinner_, LV_OPA_10, LV_PART_MAIN);
            lv_obj_set_style_arc_color(spinner_, lv_color_hex(color), LV_PART_INDICATOR);
            lv_obj_set_style_arc_rounded(spinner_, true, LV_PART_INDICATOR);
            {
                lv_anim_t a;
                lv_anim_init(&a);
                lv_anim_set_var(&a, spinner_);
                lv_anim_set_exec_cb(&a, anim_arc_rotation);
                lv_anim_set_values(&a, 0, 360);
                lv_anim_set_duration(&a, mode == Mode::THINKING ? 900 : 1300);
                lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
                lv_anim_start(&a);
            }
            lv_obj_move_to_index(spinner_, 0);  // Behind the glass disc
            break;
        case Mode::OTHER:               // Static dim ring
            lv_obj_remove_flag(ring_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_border_opa(ring_[0], LV_OPA_40, 0);
            break;
        default:                        // SLEEP: nothing moves while the screen is off
            break;
    }
}

void ProChatDisplay::SetPowerSaveMode(bool on) {
    sleeping_ = on;
    SpiLcdDisplay::SetPowerSaveMode(on);
}

void ProChatDisplay::MoveOrb(bool has_chat) {
    if (has_chat == orb_up_) {
        return;
    }
    orb_up_ = has_chat;
    lv_anim_delete(orb_zone_, anim_orb_y);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, orb_zone_);
    lv_anim_set_exec_cb(&a, anim_orb_y);
    lv_anim_set_values(&a, lv_obj_get_y_aligned(orb_zone_), has_chat ? ORB_UP_OFFSET : 0);
    lv_anim_set_duration(&a, 350);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

// ---------------------------------------------------------------- chat bubbles
lv_obj_t* ProChatDisplay::AddBubble(const char* role, const char* content) {
    const bool user = (role == ROLE_USER);
    const bool system = (role == ROLE_SYSTEM);

    lv_obj_t* row = create_plain(chat_);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, user ? LV_FLEX_ALIGN_END : system ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* bubble = create_plain(row);
    lv_obj_set_size(bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bubble, 18, 0);
    lv_obj_set_style_pad_hor(bubble, system ? 4 : 14, 0);
    lv_obj_set_style_pad_ver(bubble, system ? 2 : 8, 0);
    if (user) {
        lv_obj_set_style_bg_color(bubble, lv_color_hex(COLOR_USER_BUBBLE), 0);
        lv_obj_set_style_bg_opa(bubble, LV_OPA_90, 0);
    } else if (!system) {
        lv_obj_set_style_bg_color(bubble, lv_color_hex(COLOR_GLASS), 0);
        lv_obj_set_style_bg_opa(bubble, LV_OPA_70, 0);
        lv_obj_set_style_border_width(bubble, 1, 0);
        lv_obj_set_style_border_color(bubble, lv_color_white(), 0);
        lv_obj_set_style_border_opa(bubble, LV_OPA_20, 0);
    }

    const int max_w = system ? CHAT_W - 8 : BUBBLE_MAX_W - 28;
    lv_obj_t* label = lv_label_create(bubble);
    lv_obj_set_width(label, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(label, max_w, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(label, system ? lv_color_hex(COLOR_SYSTEM_TEXT) : lv_color_white(), 0);
    if (system) {
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_label_set_text(label, content);
    return label;
}

void ProChatDisplay::TrimHistory() {
    while (lv_obj_get_child_count(chat_) > MAX_BUBBLES) {
        lv_obj_delete(lv_obj_get_child(chat_, 0));
    }
}

void ProChatDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_ == nullptr) {
        return;
    }
    const char* r = (strcmp(role, "user") == 0)        ? ROLE_USER
                    : (strcmp(role, "assistant") == 0) ? ROLE_ASSISTANT
                                                       : ROLE_SYSTEM;

    if (content == nullptr || content[0] == '\0') {
        // Application clears its one-line system message this way: drop a trailing system line
        if (last_bubble_ != nullptr && last_role_ == ROLE_SYSTEM) {
            lv_obj_delete(lv_obj_get_parent(lv_obj_get_parent(last_bubble_)));
            last_bubble_ = nullptr;
            last_role_ = "";
        }
    } else {
        if (r == ROLE_USER) {
            thinking_ = true;
        }
        if (last_bubble_ != nullptr && r == last_role_ && r == ROLE_SYSTEM) {
            lv_label_set_text(last_bubble_, content);    // Status-like: replace
        } else if (last_bubble_ != nullptr && r == last_role_ && r == ROLE_ASSISTANT &&
                   strlen(lv_label_get_text(last_bubble_)) + strlen(content) < MAX_BUBBLE_TEXT) {
            std::string text = lv_label_get_text(last_bubble_);  // Streamed sentence: append
            text += content;
            lv_label_set_text(last_bubble_, text.c_str());
        } else {
            last_bubble_ = AddBubble(r, content);
            last_role_ = r;
            TrimHistory();
        }
    }

    const bool has_chat = lv_obj_get_child_count(chat_) > 0;
    if (has_chat && !hide_subtitle_) {
        lv_obj_remove_flag(chat_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(chat_, LV_OBJ_FLAG_HIDDEN);
    }
    MoveOrb(has_chat && !hide_subtitle_);

    // Follow the newest text
    lv_obj_update_layout(chat_);
    int32_t bottom = lv_obj_get_scroll_bottom(chat_);
    if (bottom > 0) {
        lv_obj_scroll_to_y(chat_, lv_obj_get_scroll_y(chat_) + bottom, LV_ANIM_ON);
    }
}

void ProChatDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    thinking_ = false;
    if (chat_ == nullptr) {
        return;
    }
    lv_obj_clean(chat_);
    last_bubble_ = nullptr;
    last_role_ = "";
    lv_obj_add_flag(chat_, LV_OBJ_FLAG_HIDDEN);
    MoveOrb(false);
}

void ProChatDisplay::SetHideSubtitle(bool hide) {
    DisplayLockGuard lock(this);
    hide_subtitle_ = hide;
    if (chat_ == nullptr) {
        return;
    }
    const bool show = !hide && lv_obj_get_child_count(chat_) > 0;
    if (show) {
        lv_obj_remove_flag(chat_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(chat_, LV_OBJ_FLAG_HIDDEN);
    }
    MoveOrb(show);
}
