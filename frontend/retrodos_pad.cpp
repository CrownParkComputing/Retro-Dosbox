/*
 * retro-dosbox — virtual pad implementation.
 */
#include "retrodos_pad.h"

#include "imgui.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

namespace retrodos {

void pad_button_colour(int b, float *rgb)
{
    switch (b) {
    case PAD_A: rgb[0] = 0.24f; rgb[1] = 0.72f; rgb[2] = 0.25f; break;  /* green  */
    case PAD_B: rgb[0] = 0.83f; rgb[1] = 0.19f; rgb[2] = 0.17f; break;  /* red    */
    case PAD_X: rgb[0] = 0.16f; rgb[1] = 0.44f; rgb[2] = 0.86f; break;  /* blue   */
    case PAD_Y: rgb[0] = 0.93f; rgb[1] = 0.76f; rgb[2] = 0.13f; break;  /* yellow */
    default:    rgb[0] = rgb[1] = rgb[2] = 1.0f;                break;
    }
}

const char *pad_button_name(int b)
{
    switch (b) {
    case PAD_UP:     return "Up";
    case PAD_DOWN:   return "Down";
    case PAD_LEFT:   return "Left";
    case PAD_RIGHT:  return "Right";
    case PAD_A:      return "A";
    case PAD_B:      return "B";
    case PAD_X:      return "X";
    case PAD_Y:      return "Y";
    case PAD_L:      return "LB";
    case PAD_R:      return "RB";
    case PAD_LT:     return "LT";
    case PAD_RT:     return "RT";
    case PAD_START:  return "Start";
    case PAD_SELECT: return "Select";
    case PAD_L3:     return "L3";
    case PAD_R3:     return "R3";
    default:         return "?";
    }
}

std::vector<PadControl> default_pad_layout(int width, int height)
{
    std::vector<PadControl> out;
    if (width <= 0 || height <= 0) return out;

    /* Offsets are given in units of the SMALLER screen dimension and converted
     * per axis. Using the same fraction for x and y would stretch every cluster
     * horizontally -- a d-pad would come out as a wide diamond. */
    const float smaller = (float)std::min(width, height);
    const float ux = smaller / (float)width;    /* one unit, as a width fraction  */
    const float uy = smaller / (float)height;   /* one unit, as a height fraction */

    const float r    = 0.062f;                  /* control radius, in units */
    const float step = 0.150f;                  /* cluster spacing, in units */

    auto add = [&](int button, const char *label, float cx, float cy,
                   float ox, float oy) {
        PadControl c;
        c.button = button;
        c.label  = label;
        c.x      = cx + ox * ux;
        c.y      = cy + oy * uy;
        c.radius = r;
        out.push_back(c);
    };

    /* Cluster centres are kept well inside the edges: placing them by eye put
     * the outermost controls half off the screen on a tall display. */
    const float dpad_cx = 0.13f, dpad_cy = 0.68f;
    const float face_cx = 0.87f, face_cy = 0.68f;

    add(PAD_UP,    "^", dpad_cx, dpad_cy,  0.0f, -step);
    add(PAD_DOWN,  "v", dpad_cx, dpad_cy,  0.0f,  step);
    add(PAD_LEFT,  "<", dpad_cx, dpad_cy, -step,  0.0f);
    add(PAD_RIGHT, ">", dpad_cx, dpad_cy,  step,  0.0f);

    /* A is where the thumb rests: nearest the player, bottom of the cluster.
     * In DOS terms that is usually fire. */
    add(PAD_A, "A", face_cx, face_cy,  0.0f,  step);
    add(PAD_B, "B", face_cx, face_cy,  step,  0.0f);
    add(PAD_X, "X", face_cx, face_cy, -step,  0.0f);
    add(PAD_Y, "Y", face_cx, face_cy,  0.0f, -step);

    /* Shoulders above triggers on each side, the way they sit on the pad. */
    add(PAD_L,  "LB", 0.10f, 0.30f, 0.0f, 0.0f);
    add(PAD_R,  "RB", 0.90f, 0.30f, 0.0f, 0.0f);
    add(PAD_LT, "LT", 0.10f, 0.13f, 0.0f, 0.0f);
    add(PAD_RT, "RT", 0.90f, 0.13f, 0.0f, 0.0f);

    /* Smaller and centred low: pressed between rounds, not during play. */
    PadControl sel;
    sel.button = PAD_SELECT; sel.label = "Esc";
    sel.x = 0.44f; sel.y = 0.93f; sel.radius = r * 0.72f;
    out.push_back(sel);

    PadControl start;
    start.button = PAD_START; start.label = "Ent";
    start.x = 0.56f; start.y = 0.93f; start.radius = r * 0.72f;
    out.push_back(start);

    return out;
}

VirtualPad::VirtualPad() {}

void VirtualPad::reset_layout(int w, int h)
{
    controls_ = default_pad_layout(w, h);
    active_.clear();
    held_ = 0;
}

float VirtualPad::radius_px(const PadControl &c, int w, int h) const
{
    return c.radius * (float)std::min(w, h);
}

int VirtualPad::control_at(float px, float py, int w, int h) const
{
    /* Nearest hit wins rather than the first: the clusters are close enough
     * that overlapping touch areas are normal, and picking the first in the
     * list would make one neighbour permanently shadow another. */
    int   best = -1;
    float best_d2 = 0.0f;
    for (size_t i = 0; i < controls_.size(); ++i) {
        const PadControl &c = controls_[i];
        const float cx = c.x * (float)w, cy = c.y * (float)h;
        const float r  = radius_px(c, w, h);
        const float dx = px - cx, dy = py - cy;
        const float d2 = dx * dx + dy * dy;
        if (d2 <= r * r && (best < 0 || d2 < best_d2)) { best = (int)i; best_d2 = d2; }
    }
    return best;
}

void VirtualPad::rebuild_held()
{
    held_ = 0;
    /* While the layout is being edited a touch is a drag, not a press --
     * otherwise moving the fire button would fire. */
    if (editing_) return;
    for (const Touch &t : active_)
        if (t.index >= 0 && t.index < (int)controls_.size())
            held_ |= 1u << controls_[t.index].button;
}

bool VirtualPad::handle_event(const SDL_Event &ev, int w, int h)
{
    if (!visible() || w <= 0 || h <= 0) return false;

    /* Touch coordinates arrive normalised; the layout is in fractions too, but
     * hit testing is done in pixels so a control stays round on a non-square
     * screen. */
    const float px = ev.tfinger.x * (float)w;
    const float py = ev.tfinger.y * (float)h;

    switch (ev.type) {
    case SDL_EVENT_FINGER_DOWN: {
        const int idx = control_at(px, py, w, h);
        if (idx < 0) return false;
        Touch t;
        t.finger = (long long)ev.tfinger.fingerID;
        t.index  = idx;
        active_.push_back(t);
        rebuild_held();
        return true;
    }

    case SDL_EVENT_FINGER_UP: {
        const long long id = (long long)ev.tfinger.fingerID;
        const size_t before = active_.size();
        active_.erase(std::remove_if(active_.begin(), active_.end(),
                                     [id](const Touch &t) { return t.finger == id; }),
                      active_.end());
        if (active_.size() == before) return false;
        rebuild_held();
        return true;
    }

    case SDL_EVENT_FINGER_MOTION: {
        const long long id = (long long)ev.tfinger.fingerID;
        for (Touch &t : active_) {
            if (t.finger != id) continue;

            if (editing_) {
                /* Drag to reposition. Stored as fractions, so the move survives
                 * a rotation or a different screen. */
                if (t.index >= 0 && t.index < (int)controls_.size()) {
                    PadControl &c = controls_[(size_t)t.index];
                    c.x = std::min(0.98f, std::max(0.02f, px / (float)w));
                    c.y = std::min(0.98f, std::max(0.02f, py / (float)h));
                }
                return true;
            }

            /* Sliding off a button releases it and sliding onto another presses
             * it, which is how a thumb rolling across a d-pad is meant to
             * behave -- without this, diagonal movement is impossible. */
            const int idx = control_at(px, py, w, h);
            if (idx != t.index) { t.index = idx; rebuild_held(); }
            return true;
        }
        return false;
    }

    default:
        return false;
    }
}

void VirtualPad::draw(int w, int h) const
{
    if (!visible() || w <= 0 || h <= 0) return;

    ImDrawList *dl = ImGui::GetForegroundDrawList();

    /* Deliberately faint. These sit on top of the game, and a solid pad hides
     * more of a 320x200 picture than it is worth. */
    const float alpha_fill   = editing_ ? 0.40f : 0.22f;
    const float alpha_border  = editing_ ? 0.95f : 0.55f;

    for (const PadControl &c : controls_) {
        const ImVec2 centre(c.x * (float)w, c.y * (float)h);
        const float  r = radius_px(c, w, h);
        const bool   on = (held_ & (1u << c.button)) != 0;

        float rgb[3];
        pad_button_colour(c.button, rgb);
        const ImU32 fill = ImGui::GetColorU32(
            ImVec4(rgb[0], rgb[1], rgb[2], on ? 0.70f : alpha_fill));
        const ImU32 edge = ImGui::GetColorU32(
            ImVec4(rgb[0], rgb[1], rgb[2], alpha_border));

        dl->AddCircleFilled(centre, r, fill, 32);
        dl->AddCircle(centre, r, edge, 32, editing_ ? 3.0f : 2.0f);

        if (!c.label.empty()) {
            const ImVec2 ts = ImGui::CalcTextSize(c.label.c_str());
            dl->AddText(ImVec2(centre.x - ts.x * 0.5f, centre.y - ts.y * 0.5f),
                        ImGui::GetColorU32(ImVec4(1, 1, 1, 0.95f)), c.label.c_str());
        }
    }
}

} /* namespace retrodos */
