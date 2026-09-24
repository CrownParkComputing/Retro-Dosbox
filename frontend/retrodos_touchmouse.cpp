/*
 * retro-dosbox — the touchscreen as a trackpad.
 */
#include "retrodos_touchmouse.h"

#include "imgui.h"
#include "retrodos_host.h"

#include <SDL3/SDL.h>


namespace retrodos {

namespace {

/* Long enough that a tap is never mistaken for a hold, short enough that
 * waiting for it does not feel like the app has stopped responding. The same
 * half second every touch platform uses for a press-and-hold. */
const uint64_t kLongPressMs = 450;

/* A second tap this soon after the first, and this close to it, starts a drag
 * instead of being another click. */
const uint64_t kDoubleTapMs   = 400;
const float    kDoubleTapSlop = 2.5f;   /* multiples of the move threshold */

/* How long a click is held down. Same reasoning as the on-screen keyboard's
 * key hold: a press and a release drained by one pump land in the same instant
 * of guest time, and a zero-length click is one a guest can miss entirely. */
const uint64_t kClickHoldMs = 70;

/* Movement before a touch stops being a tap. A fraction of the screen rather
 * than a pixel count: a finger wobbles by about the same fraction of a display
 * whatever its resolution. */
const float kSlopFraction = 0.012f;

const int kLeft = 0, kRight = 1;

/* Spelled out rather than taken from imgui_internal.h: one arc is not worth a
 * dependency on ImGui's private header. */
const float kPi = 3.14159265f;

} /* namespace */

void TouchMouse::reset()
{
    if (held_button_ >= 0) retrodos_host_mouse_button(held_button_, false);
    if (dragging_)         retrodos_host_mouse_button(kLeft, false);
    held_button_ = -1;
    dragging_    = false;
    tracking_    = false;
    moved_       = false;
    long_done_   = false;
    acc_x_ = acc_y_ = 0.0f;
}

void TouchMouse::press(int button)
{
    /* One at a time. A press arriving while the previous click still owes a
     * release would leave that button down for good. */
    release_now();
    retrodos_host_mouse_button(button, true);
    ++sent_;
    held_button_ = button;
    release_at_  = SDL_GetTicks() + kClickHoldMs;
}

void TouchMouse::release_now()
{
    if (held_button_ < 0) return;
    retrodos_host_mouse_button(held_button_, false);
    held_button_ = -1;
}

void TouchMouse::flush_motion()
{
    const int q = quantum_ > 0 ? quantum_ : 1;

    /* Truncate towards zero, in whole quanta, and keep the rest for next
     * frame. Sending the fraction would be sending nothing -- see the note on
     * set_quantum() -- and dropping it would make a slow drag stand still. */
    const int ux = (int)(acc_x_ / (float)q) * q;
    const int uy = (int)(acc_y_ / (float)q) * q;
    if (ux == 0 && uy == 0) return;

    acc_x_ -= (float)ux;
    acc_y_ -= (float)uy;
    ++sent_;

    /*
     * ONE call per frame, never one per event.
     *
     * Each queued move becomes its own PS/2 packet when the bridge drains the
     * queue, and the keyboard controller's buffer holds only a handful before
     * it starts discarding them -- so a burst of small moves is mostly thrown
     * away, and what survives is jerky. Coalescing into a single delta per
     * frame is both smoother and lossless.
     */
    retrodos_host_mouse_move(ux, uy);
}

void TouchMouse::add_motion(float dx_px, float dy_px)
{
    acc_x_ += dx_px * gain_;
    acc_y_ += dy_px * gain_;
}

bool TouchMouse::handle_event(const SDL_Event &ev, int win_w, int win_h)
{
    if (win_w <= 0 || win_h <= 0) return false;

    const float px = ev.tfinger.x * (float)win_w;
    const float py = ev.tfinger.y * (float)win_h;
    const uint64_t id  = (uint64_t)ev.tfinger.fingerID;
    const uint64_t now = SDL_GetTicks();

    switch (ev.type) {
    case SDL_EVENT_FINGER_DOWN: {
        /* A second finger is not a second pointer. Swallowed rather than
         * ignored, so it cannot be taken for a new gesture. */
        if (tracking_) return true;

        slop_     = kSlopFraction * (float)((win_w < win_h) ? win_w : win_h);
        tracking_ = true;
        finger_   = id;
        start_x_ = last_x_ = px;
        start_y_ = last_y_ = py;
        down_ms_  = now;
        moved_    = false;
        long_done_= false;

        /* Down again, straight after a tap and in the same place: this is the
         * second half of a double tap, and what follows is a drag. */
        const float dx = px - last_up_x_, dy = py - last_up_y_;
        const float near = slop_ * kDoubleTapSlop;
        if (now - last_up_ms_ <= kDoubleTapMs && (dx * dx + dy * dy) <= near * near) {
            release_now();          /* the first tap's own release, if it is due */
            retrodos_host_mouse_button(kLeft, true);
            dragging_ = true;
        }
        return true;
    }

    case SDL_EVENT_FINGER_MOTION: {
        if (!tracking_ || id != finger_) return false;

        acc_x_ += (px - last_x_) * gain_;
        acc_y_ += (py - last_y_) * gain_;
        last_x_ = px;
        last_y_ = py;

        if (!moved_) {
            const float dx = px - start_x_, dy = py - start_y_;
            if (dx * dx + dy * dy > slop_ * slop_) moved_ = true;
        }
        return true;
    }

    case SDL_EVENT_FINGER_UP: {
        if (!tracking_ || id != finger_) return false;
        tracking_ = false;

        if (dragging_) {
            retrodos_host_mouse_button(kLeft, false);
            dragging_ = false;
            return true;
        }

        /* A hold that has already fired its right click is finished; so is a
         * drag. What is left is a tap. */
        if (!moved_ && !long_done_) {
            press(kLeft);
            last_up_ms_ = now;
            last_up_x_  = px;
            last_up_y_  = py;
        }
        return true;
    }

    default:
        return false;
    }
}

void TouchMouse::update()
{
    const uint64_t now = SDL_GetTicks();

    flush_motion();

    /* Held still for long enough: right click, once, and the finger coming up
     * afterwards does nothing. */
    if (tracking_ && !moved_ && !dragging_ && !long_done_ &&
        now - down_ms_ >= kLongPressMs) {
        press(kRight);
        long_done_ = true;
    }

    if (held_button_ >= 0 && now >= release_at_) release_now();
}

void TouchMouse::draw() const
{
    /*
     * The hold, made visible.
     *
     * Without this a press-and-hold is a guess: nothing on screen says the
     * gesture was recognised, how long is left, or that letting go early gives
     * an ordinary click instead. The ring closes as the timer runs and is gone
     * the moment the right click is sent.
     */
    if (!tracking_ || moved_ || dragging_ || long_done_) return;

    const uint64_t elapsed = SDL_GetTicks() - down_ms_;
    /* Nothing for the first fraction of the hold: a ring that flashed up under
     * every tap would be noise over the game. */
    if (elapsed < kLongPressMs / 4) return;

    float t = (float)elapsed / (float)kLongPressMs;
    if (t > 1.0f) t = 1.0f;

    ImDrawList *dl = ImGui::GetForegroundDrawList();
    const ImVec2 c(last_x_, last_y_);
    const float  r = slop_ * 2.0f;

    dl->AddCircle(c, r, ImGui::GetColorU32(ImVec4(1, 1, 1, 0.25f)), 32, 2.0f);
    dl->PathArcTo(c, r, -kPi * 0.5f, -kPi * 0.5f + t * 2.0f * kPi, 32);
    dl->PathStroke(ImGui::GetColorU32(ImVec4(1, 1, 1, 0.85f)), 0, 3.0f);
}

} /* namespace retrodos */
