/* SPDX-License-Identifier: MIT */
#ifndef HAX_TERMINAL_ANSI_H
#define HAX_TERMINAL_ANSI_H

/* String-literal terminal controls, suitable for compile-time concatenation. */
#define ANSI_ESC               "\x1b"
#define ANSI_BEL               "\x07"
#define ANSI_STRING_TERMINATOR ANSI_ESC "\\"
#define ANSI_CSI               ANSI_ESC "["

/* The extra ESC before the inner sequence is tmux's quoting convention. These wrappers support an
 * inner sequence whose only ESC is its leading byte. */
#define ANSI_TMUX_PASSTHROUGH_BEGIN ANSI_ESC "Ptmux;" ANSI_ESC
#define ANSI_TMUX_PASSTHROUGH_END   ANSI_STRING_TERMINATOR

/* ANSI_RESET clears all SGR state. SGR 22 restores normal intensity, closing bold and dim. */
#define ANSI_RESET      ANSI_CSI "0m"
#define ANSI_BOLD       ANSI_CSI "1m"
#define ANSI_DIM        ANSI_CSI "2m"
#define ANSI_ITALIC     ANSI_CSI "3m"
#define ANSI_BOLD_OFF   ANSI_CSI "22m"
#define ANSI_ITALIC_OFF ANSI_CSI "23m"

#define ANSI_UNDERLINE     ANSI_CSI "4m"
#define ANSI_UNDERLINE_OFF ANSI_CSI "24m"

/* Production rendering should normally use semantic theme roles instead of raw palette colors. */
#define ANSI_RED            ANSI_CSI "31m"
#define ANSI_GREEN          ANSI_CSI "32m"
#define ANSI_YELLOW         ANSI_CSI "33m"
#define ANSI_CYAN           ANSI_CSI "36m"
#define ANSI_BRIGHT_MAGENTA ANSI_CSI "95m"
#define ANSI_FG_DEFAULT     ANSI_CSI "39m"

#define ANSI_ERASE_LINE   ANSI_CSI "K"  /* cursor through line end (EL 0) */
#define ANSI_ERASE_BELOW  ANSI_CSI "J"  /* cursor through screen end (ED 0) */
#define ANSI_ERASE_SCREEN ANSI_CSI "2J" /* entire screen (ED 2) */
#define ANSI_CURSOR_HOME  ANSI_CSI "H"

#define ANSI_BRACKETED_PASTE_ENABLE  ANSI_CSI "?2004h"
#define ANSI_BRACKETED_PASTE_DISABLE ANSI_CSI "?2004l"

/* DEC mode 2026 presents output between BEGIN and END as one frame. Unsupported terminals ignore
 * it; tty restoration must emit END in case output was interrupted mid-frame. */
#define ANSI_SYNC_BEGIN ANSI_CSI "?2026h"
#define ANSI_SYNC_END   ANSI_CSI "?2026l"

/* DECTCEM cursor visibility; tty restoration must always emit SHOW. */
#define ANSI_CURSOR_HIDE ANSI_CSI "?25l"
#define ANSI_CURSOR_SHOW ANSI_CSI "?25h"

#endif /* HAX_TERMINAL_ANSI_H */
