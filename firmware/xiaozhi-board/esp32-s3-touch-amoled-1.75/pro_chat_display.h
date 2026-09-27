#pragma once

#include "display/lcd_display.h"

#include <atomic>

/*
 * "Pro" chat UI for the 466x466 round AMOLED (1.75 / 1.75C), smartwatch style:
 *
 *            12:30   (wifi)   (battery)        <- top_bar_ (transparent, inside the circle)
 *              ( ● 聆听中 )                     <- status_bar_ restyled as a glass pill
 *               .-------.
 *              (  emoji  )  <- animated rings   <- "orb": glass disc + state animation
 *               '-------'
 *        [AI: 今天上海晴，气温…]                  <- scrollable bubbles (user right, AI left)
 *                   [我: 天气怎么样]
 *
 * States (polled from Application): idle = slow breathing ring, connecting / thinking = spinning
 * arc, listening = ripples, speaking = fast pulse. Everything is board-level: the shared
 * LcdDisplay code is untouched; its objects (status/network/battery labels, emoji) are reused
 * and only restyled, so Application keeps driving them as before.
 */
class ProChatDisplay : public SpiLcdDisplay {
public:
    ProChatDisplay(esp_lcd_panel_io_handle_t io_handle, esp_lcd_panel_handle_t panel_handle,
                   int width, int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                   bool swap_xy);

    virtual void SetupUI() override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void ClearChatMessages() override;
    virtual void SetTheme(Theme* theme) override;
    virtual void SetHideSubtitle(bool hide) override;
    virtual void SetPowerSaveMode(bool on) override;

private:
    enum class Mode { NONE, IDLE, CONNECTING, LISTENING, THINKING, SPEAKING, OTHER, SLEEP };

    static void OnStateTimer(lv_timer_t* timer);
    void ApplyStyles();
    void ApplyMode(Mode mode);
    void StopAnimations();
    void MoveOrb(bool has_chat);
    lv_obj_t* AddBubble(const char* role, const char* content);
    void TrimHistory();

    lv_obj_t* orb_zone_ = nullptr;   // Holds the rings, spinner and glass disc; moves up with chat
    lv_obj_t* orb_disc_ = nullptr;
    lv_obj_t* ring_[2] = {nullptr, nullptr};
    lv_obj_t* spinner_ = nullptr;
    lv_obj_t* status_dot_ = nullptr;
    lv_obj_t* chat_ = nullptr;
    lv_obj_t* last_bubble_ = nullptr;    // Label of the newest bubble
    const char* last_role_ = "";
    lv_timer_t* state_timer_ = nullptr;

    Mode mode_ = Mode::NONE;
    bool orb_up_ = false;
    std::atomic<bool> thinking_{false};  // User text arrived, waiting for the answer
    std::atomic<bool> sleeping_{false};
};
