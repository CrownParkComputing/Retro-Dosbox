/* retro-dosbox — on-screen keyboard. See retrodos_osk.cpp for why the
 * modifiers are sticky rather than held. */
#ifndef RETRODOS_OSK_H
#define RETRODOS_OSK_H

namespace retrodos {

/* Draws a keyboard docked to the bottom of the screen. Call between
 * ImGui::NewFrame() and ImGui::Render(). Keys are delivered as SDL scancodes
 * through retrodos_host_send_key, so DOS sees ordinary key presses. */
void osk_draw(float screen_w, float screen_h);

/* Must be called every frame, whether or not the keyboard is visible: a key is
 * held for a short interval after it is pressed, and its release is issued from
 * here. */
void osk_update(void);

/* Ctrl+Alt+Del as one action -- too many DOS installers need it. */
void osk_send_ctrl_alt_del(void);

/*
 * Type [text] into the guest, one key at a time.
 *
 * Queued rather than sent at once: the guest is a PC being driven by a
 * keyboard controller, and a burst of presses in a single moment of guest time
 * is read as one keystroke or none. osk_update() releases the queue at the
 * same pace a person types.
 *
 * Characters with no key on a US layout are skipped rather than approximated.
 * Returns how many will actually be sent.
 */
int osk_type_text(const char *text);

/* How many characters are still waiting, so the UI can say so. */
int osk_type_pending(void);

} /* namespace retrodos */

#endif
