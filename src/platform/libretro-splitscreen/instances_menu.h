/* In-core boot menu (see instances_menu.c): rendered through the normal
 * video callback before sessions load, driven by Player 1's pad. */
#ifndef INSTANCES_MENU_H
#define INSTANCES_MENU_H

#include <stdbool.h>
#include <stdint.h>

struct sp_video;

/* Arm the menu. `defaultPlayers` (2-4) is the initially highlighted choice
 * (the persisted Players per ROM core option). */
void spMenuStart(int defaultPlayers);

/* True while the boot menu is showing. */
bool spMenuActive(void);

/* Advance the menu one frame: read P1 input, redraw, and flip to confirmed
 * on A/Start. Draws through the same sp_video the game frames use. */
void spMenuFrame(struct sp_video* vid, void (*poll)(void), uint32_t (*readKeys)(unsigned port));

/* One-shot: consumes the confirmation. True exactly once after A/Start. */
bool spMenuTakeConfirmed(void);

/* Chosen player count (2-4); valid at confirmation time. */
int spMenuSelection(void);

#endif
