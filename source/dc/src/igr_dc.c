/**
 * igr_dc.c — In-game reset (soft reset) for the Dreamcast build.
 *
 * Implements the Sega Dreamcast Software Creation Standards soft-reset rules;
 * see igr.h for the summary. Two outcomes:
 *
 *   Title loop -> IGR_ExitToBios(): stop audio and networking, then exit with
 *   the KOS exit path set to the Boot ROM menu. The old quit hotkey called
 *   exit() on the default ARCH_EXIT_RETURN path, which jumps back to whatever
 *   loaded the program; booted from disc there is nothing to return to, so
 *   the console crashed instead of reaching the BIOS. Under dcload (ip or
 *   serial) the path stays ARCH_EXIT_RETURN, the KOS default, so the program
 *   hands back to dcload as normal.
 *
 *   Main game  -> longjmp to main(), which tidies up the race/menu state and
 *   restarts the title sequence. All screen loops call platform_pump_events()
 *   at the top of their frame, so the jump always leaves from a frame
 *   boundary rather than from the middle of a load or a VMU write.
 */

#include <kos.h>
#include <arch/arch.h>
#include <arch/timer.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/maple/keyboard.h>
#include <dc/cdrom.h>
#include <dc/fs_dcload.h>
#include <stdlib.h>

#include "igr.h"

extern void StopCD(void);
extern void SFX_StopAll(void);
extern void StopAmbientSounds(void);
extern void CloseCDDevice(void);
extern void CloseDirectPlaySession(void);

jmp_buf g_igrJmpBuf;

static int s_jumpReady = 0;
static int s_inTitleLoop = 1;     /* boot starts in the title loop */

/* Disc-lid watch. Only a change from "lid shut" to "lid open" counts, so a
 * session started with the lid already open does not bounce straight to the
 * BIOS. */
#define LID_POLL_MS 500
static uint64_t s_lidNextPollMs = 0;
static int s_lidSeenClosed = 0;

void IGR_SetJumpReady(void)
{
    s_jumpReady = 1;
}

void IGR_SetTitleLoop(int inTitleLoop)
{
    s_inTitleLoop = inTitleLoop ? 1 : 0;
}

#define IGR_PAD_COMBO (CONT_A | CONT_B | CONT_X | CONT_Y | CONT_START)

/* Any extra buttons held alongside the combo are ignored (4.1.2,
 * "Recommended"). Every connected controller is checked (4.1.3); arcade
 * sticks and racing controllers report through the same controller function
 * and use the same combo (18.3.2, 18.4.2). */
static int pad_combo_held(void)
{
    maple_device_t *dev;
    for (int i = 0; (dev = maple_enum_type(i, MAPLE_FUNC_CONTROLLER)) != NULL; i++) {
        cont_state_t *cs = (cont_state_t *)maple_dev_status(dev);
        if (cs && (cs->buttons & IGR_PAD_COMBO) == IGR_PAD_COMBO) {
            return 1;
        }
    }
    return 0;
}

/* Ctrl+Alt+Del on a Dreamcast keyboard (18.2.2). */
static int kbd_combo_held(void)
{
    maple_device_t *dev;
    for (int i = 0; (dev = maple_enum_type(i, MAPLE_FUNC_KEYBOARD)) != NULL; i++) {
        kbd_state_t *ks = (kbd_state_t *)maple_dev_status(dev);
        if (!ks) {
            continue;
        }
        uint8_t mods = ks->last_modifiers.raw;
        if ((mods & KBD_MOD_CTRL) && (mods & KBD_MOD_ALT) &&
            ks->key_states[KBD_KEY_DEL].is_down) {
            return 1;
        }
    }
    return 0;
}

int IGR_ComboHeld(void)
{
    return pad_combo_held() || kbd_combo_held();
}

/* Combo buttons still held as the title comes back up would be read as fresh
 * presses: START skips past it, A+B starts the attract demo. Wait for every
 * one of them to be released first. */
static void wait_for_release(void)
{
    for (;;) {
        int held = 0;
        maple_device_t *dev;
        for (int i = 0; (dev = maple_enum_type(i, MAPLE_FUNC_CONTROLLER)) != NULL; i++) {
            cont_state_t *cs = (cont_state_t *)maple_dev_status(dev);
            if (cs && (cs->buttons & IGR_PAD_COMBO)) {
                held = 1;
            }
        }
        for (int i = 0; (dev = maple_enum_type(i, MAPLE_FUNC_KEYBOARD)) != NULL; i++) {
            kbd_state_t *ks = (kbd_state_t *)maple_dev_status(dev);
            if (ks && ks->key_states[KBD_KEY_DEL].is_down) {
                held = 1;
            }
        }
        if (!held) {
            return;
        }
        thd_sleep(16);
    }
}

void IGR_ExitToBios(void)
{
    /* Quiet the hardware before KOS shuts down: no music stream thread
     * reading the disc, no SFX voices, no network receive thread. */
    StopCD();
    SFX_StopAll();
    CloseDirectPlaySession();
    CloseCDDevice();

    arch_set_exit_path(dcload_type == DCLOAD_TYPE_NONE ? ARCH_EXIT_MENU
                                                       : ARCH_EXIT_RETURN);
    /* exit() runs the atexit hook that saves the option settings, then KOS
     * shuts down and takes the exit path chosen above. */
    exit(0);
}

static void reset_to_title(void)
{
    StopCD();
    StopAmbientSounds();    /* looping SFX hold their channels; SFX_StopAll skips those */
    SFX_StopAll();
    wait_for_release();
    longjmp(g_igrJmpBuf, 1);
}

/* Disc lid opened -> Boot ROM menu within 10 seconds (4.2.1). Retail-boot
 * behaviour only: under dcload the drive is not where the program came from,
 * so the lid is left alone, as with any other dcload program. */
static int lid_opened(void)
{
    if (dcload_type != DCLOAD_TYPE_NONE) {
        return 0;
    }

    uint64_t now = timer_ms_gettime64();
    if (now < s_lidNextPollMs) {
        return 0;
    }
    s_lidNextPollMs = now + LID_POLL_MS;

    int status = 0;
    if (cdrom_get_status(&status, NULL) != ERR_OK) {
        return 0;
    }
    if (status == CD_STATUS_OPEN) {
        return s_lidSeenClosed;
    }
    s_lidSeenClosed = 1;
    return 0;
}

void IGR_Poll(void)
{
    if (lid_opened()) {
        IGR_ExitToBios();
    }

    if (!IGR_ComboHeld()) {
        return;
    }

    if (s_inTitleLoop || !s_jumpReady) {
        IGR_ExitToBios();
    }
    reset_to_title();
}
