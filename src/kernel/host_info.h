/**
 * What the game runs on: Windows, or Wine / Proton (and a Steam Deck).
 *
 * Wine is found once through ntdll's wine_get_version export (Wine's own
 * ntdll exports it; Windows' does not). Pieces that behave differently
 * under Wine ask xbox_HostIsWine(): the log file (a Wine process always has
 * a valid stdout), the VSync watcher (DXVK emulates WaitForVBlank).
 *
 * RECOMP_FAKE_WINE=1 makes xbox_HostIsWine() say yes on Windows, so the
 * Wine-only paths can be tried there.
 */
#ifndef XBOX_HOST_INFO_H
#define XBOX_HOST_INFO_H

#ifdef __cplusplus
extern "C" {
#endif

/* 1 under Wine or Proton (or RECOMP_FAKE_WINE=1), else 0. */
int xbox_HostIsWine(void);

/* Wine's version ("9.21 (Staging)"), "" when not under Wine. */
const char *xbox_HostWineVersion(void);

/* 1 when the environment says SteamDeck=1 (Steam sets it on the Deck, in
 * Game Mode and Desktop Mode). */
int xbox_HostIsSteamDeck(void);

/* One "[HOST] ..." line on stdout: the system, Wine and its host system if
 * any, SteamDeck=1, logical processors. */
void xbox_HostLogLine(void);

#ifdef __cplusplus
}
#endif

#endif
