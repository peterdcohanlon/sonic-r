/**
 * igr.h — In-game reset (soft reset), Dreamcast only.
 *
 * Sega Dreamcast Software Creation Standards, chapter 4:
 *   - A+B+X+Y held, then START, on any controller in use (4.1.2, 4.1.3).
 *     Ctrl+Alt+Del on a keyboard (18.2).
 *   - From the title loop (logos, title screen, attract demo) the reset
 *     leaves to the Boot ROM menu (4.1.4.1).
 *   - From the main game (anything after START on the title screen) it
 *     returns to the title screen (4.1.4.1).
 *   - Opening the disc lid leaves to the Boot ROM menu (4.2.1).
 *
 * Detection runs inside platform_pump_events(), which every screen loop calls
 * at the top of each frame. VMU saves are synchronous and never pump events,
 * so a reset can never land in the middle of a save (15.4).
 *
 * On the desktop build these are no-ops.
 */
#ifndef SONICR_IGR_H
#define SONICR_IGR_H

#ifdef SONICR_DC
#include <setjmp.h>

/* Jump target for a reset back to the title. main() setjmps on it just ahead
 * of the title sequence and then calls IGR_SetJumpReady(). */
extern jmp_buf g_igrJmpBuf;
void IGR_SetJumpReady(void);

/* 1 while in the title loop (logos, title screen, attract demo), 0 once the
 * player has pressed START and the main game has begun. */
void IGR_SetTitleLoop(int inTitleLoop);

/* Per-frame check, called from platform_pump_events(). May not return. */
void IGR_Poll(void);

/* 1 if any controller holds A+B+X+Y+START, or a keyboard holds Ctrl+Alt+Del. */
int IGR_ComboHeld(void);

/* Leave to the Boot ROM menu (or back to dcload when running under it).
 * Stops audio and networking first. Does not return. */
void IGR_ExitToBios(void);
#else
#define IGR_SetTitleLoop(inTitleLoop) ((void)0)
#endif

#endif /* SONICR_IGR_H */
