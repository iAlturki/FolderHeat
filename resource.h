// SPDX-License-Identifier: CC-BY-NC-SA-4.0
// Copyright (c) 2026 iAlturki
#ifndef FOLDERHEAT_RESOURCE_H
#define FOLDERHEAT_RESOURCE_H

/* Resource ids */
#define IDI_APPICON                 1

/* Private window messages (must be in WM_APP range) */
#define WM_TRAYICON                 (WM_APP + 1)
#define WM_APP_PING                 (WM_APP + 2)   /* a second copy was started */

/* Menu command ids */
#define IDM_HEADER                  100   /* disabled status line */
#define IDM_PAUSE                   101
#define IDM_COLORS                  102
#define IDM_AUTOSTART               103
#define IDM_EXCLUDE                 104
#define IDM_CLEAR                   105
#define IDM_ABOUT                   106
#define IDM_EXIT                    107
#define IDM_EMPTY                   108   /* "nothing yet" placeholder */

/* Cooling speed radio group (id = base + half-life in days) */
#define IDM_SPEED_BASE              1000
/* Hottest folders list (id = base + row) */
#define IDM_HOT_BASE                2000

#endif /* FOLDERHEAT_RESOURCE_H */
