/*
 * retro-dosbox — the touchscreen as a trackpad.
 *
 * A finger is not a mouse, and the difference is not a detail. SDL will
 * happily synthesise mouse events from touches, and that is what this app
 * used to feed the guest: every tap teleported the pointer by the distance
 * between the last touch and this one, and every drag arrived as a held
 * button, so a Windows desktop answered a scrolled finger by rubber-banding a
 * selection box or dragging whatever icon it happened to land on.
 *
 * So touches are read directly and turned into the gestures a laptop trackpad
 * has taught everybody:
 *
 *     drag                  move the pointer, relative, at a set speed
 *     tap                   left click where the pointer already is
 *     press and hold        right click -- with a ring that fills while you
 *                           hold, because an invisible timer is not a control
 *     double-tap and drag   hold the left button down and drag, which is how
 *                           an icon gets moved or a selection made
 *
 * Nothing here is absolute. A PS/2 mouse reports movement, not position, so
 * "put the pointer under my finger" is not a thing the guest can be told; it
 * can only be steered. Relative movement is therefore not a compromise, it is
 * the only honest model -- and it is the one that survives the guest having
 * its own pointer speed and acceleration, which it does and which cannot be
 * read from out here.
 */
#ifndef RETRODOS_TOUCHMOUSE_H
#define RETRODOS_TOUCHMOUSE_H

#include <cstdint>

union SDL_Event;

namespace retrodos {

class TouchMouse {
public:
    /*
     * How much guest movement one pixel of finger travel is worth.
     *
     * Set by the caller because only the caller knows how big the guest's
     * picture is on this screen and what the player asked for -- this class
     * deliberately knows nothing about either.
     */
    void set_gain(float units_per_pixel) { gain_ = units_per_pixel; }

    /*
     * Send only multiples of this many units.
     *
     * A booted guest reads a PS/2 mouse, and DOSBox-X's 8042 turns N units
     * into N*(1<<resolution)/16 counts with INTEGER division and then throws
     * the remainder away -- so with the resolution Windows programs, anything
     * under two units is not "a small movement", it is no movement at all. A
     * slow, careful drag would send a stream of ones and the pointer would sit
     * perfectly still. Rounding down to a multiple here keeps the remainder on
     * this side of the wall, where the next frame can still use it.
     */
    void set_quantum(int units) { quantum_ = units > 0 ? units : 1; }

    /* One finger event. True when it was consumed. */
    bool handle_event(const SDL_Event &ev, int win_w, int win_h);

    /*
     * Movement from a real mouse, in screen pixels.
     *
     * It comes through here rather than going straight to the guest so that
     * both pointers are scaled by the same gain and, more importantly, share
     * one accumulator: the quantum below is a wall that whole-number movement
     * falls foul of whatever produced it, and a desk mouse moved slowly hits
     * it exactly as a finger does.
     */
    void add_motion(float dx_px, float dy_px);

    /*
     * Once a frame, whether or not a finger moved.
     *
     * This is where movement is actually sent, where a held press becomes a
     * right click, and where a click that has been down long enough is
     * released. A click cannot be pressed and released in one go: both would
     * be drained by the same pump and reach the guest inside one instant of
     * its time, which is not a click anyone's software recognises.
     */
    void update();

    /* The hold indicator, in ImGui's foreground list. */
    void draw() const;

    /* Let go of everything. For leaving the game: a button still held when
     * the picture disappears is held forever. */
    void reset();

    /* How much has actually reached the guest through here. The overlay shows
     * it beside the mouse's own count, because "the pointer does not move" has
     * two causes that look identical from outside -- nothing is arriving, or
     * it is arriving and going nowhere -- and a touchscreen now uses this path
     * rather than the mouse one. */
    unsigned long sent() const { return sent_; }

private:
    void flush_motion();
    void press(int button);
    void release_now();

    float    gain_     = 1.0f;
    int      quantum_  = 1;

    /* The finger being tracked, and where it is. Only one drives the pointer:
     * a second finger belongs to the on-screen pad, or to nothing. */
    bool     tracking_ = false;
    uint64_t finger_   = 0;
    float    start_x_  = 0.0f, start_y_ = 0.0f;
    float    last_x_   = 0.0f, last_y_  = 0.0f;
    float    slop_     = 12.0f;   /* px before a tap becomes a drag */
    uint64_t down_ms_  = 0;
    bool     moved_    = false;
    bool     long_done_= false;   /* the right click has already been sent */
    bool     dragging_ = false;   /* left button held for a double-tap drag */

    /* The tap before this one, for spotting a double tap. */
    uint64_t last_up_ms_ = 0;
    float    last_up_x_  = 0.0f, last_up_y_ = 0.0f;

    /* Sub-unit movement that has not been sent yet. */
    float    acc_x_ = 0.0f, acc_y_ = 0.0f;

    /* A button that is down and owes a release. */
    int      held_button_ = -1;
    uint64_t release_at_  = 0;

    unsigned long sent_ = 0;
};

} /* namespace retrodos */

#endif /* RETRODOS_TOUCHMOUSE_H */
