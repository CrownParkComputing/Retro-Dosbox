/*
 * retro-dosbox — on-screen keyboard.
 *
 * A handheld has no keyboard, and DOS games are full of titles that cannot be
 * started, configured or quit without one. This is not a text-entry widget:
 * DOS reads the keyboard at the SCANCODE level, so every key here sends a
 * scancode down and up through retrodos_host_send_key, exactly as a real key
 * would arrive.
 *
 * Modifiers are STICKY rather than held. You cannot press Ctrl and C at once
 * with one finger, so Ctrl latches, applies to the next key, and releases
 * itself -- which is what makes Ctrl+C, Alt+X and the Ctrl+Alt+Del of a
 * hundred install programs reachable at all.
 */
#include "retrodos_osk.h"

#include <deque>

#include "imgui.h"
#include "retrodos_host.h"

#include <SDL3/SDL.h>
#if defined(__ANDROID__)
#include <android/log.h>
#define OSKLOG(...) __android_log_print(ANDROID_LOG_INFO, "retrodos", __VA_ARGS__)
#else
#define OSKLOG(...) SDL_Log(__VA_ARGS__)
#endif

namespace retrodos {
namespace {

/* Share of screen height the keyboard may take. It spans the full width, so
 * this only decides how tall the keys are -- enough for a comfortable target
 * while still leaving most of the DOS picture visible above it. */
const float kHeightShare = 0.42f;

struct Key {
    const char *label;
    int         scancode;
    float       width;   /* in units of one standard key */
};

/* Compact layout: a full 104-key board is unusable at thumb size, so this is
 * the set DOS software actually needs -- letters, digits, the punctuation DOS
 * paths use, function keys, and the editing/arrow cluster. */
const Key kRow0[] = {
    {"Esc", SDL_SCANCODE_ESCAPE, 1.4f},
    {"F1", SDL_SCANCODE_F1, 1}, {"F2", SDL_SCANCODE_F2, 1},
    {"F3", SDL_SCANCODE_F3, 1}, {"F4", SDL_SCANCODE_F4, 1},
    {"F5", SDL_SCANCODE_F5, 1}, {"F6", SDL_SCANCODE_F6, 1},
    {"F7", SDL_SCANCODE_F7, 1}, {"F8", SDL_SCANCODE_F8, 1},
    {"F9", SDL_SCANCODE_F9, 1}, {"F10", SDL_SCANCODE_F10, 1},
    {"F11", SDL_SCANCODE_F11, 1}, {"F12", SDL_SCANCODE_F12, 1},
};
const Key kRow1[] = {
    {"1", SDL_SCANCODE_1, 1}, {"2", SDL_SCANCODE_2, 1}, {"3", SDL_SCANCODE_3, 1},
    {"4", SDL_SCANCODE_4, 1}, {"5", SDL_SCANCODE_5, 1}, {"6", SDL_SCANCODE_6, 1},
    {"7", SDL_SCANCODE_7, 1}, {"8", SDL_SCANCODE_8, 1}, {"9", SDL_SCANCODE_9, 1},
    {"0", SDL_SCANCODE_0, 1}, {"-", SDL_SCANCODE_MINUS, 1},
    {"=", SDL_SCANCODE_EQUALS, 1}, {"Bksp", SDL_SCANCODE_BACKSPACE, 1.6f},
};
const Key kRow2[] = {
    {"Tab", SDL_SCANCODE_TAB, 1.4f},
    {"Q", SDL_SCANCODE_Q, 1}, {"W", SDL_SCANCODE_W, 1}, {"E", SDL_SCANCODE_E, 1},
    {"R", SDL_SCANCODE_R, 1}, {"T", SDL_SCANCODE_T, 1}, {"Y", SDL_SCANCODE_Y, 1},
    {"U", SDL_SCANCODE_U, 1}, {"I", SDL_SCANCODE_I, 1}, {"O", SDL_SCANCODE_O, 1},
    {"P", SDL_SCANCODE_P, 1}, {"[", SDL_SCANCODE_LEFTBRACKET, 1},
    {"]", SDL_SCANCODE_RIGHTBRACKET, 1},
};
const Key kRow3[] = {
    {"Caps", SDL_SCANCODE_CAPSLOCK, 1.6f},
    {"A", SDL_SCANCODE_A, 1}, {"S", SDL_SCANCODE_S, 1}, {"D", SDL_SCANCODE_D, 1},
    {"F", SDL_SCANCODE_F, 1}, {"G", SDL_SCANCODE_G, 1}, {"H", SDL_SCANCODE_H, 1},
    {"J", SDL_SCANCODE_J, 1}, {"K", SDL_SCANCODE_K, 1}, {"L", SDL_SCANCODE_L, 1},
    {";", SDL_SCANCODE_SEMICOLON, 1}, {"'", SDL_SCANCODE_APOSTROPHE, 1},
    {"Enter", SDL_SCANCODE_RETURN, 1.8f},
};
const Key kRow4[] = {
    {"Shift", SDL_SCANCODE_LSHIFT, 2.0f},
    {"Z", SDL_SCANCODE_Z, 1}, {"X", SDL_SCANCODE_X, 1}, {"C", SDL_SCANCODE_C, 1},
    {"V", SDL_SCANCODE_V, 1}, {"B", SDL_SCANCODE_B, 1}, {"N", SDL_SCANCODE_N, 1},
    {"M", SDL_SCANCODE_M, 1}, {",", SDL_SCANCODE_COMMA, 1},
    {".", SDL_SCANCODE_PERIOD, 1}, {"/", SDL_SCANCODE_SLASH, 1},
    {"\\", SDL_SCANCODE_BACKSLASH, 1},
};
const Key kRow5[] = {
    {"Ctrl", SDL_SCANCODE_LCTRL, 1.6f},
    {"Alt", SDL_SCANCODE_LALT, 1.4f},
    {"Space", SDL_SCANCODE_SPACE, 5.0f},
    {"<", SDL_SCANCODE_LEFT, 1}, {"^", SDL_SCANCODE_UP, 1},
    {"v", SDL_SCANCODE_DOWN, 1}, {">", SDL_SCANCODE_RIGHT, 1},
};

bool g_shift = false, g_ctrl = false, g_alt = false;

bool is_modifier(int sc)
{
    return sc == SDL_SCANCODE_LSHIFT || sc == SDL_SCANCODE_LCTRL ||
           sc == SDL_SCANCODE_LALT;
}

/* A key that has been pressed and is waiting to be released.
 *
 * Press and release CANNOT be sent together. Both would be drained by the same
 * pump and reach the emulated keyboard controller within the same instant of
 * guest time, and a DOS program that polls -- which is most of them, INT 16h in
 * a loop -- can run no code between the two and simply never observes the key.
 * A real key is held for tens of milliseconds; this reproduces that. */
struct Pending {
    int    scancode;
    Uint64 release_at;      /* ms, SDL_GetTicks clock */
    bool   shift, ctrl, alt;
};
Pending g_pending = { 0, 0, false, false, false };

/* Long enough that the guest polls at least once while the key is down, short
 * enough that it never reads as a repeat. */
const Uint64 kHoldMs = 60;

void tap(int scancode)
{
    /* One at a time: if a key is still held, release it before starting the
     * next, so a fast typist cannot leave one stuck down. */
    if (g_pending.scancode) {
        retrodos_host_send_key(g_pending.scancode, false);
        if (g_pending.shift) retrodos_host_send_key(SDL_SCANCODE_LSHIFT, false);
        if (g_pending.alt)   retrodos_host_send_key(SDL_SCANCODE_LALT,   false);
        if (g_pending.ctrl)  retrodos_host_send_key(SDL_SCANCODE_LCTRL,  false);
        g_pending.scancode = 0;
    }

    /* Modifiers first, so the guest sees them held when the key arrives. */
    if (g_ctrl)  retrodos_host_send_key(SDL_SCANCODE_LCTRL,  true);
    if (g_alt)   retrodos_host_send_key(SDL_SCANCODE_LALT,   true);
    if (g_shift) retrodos_host_send_key(SDL_SCANCODE_LSHIFT, true);

    OSKLOG("osk: tap scancode=%d", scancode);
    retrodos_host_send_key(scancode, true);

    g_pending.scancode   = scancode;
    g_pending.release_at = SDL_GetTicks() + kHoldMs;
    g_pending.shift = g_shift; g_pending.alt = g_alt; g_pending.ctrl = g_ctrl;

    /* The latch is spent the moment it is applied. */
    g_shift = g_alt = g_ctrl = false;
}

void draw_row(const Key *keys, int count, float unit_w, float unit_h, float gap)
{
    for (int i = 0; i < count; ++i) {
        if (i) ImGui::SameLine(0.0f, gap);
        const Key &k = keys[i];

        bool latched = false;
        if      (k.scancode == SDL_SCANCODE_LSHIFT) latched = g_shift;
        else if (k.scancode == SDL_SCANCODE_LCTRL)  latched = g_ctrl;
        else if (k.scancode == SDL_SCANCODE_LALT)   latched = g_alt;

        if (latched)
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));

        ImGui::PushID(k.scancode * 131 + i);
        ImGui::Button(k.label, ImVec2(unit_w * k.width, unit_h));
        if (k.scancode == SDL_SCANCODE_1 && ImGui::GetIO().MouseDown[0]) {
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            OSKLOG("osk: key'1' rect %.0f,%.0f - %.0f,%.0f  mouse %.0f,%.0f",
                   a.x, a.y, b.x, b.y,
                   ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y);
        }

        /* Fires on PRESS, not on release over the same key.
         *
         * A real key acts the moment it goes down, and a thumb that lands on a
         * key and slides slightly off before lifting still means to have
         * pressed it -- with release-based activation that input is simply
         * lost, which on a touchscreen happens constantly. */
        if (ImGui::IsItemActivated()) {
            if (is_modifier(k.scancode)) {
                /* Sticky: latch for the NEXT key rather than requiring two
                 * fingers, which a handheld cannot offer. */
                if      (k.scancode == SDL_SCANCODE_LSHIFT) g_shift = !g_shift;
                else if (k.scancode == SDL_SCANCODE_LCTRL)  g_ctrl  = !g_ctrl;
                else                                        g_alt   = !g_alt;
            } else {
                tap(k.scancode);
            }
        }
        ImGui::PopID();

        if (latched) ImGui::PopStyleColor();
    }
}

} /* namespace */

namespace {

/* US-layout character -> scancode, and whether shift is held.
 *
 * A table rather than SDL_GetScancodeFromKey because this has to describe the
 * GUEST's keyboard, not the host's: the emulated machine is a US PC whatever
 * the real keyboard is, and asking SDL would mean a host set to another layout
 * typed the wrong characters into DOS. */
bool scancode_for_char(char c, int &sc, bool &shift)
{
    shift = false;
    if (c >= 'a' && c <= 'z') { sc = SDL_SCANCODE_A + (c - 'a'); return true; }
    if (c >= 'A' && c <= 'Z') { sc = SDL_SCANCODE_A + (c - 'A'); shift = true; return true; }
    if (c == '0') { sc = SDL_SCANCODE_0; return true; }
    if (c >= '1' && c <= '9') { sc = SDL_SCANCODE_1 + (c - '1'); return true; }

    switch (c) {
    case ' ':  sc = SDL_SCANCODE_SPACE;        return true;
    case '\n': case '\r': sc = SDL_SCANCODE_RETURN; return true;
    case '\t': sc = SDL_SCANCODE_TAB;          return true;
    case '-':  sc = SDL_SCANCODE_MINUS;        return true;
    case '=':  sc = SDL_SCANCODE_EQUALS;       return true;
    case '[':  sc = SDL_SCANCODE_LEFTBRACKET;  return true;
    case ']':  sc = SDL_SCANCODE_RIGHTBRACKET; return true;
    case '\\': sc = SDL_SCANCODE_BACKSLASH;    return true;
    case ';':  sc = SDL_SCANCODE_SEMICOLON;    return true;
    case '\'': sc = SDL_SCANCODE_APOSTROPHE;   return true;
    case '`':  sc = SDL_SCANCODE_GRAVE;        return true;
    case ',':  sc = SDL_SCANCODE_COMMA;        return true;
    case '.':  sc = SDL_SCANCODE_PERIOD;       return true;
    case '/':  sc = SDL_SCANCODE_SLASH;        return true;
    /* Shifted pairs, in US-layout order. */
    case '!':  sc = SDL_SCANCODE_1; shift = true; return true;
    case '@':  sc = SDL_SCANCODE_2; shift = true; return true;
    case '#':  sc = SDL_SCANCODE_3; shift = true; return true;
    case '$':  sc = SDL_SCANCODE_4; shift = true; return true;
    case '%':  sc = SDL_SCANCODE_5; shift = true; return true;
    case '^':  sc = SDL_SCANCODE_6; shift = true; return true;
    case '&':  sc = SDL_SCANCODE_7; shift = true; return true;
    case '*':  sc = SDL_SCANCODE_8; shift = true; return true;
    case '(':  sc = SDL_SCANCODE_9; shift = true; return true;
    case ')':  sc = SDL_SCANCODE_0; shift = true; return true;
    case '_':  sc = SDL_SCANCODE_MINUS;        shift = true; return true;
    case '+':  sc = SDL_SCANCODE_EQUALS;       shift = true; return true;
    case '{':  sc = SDL_SCANCODE_LEFTBRACKET;  shift = true; return true;
    case '}':  sc = SDL_SCANCODE_RIGHTBRACKET; shift = true; return true;
    case '|':  sc = SDL_SCANCODE_BACKSLASH;    shift = true; return true;
    case ':':  sc = SDL_SCANCODE_SEMICOLON;    shift = true; return true;
    case '"':  sc = SDL_SCANCODE_APOSTROPHE;   shift = true; return true;
    case '~':  sc = SDL_SCANCODE_GRAVE;        shift = true; return true;
    case '<':  sc = SDL_SCANCODE_COMMA;        shift = true; return true;
    case '>':  sc = SDL_SCANCODE_PERIOD;       shift = true; return true;
    case '?':  sc = SDL_SCANCODE_SLASH;        shift = true; return true;
    default:   return false;
    }
}

struct Typed { int sc; bool shift; };
std::deque<Typed> g_typing;

} /* namespace */

void osk_update(void)
{
    /* Releases the key held by the last tap once it has been down long enough.
     * Called every frame, not only while the keyboard is drawn: hiding the
     * keyboard between the press and the release would otherwise leave the key
     * held down in the guest forever. */
    if (!g_pending.scancode) {
        /* Nothing held: start the next pasted character, if any.
         *
         * One per call, and only once the previous key has been released --
         * a paste driven any faster is delivered inside a single moment of
         * guest time and arrives as one keystroke or none. */
        if (!g_typing.empty()) {
            const Typed t = g_typing.front();
            g_typing.pop_front();
            const bool saved_shift = g_shift;
            g_shift = t.shift;
            tap(t.sc);
            g_shift = saved_shift;
        }
        return;
    }
    if (SDL_GetTicks() < g_pending.release_at) return;

    retrodos_host_send_key(g_pending.scancode, false);
    if (g_pending.shift) retrodos_host_send_key(SDL_SCANCODE_LSHIFT, false);
    if (g_pending.alt)   retrodos_host_send_key(SDL_SCANCODE_LALT,   false);
    if (g_pending.ctrl)  retrodos_host_send_key(SDL_SCANCODE_LCTRL,  false);
    g_pending.scancode = 0;
}

void osk_draw(float screen_w, float screen_h)
{
    /* Keys are RECTANGLES, not squares.
     *
     * Sizing everything from one unit forces a choice between a keyboard that
     * spans the screen and one that leaves the game visible: the layout is
     * ~14.5 wide by 6 tall, so a full-width board on 16:9 is about 70% of the
     * height, and shrinking it to fit a height budget leaves it under half the
     * screen wide -- a cramped island with a lot of empty space beside it.
     *
     * Real keyboards are not square-keyed either. Taking WIDTH from the screen
     * width and HEIGHT from a separate budget gives a board that spans the
     * display, stays about two fifths of its height, and has comfortably large
     * targets. */
    const float gap  = ImGui::GetStyle().ItemSpacing.x * 0.4f;
    const float pad  = ImGui::GetStyle().WindowPadding.y;

    const float unit_w = (screen_w * 0.985f - gap * 14.0f - pad * 2.0f) / 14.5f;
    const float unit_h = (screen_h * kHeightShare - gap * 5.0f - pad * 2.0f) / 6.0f;

    const float kb_w = unit_w * 14.5f + gap * 14.0f + pad * 2.0f;
    const float kb_h = unit_h * 6.0f  + gap * 5.0f  + pad * 2.0f;

    ImGui::SetNextWindowPos(ImVec2(screen_w * 0.5f, screen_h - pad), ImGuiCond_Always,
                            ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(kb_w, kb_h));
    ImGui::SetNextWindowBgAlpha(0.88f);   /* keep the game readable behind it */
    ImGui::Begin("##osk", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoNav);

    draw_row(kRow0, SDL_arraysize(kRow0), unit_w, unit_h, gap);
    draw_row(kRow1, SDL_arraysize(kRow1), unit_w, unit_h, gap);
    draw_row(kRow2, SDL_arraysize(kRow2), unit_w, unit_h, gap);
    draw_row(kRow3, SDL_arraysize(kRow3), unit_w, unit_h, gap);
    draw_row(kRow4, SDL_arraysize(kRow4), unit_w, unit_h, gap);
    draw_row(kRow5, SDL_arraysize(kRow5), unit_w, unit_h, gap);

    ImGui::End();
}

int osk_type_text(const char *text)
{
    if (!text) return 0;
    int n = 0;
    for (const char *p = text; *p; ++p) {
        int sc = 0; bool shift = false;
        if (!scancode_for_char(*p, sc, shift)) continue;   /* silently skipped */
        /* A cap, so a whole document pasted by accident does not hold the
         * guest's keyboard for several minutes with no way to stop it. */
        if (g_typing.size() >= 512) break;
        g_typing.push_back(Typed{ sc, shift });
        ++n;
    }
    return n;
}

int osk_type_pending(void)
{
    return (int)g_typing.size();
}

void osk_send_ctrl_alt_del(void)
{
    /* Routed through the same held-key path as any other key: sent as an
     * instantaneous press and release it would land in one moment of guest time
     * and the DOS shell would never see it. */
    g_ctrl = g_alt = true;
    tap(SDL_SCANCODE_DELETE);
}

} /* namespace retrodos */
