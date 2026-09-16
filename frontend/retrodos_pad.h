/*
 * retro-dosbox — virtual pad: on-screen controls and physical gamepads.
 *
 * Both input sources feed ONE button state here, and everything downstream
 * reads only that. A DOS game cannot tell whether Left came from a thumb on
 * glass or from a stick, and neither should the rest of the frontend -- the
 * alternative is two parallel mapping paths that drift apart.
 *
 * What the buttons DO is deliberately not fixed. Most DOS games are keyboard
 * games: Descent, Doom and Commander Keen all want arrows and Ctrl/Alt, not a
 * stick. So each virtual button carries a scancode, and the emulated game port
 * is driven as well only when asked for. Bindings are settings, per game.
 *
 * Positions are fractions of the screen rather than pixels. A phone, a handheld
 * and a tablet have very different screens, and a pixel layout that suits one
 * is off the edge of another -- rotating the device would move every control.
 * Fractions survive all of it.
 */
#ifndef RETRODOS_PAD_H
#define RETRODOS_PAD_H

#include <string>
#include <vector>

union SDL_Event;

namespace retrodos {

/** The virtual buttons. Deliberately a small, familiar set: a DOS game has
 *  nothing to do with a modern controller's full complement. */
enum PadButton {
    PAD_UP = 0, PAD_DOWN, PAD_LEFT, PAD_RIGHT,
    PAD_A, PAD_B, PAD_X, PAD_Y,
    PAD_L, PAD_R,
    PAD_LT, PAD_RT,
    PAD_START, PAD_SELECT,
    /* Appended after the originals: the enum values are persisted in saved
     * layouts and binding tables, so inserting earlier would silently remap
     * every existing config. */
    PAD_L3, PAD_R3,
    PAD_COUNT
};

/* The Xbox face-button colours. A DOS game has no notion of them, but the pad
 * in the player's hands does, and matching it is what makes "A fires" mean
 * anything at a glance. */
void pad_button_colour(int button, float *rgb);

/** Human-readable name, for the bindings UI. */
const char *pad_button_name(int button);

/** One on-screen control. */
struct PadControl {
    int         button = PAD_A;
    std::string label;
    float       x      = 0.5f;   /* centre, fraction of width  */
    float       y      = 0.5f;   /* centre, fraction of height */
    float       radius = 0.06f;  /* fraction of the SMALLER screen dimension */
};

/**
 * Default placement for a screen of this shape.
 *
 * x is a fraction of width and y a fraction of height, so equal fractional
 * offsets are NOT equal distances: on a 16:9 screen a horizontal offset spans
 * nearly twice the pixels a vertical one does, which turns a d-pad cross into a
 * wide diamond. Offsets are therefore expressed in units of the smaller
 * dimension and converted per axis, which is why this needs the aspect ratio
 * and cannot be a static table.
 */
std::vector<PadControl> default_pad_layout(int width, int height);

/**
 * The on-screen pad.
 *
 * Holds the layout and which controls are currently held. It does not know
 * about scancodes or the emulator: it reports a button bitmask and the caller
 * decides what that means.
 */
class VirtualPad {
public:
    VirtualPad();

    /** Saved user preference: whether the glass controls are wanted at all. */
    void set_enabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }

    /** A gamepad appearing is a good reason to get the controls out of the way,
     *  and its disappearing a good reason to bring them back. Kept separate
     *  from the preference so plugging one in never overwrites what the user
     *  chose. */
    void set_gamepad_present(bool present) { gamepad_present_ = present; }
    bool gamepad_present() const { return gamepad_present_; }

    /** Drawn only when wanted AND there is nothing better to play with. */
    bool visible() const { return enabled_ && !gamepad_present_; }

    /** Layout mode: dragging moves a control instead of pressing it. */
    void set_editing(bool on) { editing_ = on; }
    bool editing() const { return editing_; }

    void reset_layout(int w, int h);
    const std::vector<PadControl> &controls() const { return controls_; }
    void set_controls(const std::vector<PadControl> &c) { controls_ = c; }

    /** Feed one SDL event. Returns true when the event was consumed, so the
     *  caller does not also treat it as a click on the game. */
    bool handle_event(const SDL_Event &ev, int w, int h);

    /** Buttons currently held on the GLASS only, as a PAD_* bitmask. */
    unsigned held() const { return held_; }

    /** Draw into ImGui's foreground list. */
    void draw(int w, int h) const;

private:
    int   control_at(float px, float py, int w, int h) const;
    float radius_px(const PadControl &c, int w, int h) const;
    void  rebuild_held();

    std::vector<PadControl> controls_;
    bool     enabled_        = false;
    bool     editing_        = false;
    bool     gamepad_present_= false;
    unsigned held_           = 0;

    /* Which control each finger holds. A player presses two buttons at once,
     * and tracking per finger is what makes that work rather than the most
     * recent touch winning. */
    struct Touch {
        long long finger = 0;
        int       index  = -1;
    };
    std::vector<Touch> active_;
};

} /* namespace retrodos */

#endif /* RETRODOS_PAD_H */
