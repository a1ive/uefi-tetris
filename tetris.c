/*
 *  UEFI Tetris
 *  Copyright (C) 2013¨C2014, Curtis McEnroe programble@gmail.com
 *
 *  Permission to use, copy, modify, and/or distribute this software
 *  for any purpose with or without fee is hereby granted, provided 
 *  that the above copyright notice and this permission notice
 *  appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 *  WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 *  WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL
 *  THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR 
 *  CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 *  LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 *  NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 *  CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <efi.h>
#include <efilib.h>

EFI_SIMPLE_TEXT_OUT_PROTOCOL *ConOut = NULL;
EFI_SIMPLE_TEXT_IN_PROTOCOL *ConIn = NULL;

typedef UINT8 uint8_t;
typedef UINT16 uint16_t;
typedef UINT32 uint32_t;
typedef UINT64 uint64_t;
typedef UINTN uintn_t;
typedef INT8 int8_t;
typedef INT16 int16_t;
typedef INT32 int32_t;
typedef INT64 int64_t;
typedef CHAR16 char16_t;
typedef UINTN size_t;

/* Tetris well dimensions */
#define WELL_WIDTH  (10)
#define WELL_HEIGHT (22)
/* Initial interval in milliseconds at which to apply gravity */
#define INITIAL_SPEED (1000)
/* Delay in milliseconds before rows are cleared */
#define CLEAR_DELAY (100)
/* Scoring: score is increased by the product of the current level and a factor
 * corresponding to the number of rows cleared. */
#define SCORE_FACTOR_1 (100)
#define SCORE_FACTOR_2 (300)
#define SCORE_FACTOR_3 (500)
#define SCORE_FACTOR_4 (800)
/* Amount to increase the score for a soft drop */
#define SOFT_DROP_SCORE (1)
/* Factor by which to multiply the number of rows dropped to increase the score
 * for a hard drop */
#define HARD_DROP_SCORE_FACTOR (2)
/* Number of rows that need to be cleared to increase level */
#define ROWS_PER_LEVEL (10)

typedef enum bool {
    false,
    true
} bool;

static void *memcpy (void *dest, const void *src, size_t len)
{
    void *edi = dest;
    const void *esi = src;
    int discard_ecx;

    asm volatile ("rep movsl" : "=&D" (edi), "=&S" (esi), "=&c" (discard_ecx) : "0" (edi), "1" (esi), "2" (len >> 2) : "memory");
    asm volatile ("rep movsb" : "=&D" (edi), "=&S" (esi), "=&c" (discard_ecx) : "0" (edi), "1" (esi), "2" (len & 3) : "memory");
    return dest;
}

static void *memset (void *dest, int c, size_t len)
{
    void *edi = dest;
    int eax = c;
    int discard_ecx;

    eax |= ( eax << 8 );
    eax |= ( eax << 16 );

    asm volatile ("rep stosl" : "=&D" (edi), "=&a" (eax), "=&c" (discard_ecx) : "0" (edi), "1" (eax), "2" (len >> 2) : "memory");
    asm volatile ("rep stosb" : "=&D" (edi), "=&a" (eax), "=&c" (discard_ecx) : "0" (edi), "1" (eax), "2" (len & 3) : "memory" );
    return dest;
}

/* Port I/O */

static inline uint8_t inb(uint16_t p)
{
    uint8_t r;
    asm("inb %1, %0" : "=a" (r) : "dN" (p));
    return r;
}

static inline void outb(uint16_t p, uint8_t d)
{
    asm("outb %1, %0" : : "dN" (p), "a" (d));
}

/* Timing */

/* Return the number of CPU ticks since boot. */
static inline uint64_t rdtsc(void)
{
    uint32_t hi, lo;
    asm("rdtsc" : "=a" (lo), "=d" (hi));
    return ((uint64_t) lo) | (((uint64_t) hi) << 32);
}

/* Return the current second field of the real-time-clock (RTC). Note that the
 * value may or may not be represented in such a way that it should be
 * formatted in hex to display the current second (i.e. 0x30 for the 30th
 * second). */
static uint8_t rtcs(void)
{
    uint8_t last = 0, sec;
    do { /* until value is the same twice in a row */
        /* wait for update not in progress */
        do { outb(0x70, 0x0A); } while (inb(0x71) & 0x80);
        outb(0x70, 0x00);
        sec = inb(0x71);
    } while (sec != last && (last = sec));
    return sec;
}

/* The number of CPU ticks per millisecond */
uint64_t tpms;

/* Set tpms to the number of CPU ticks per millisecond based on the number of
 * ticks in the last second, if the RTC second has changed since the last call.
 * This gets called on every iteration of the main loop in order to provide
 * accurate timing. */
static void tps(void)
{
    static uint64_t ti = 0;
    static uint8_t last_sec = 0xFF;
    uint8_t sec = rtcs();
    if (sec != last_sec) {
        last_sec = sec;
        uint64_t tf = rdtsc();
        tpms = (uint32_t) ((tf - ti) >> 3) / 125; /* Less chance of truncation */
        ti = tf;
    }
}

/* IDs used to keep separate timing operations separate */
enum timer {
    TIMER_UPDATE,
    TIMER_CLEAR,
    TIMER__LENGTH
};

uint64_t timers[TIMER__LENGTH] = {0};

/* Return true if at least ms milliseconds have elapsed since the last call
 * that returned true for this timer. When called on each iteration of the main
 * loop, has the effect of returning true once every ms milliseconds. */
static bool interval(enum timer timer, uint32_t ms)
{
    uint64_t tf = rdtsc();
    if (tf - timers[timer] >= tpms * ms) {
        timers[timer] = tf;
        return true;
    } else return false;
}

/* Return true if at least ms milliseconds have elapsed since the first call
 * for this timer and reset the timer. */
static bool wait(enum timer timer, uint32_t ms)
{
    if (timers[timer]) {
        if (rdtsc() - timers[timer] >= tpms * ms) {
            timers[timer] = 0;
            return true;
        } else return false;
    } else {
        timers[timer] = rdtsc();
        return false;
    }
}

/* Video Output */

enum color {
    BLACK,
    BLUE,
    GREEN,
    CYAN,
    RED,
    MAGENTA,
    BROWN,
    GRAY,
    BRIGHT,
    WHITE
};

#define COLS (80)
#define ROWS (25)

#define TERM_BLACK        0x00
#define TERM_BLUE         0x01
#define TERM_GREEN        0x02
#define TERM_CYAN         0x03
#define TERM_RED          0x04
#define TERM_MAGENTA      0x05
#define TERM_BROWN        0x06
#define TERM_LIGHTGRAY    0x07
#define TERM_BRIGHT       0x08
#define TERM_DARKGRAY     0x08
#define TERM_LIGHTBLUE    0x09
#define TERM_LIGHTGREEN   0x0A
#define TERM_LIGHTCYAN    0x0B
#define TERM_LIGHTRED     0x0C
#define TERM_LIGHTMAGENTA 0x0D
#define TERM_YELLOW       0x0E
#define TERM_WHITE        0x0F

#define TERM_BACKGROUND_BLACK     0x00
#define TERM_BACKGROUND_BLUE      0x10
#define TERM_BACKGROUND_GREEN     0x20
#define TERM_BACKGROUND_CYAN      0x30
#define TERM_BACKGROUND_RED       0x40
#define TERM_BACKGROUND_MAGENTA   0x50
#define TERM_BACKGROUND_BROWN     0x60
#define TERM_BACKGROUND_LIGHTGRAY 0x70

#define TERM_TEXT_ATTR(fg, bg)    ((fg) | ((bg)))

uintn_t color_fg[10] = 
{
    TERM_BLACK,
    TERM_BLUE,
    TERM_GREEN,
    TERM_CYAN,
    TERM_RED,
    TERM_MAGENTA,
    TERM_BROWN,
    TERM_LIGHTGRAY,
    TERM_WHITE
};

uintn_t color_bg[10] = 
{
    TERM_BACKGROUND_BLACK,
    TERM_BACKGROUND_BLUE,
    TERM_BACKGROUND_GREEN,
    TERM_BACKGROUND_CYAN,
    TERM_BACKGROUND_RED,
    TERM_BACKGROUND_MAGENTA,
    TERM_BACKGROUND_BROWN,
    TERM_BACKGROUND_LIGHTGRAY,
    TERM_BACKGROUND_LIGHTGRAY,
    TERM_BACKGROUND_BLACK
};

/* Display a character at x, y in fg foreground color and bg background color.
 */

static void _putc(uint8_t x, uint8_t y, enum color fg, enum color bg, char c)
{
    char16_t str[2];
    str[0] = c;
    str[1] = 0;
    uefi_call_wrapper (ConOut->SetCursorPosition, 3, ConOut, x, y);
    uefi_call_wrapper (ConOut->SetAttribute, 2,
                       ConOut, TERM_TEXT_ATTR(color_fg[fg], color_bg[bg]));
    uefi_call_wrapper (ConOut->OutputString, 2, ConOut, str);
    uefi_call_wrapper (ConOut->SetCursorPosition, 3, ConOut, 0, 0);
    uefi_call_wrapper (ConOut->SetAttribute, 2,
                       ConOut, TERM_TEXT_ATTR(color_fg[9], color_bg[0]));
}

/* Display a string starting at x, y in fg foreground color and bg background
 * color. Characters in the string are not interpreted (e.g \n, \b, \t, etc.).
 * */
static void _puts(uint8_t x, uint8_t y, enum color fg, enum color bg, const char *s)
{
    for (; *s; s++, x++)
        _putc(x, y, fg, bg, *s);
}

/* Clear the screen to bg backround color. */
static void clear(enum color bg)
{
    uint8_t x, y;
    for (y = 0; y < ROWS; y++)
        for (x = 0; x < COLS; x++)
            _putc(x, y, bg, bg, ' ');
}

/* Keyboard Input */

#define KEY_D     'd'
#define KEY_H     'h'
#define KEY_P     'p'
#define KEY_R     'r'
#define KEY_S     's'
#define KEY_UP    SCAN_UP
#define KEY_DOWN  SCAN_DOWN
#define KEY_LEFT  SCAN_LEFT
#define KEY_RIGHT SCAN_RIGHT
#define KEY_ENTER 0x0d
#define KEY_SPACE ' '
#define KEY_ESC   SCAN_ESC

/* Return the scancode of the current up or down key if it has changed since
 * the last call, otherwise returns 0. When called on every iteration of the
 * main loop, returns non-zero on a key event. */
static int scan(void)
{
    EFI_STATUS status;
    EFI_INPUT_KEY key;
    status = uefi_call_wrapper (ConIn->ReadKeyStroke, 2, ConIn, &key);
    if (status == EFI_SUCCESS)
    {
        if (key.ScanCode != 0)
            return key.ScanCode; 
        if (key.UnicodeChar != 0)
            return (int) key.UnicodeChar;
    }
    return 0;
}

/* PC Speaker */
static void speaker_play(uint32_t hz, uintn_t time)
{
    uint32_t div = 0;
    if (hz < 20)
        hz = 20;
    if (hz > 20000)
        hz = 20000;
    div = 1193180 / hz;
    uintn_t ms = time * 1000;
    /* speaker freq */
    outb(0x43, 0xB6);
    outb(0x42, (uint8_t) div);
    outb(0x42, (uint8_t) (div >> 8));
    /* speaker on */
    outb(0x61, inb(0x61) | 0x3);
    /* sleep */
    uefi_call_wrapper (BS->Stall, 1, ms);
    /* speaker off */
    outb(0x61, inb(0x61) & 0xFC);
}

/* Formatting */

/* Format n in radix r (2-16) as a w length string. */
static char *itoa(uint32_t n, uint8_t r, uint8_t w)
{
    static const char d[16] = "0123456789ABCDEF";
    static char s[34];
    s[33] = 0;
    uint8_t i = 33;
    do {
        i--;
        s[i] = d[n % r];
        n /= r;
    } while (i > 33 - w);
    return (char *) (s + i);
}

/* Random */

/* Generate a random number from 0 inclusive to range exclusive from the number
 * of CPU ticks since boot. */
static uint32_t rand(uint32_t range)
{
    return (uint32_t) rdtsc() % range;
}

/* Shuffle an array of bytes arr of length len in-place using Fisher-Yates. */
static void shuffle(uint8_t arr[], uint32_t len)
{
    uint32_t i, j;
    uint8_t t;
    for (i = len - 1; i > 0; i--) {
        j = rand(i + 1);
        t = arr[i];
        arr[i] = arr[j];
        arr[j] = t;
    }
}

/* Tetris */

/* The seven tetriminos in each rotation. Each tetrimino is represented as an
 * array of 4 rotations, each represented by a 4x4 array of color values. */
uint8_t TETRIS[7][4][4][4] = {
    { /* I */
        {{0,0,0,0},
         {4,4,4,4},
         {0,0,0,0},
         {0,0,0,0}},
        {{0,4,0,0},
         {0,4,0,0},
         {0,4,0,0},
         {0,4,0,0}},
        {{0,0,0,0},
         {4,4,4,4},
         {0,0,0,0},
         {0,0,0,0}},
        {{0,4,0,0},
         {0,4,0,0},
         {0,4,0,0},
         {0,4,0,0}}
    },
    { /* J */
        {{7,0,0,0},
         {7,7,7,0},
         {0,0,0,0},
         {0,0,0,0}},
        {{0,7,7,0},
         {0,7,0,0},
         {0,7,0,0},
         {0,0,0,0}},
        {{0,0,0,0},
         {7,7,7,0},
         {0,0,7,0},
         {0,0,0,0}},
        {{0,7,0,0},
         {0,7,0,0},
         {7,7,0,0},
         {0,0,0,0}}
    },
    { /* L */
        {{0,0,5,0},
         {5,5,5,0},
         {0,0,0,0},
         {0,0,0,0}},
        {{0,5,0,0},
         {0,5,0,0},
         {0,5,5,0},
         {0,0,0,0}},
        {{0,0,0,0},
         {5,5,5,0},
         {5,0,0,0},
         {0,0,0,0}},
        {{5,5,0,0},
         {0,5,0,0},
         {0,5,0,0},
         {0,0,0,0}}
    },
    { /* O */
        {{0,0,0,0},
         {0,1,1,0},
         {0,1,1,0},
         {0,0,0,0}},
        {{0,0,0,0},
         {0,1,1,0},
         {0,1,1,0},
         {0,0,0,0}},
        {{0,0,0,0},
         {0,1,1,0},
         {0,1,1,0},
         {0,0,0,0}},
        {{0,0,0,0},
         {0,1,1,0},
         {0,1,1,0},
         {0,0,0,0}}
    },
    { /* S */
        {{0,0,0,0},
         {0,2,2,0},
         {2,2,0,0},
         {0,0,0,0}},
        {{0,2,0,0},
         {0,2,2,0},
         {0,0,2,0},
         {0,0,0,0}},
        {{0,0,0,0},
         {0,2,2,0},
         {2,2,0,0},
         {0,0,0,0}},
        {{0,2,0,0},
         {0,2,2,0},
         {0,0,2,0},
         {0,0,0,0}}
    },
    { /* T */
        {{0,6,0,0},
         {6,6,6,0},
         {0,0,0,0},
         {0,0,0,0}},
        {{0,6,0,0},
         {0,6,6,0},
         {0,6,0,0},
         {0,0,0,0}},
        {{0,0,0,0},
         {6,6,6,0},
         {0,6,0,0},
         {0,0,0,0}},
        {{0,6,0,0},
         {6,6,0,0},
         {0,6,0,0},
         {0,0,0,0}}
    },
    { /* Z */
        {{0,0,0,0},
         {3,3,0,0},
         {0,3,3,0},
         {0,0,0,0}},
        {{0,0,3,0},
         {0,3,3,0},
         {0,3,0,0},
         {0,0,0,0}},
        {{0,0,0,0},
         {3,3,0,0},
         {0,3,3,0},
         {0,0,0,0}},
        {{0,0,3,0},
         {0,3,3,0},
         {0,3,0,0},
         {0,0,0,0}}
    }
};

/* Two-dimensional array of color values */
uint8_t well[WELL_HEIGHT][WELL_WIDTH];

struct {
    uint8_t i, r; /* Index and rotation into the TETRIS array */
    uint8_t p;    /* Index into bag of preview tetrimino */
    int8_t x, y; /* Coordinates */
    int8_t g;    /* Y-coordinate of ghost */
} current;

/* Shuffled bag of next tetrimino indices */
#define BAG_SIZE (7)
uint8_t bag[BAG_SIZE] = {0, 1, 2, 3, 4, 5, 6};

uint32_t score = 0, level = 1, speed = INITIAL_SPEED, level_up = 0;

bool paused = false, game_over = false;

/* Return true if the tetrimino i in rotation r will collide when placed at x,
 * y. */
static bool collide(uint8_t i, uint8_t r, int8_t x, int8_t y)
{
    uint8_t xx, yy;
    for (yy = 0; yy < 4; yy++)
        for (xx = 0; xx < 4; xx++)
            if (TETRIS[i][r][yy][xx])
                if (x + xx < 0 || x + xx >= WELL_WIDTH ||
                    y + yy < 0 || y + yy >= WELL_HEIGHT ||
                    well[y + yy][x + xx])
                        return true;
    return false;
}

uint32_t stats[7];

/* Set the current tetrimino to the preview tetrimino in the default rotation
 * and place it in the top center. Increase the stats count for the spawned
 * tetrimino. Set the preview tetrimino to the next one in the shuffled bag. If
 * the spawned tetrimino was the last in the bag, re-shuffle the bag and set
 * the preview to the first in the bag. */
static void spawn(void)
{
    current.i = bag[current.p];
    stats[current.i]++;
    current.r = 0;
    current.x = WELL_WIDTH / 2 - 2;
    current.y = 0;
    current.p++;
    if (current.p == BAG_SIZE) {
        current.p = 0;
        shuffle(bag, BAG_SIZE);
    }
}

/* Set the ghost y-coordinate by moving the current tetrimino down until it
 * collides. */
static void ghost(void)
{
    int8_t y;
    for (y = current.y; y < WELL_HEIGHT; y++)
        if (collide(current.i, current.r, current.x, y))
            break;
    current.g = y - 1;
}

/* Try to move the current tetrimino by dx, dy and return true if successful.
 */
static bool move(int8_t dx, int8_t dy)
{
    if (game_over)
        return false;

    if (collide(current.i, current.r, current.x + dx, current.y + dy))
        return false;
    current.x += dx;
    current.y += dy;
    return true;
}

/* Try to rotate the current tetrimino clockwise and return true if successful.
 */
static bool rotate(void)
{
    if (game_over)
        return false;

    uint8_t r = (current.r + 1) % 4;
    if (collide(current.i, r, current.x, current.y))
        return false;
    current.r = r;
    return true;
}

/* Try to move the current tetrimino down one and increase the score if
 * successful. */
static void soft_drop(void)
{
    if (move(0, 1))
        score += SOFT_DROP_SCORE;
}

/* Lock the current tetrimino into the well. This is done by copying the color
 * values from the 4x4 array of the tetrimino into the well array. */
static void lock(void)
{
    uint8_t x, y;
    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++)
            if (TETRIS[current.i][current.r][y][x])
                well[current.y + y][current.x + x] =
                    TETRIS[current.i][current.r][y][x];
}

/* The y-coordinates of the rows cleared in the last update, top down */
int8_t cleared_rows[4];

/* Update the game state. Called at an interval relative to the current level.
 */
static void update(void)
{
    /* Gravity: move the current tetrimino down by one. If it cannot be moved
     * and it is still in the top row, set game over state. If it cannot be
     * moved down but is not in the top row, lock it in place and spawn a new
     * tetrimino. */
    if (!move(0, 1)) {
        if (current.y == 0) {
            game_over = true;
            return;
        }
        lock();
        spawn();
    }

    /* Row clearing: check if any rows are full across and add them to the
     * cleared_rows array. */
    static uint8_t level_rows = 0; /* Rows cleared in the current level */

    uint8_t x, y, a, i = 0, rows = 0;
    for (y = 0; y < WELL_HEIGHT; y++) {
        for (a = 0, x = 0; x < WELL_WIDTH; x++)
            if (well[y][x])
                a++;
        if (a != WELL_WIDTH)
            continue;

        rows++;
        cleared_rows[i++] = y;
    }

    /* Scoring */
    switch (rows) {
    case 1: score += SCORE_FACTOR_1 * level; break;
    case 2: score += SCORE_FACTOR_2 * level; break;
    case 3: score += SCORE_FACTOR_3 * level; break;
    case 4: score += SCORE_FACTOR_4 * level; break;
    }
    /* Leveling: increase the level for every 10 rows cleared, increase game
     * speed. */
    level_rows += rows;
    if (level_rows >= ROWS_PER_LEVEL) {
        level++;
        level_rows -= ROWS_PER_LEVEL;
        speed = 10 + 990 / level;
        level_up = 1;
    }
}

/* Clear the rows in the rows_cleared array and move all rows above them down.
 */
static void clear_rows(void)
{
    int8_t i, y, x;
    for (i = 0; i < 4; i++) {
        if (!cleared_rows[i])
            break;
        for (y = cleared_rows[i]; y > 0; y--)
            for (x = 0; x < WELL_WIDTH; x++)
                well[y][x] = well[y - 1][x];
        cleared_rows[i] = 0;
    }
}

/* Move the current tetrimino to the position of its ghost, increase the score
 * and trigger an update (to cause locking and clearing). */
static void drop(void)
{
    if (game_over)
        return;

    score += HARD_DROP_SCORE_FACTOR * (current.g - current.y);
    current.y = current.g;
    update();
}

#define TITLE_X (COLS / 2 - 9)
#define TITLE_Y (ROWS / 2 - 1)

/* Draw about information in the centre. Shown on boot and pause. */
static void draw_about(void) {
    _puts(TITLE_X,      TITLE_Y,     BLACK,  RED,     "   ");
    _puts(TITLE_X + 3,  TITLE_Y,     BLACK,  MAGENTA, "   ");
    _puts(TITLE_X + 6,  TITLE_Y,     BLACK,  BLUE,    "   ");
    _puts(TITLE_X + 9,  TITLE_Y,     BLACK,  GREEN,   "   ");
    _puts(TITLE_X + 12, TITLE_Y,     BLACK,  BROWN,   "   ");
    _puts(TITLE_X + 15, TITLE_Y,     BLACK,  CYAN,    "   ");
    _puts(TITLE_X,      TITLE_Y + 1, GRAY,   RED,     " T ");
    _puts(TITLE_X + 3,  TITLE_Y + 1, GRAY,   MAGENTA, " E ");
    _puts(TITLE_X + 6,  TITLE_Y + 1, GRAY,   BLUE,    " T ");
    _puts(TITLE_X + 9,  TITLE_Y + 1, GRAY,   GREEN,   " R ");
    _puts(TITLE_X + 12, TITLE_Y + 1, GRAY,   BROWN,   " I ");
    _puts(TITLE_X + 15, TITLE_Y + 1, GRAY,   CYAN,    " S ");
    _puts(TITLE_X,      TITLE_Y + 2, BLACK,  RED,     "   ");
    _puts(TITLE_X + 3,  TITLE_Y + 2, BLACK,  MAGENTA, "   ");
    _puts(TITLE_X + 6,  TITLE_Y + 2, BLACK,  BLUE,    "   ");
    _puts(TITLE_X + 9,  TITLE_Y + 2, BLACK,  GREEN,   "   ");
    _puts(TITLE_X + 12, TITLE_Y + 2, BLACK,  BROWN,   "   ");
    _puts(TITLE_X + 15, TITLE_Y + 2, BLACK,  CYAN,    "   ");

    _puts(0, ROWS - 1, GRAY,  BLACK,
         "TETRIS for UEFI");
}

#define WELL_X (COLS / 2 - WELL_WIDTH)

#define PREVIEW_X (COLS * 3/4 + 1)
#define PREVIEW_Y (2)

#define STATUS_X (COLS * 3/4)
#define STATUS_Y (ROWS / 2 - 4)

#define SCORE_X STATUS_X
#define SCORE_Y (ROWS / 2 - 1)

#define LEVEL_X SCORE_X
#define LEVEL_Y (SCORE_Y + 4)

/* Draw the well, current tetrimino, its ghost, the preview tetrimino, the
 * status, score and level indicators. Each well/tetrimino cell is drawn one
 * screen-row high and two screen-columns wide. The top two rows of the well
 * are hidden. Rows in the cleared_rows array are drawn as white rather than
 * their actual colors. */
static void draw(void)
{
    uint8_t x, y;

    if (paused) {
        draw_about();
        goto status;
    }

    /* Border */
    for (y = 2; y < WELL_HEIGHT; y++) {
        _putc(WELL_X - 1,            y, BLACK, BRIGHT, ' ');
        _putc(COLS / 2 + WELL_WIDTH, y, BLACK, BRIGHT, ' ');
    }
    for (x = 0; x < WELL_WIDTH * 2 + 2; x++)
        _putc(WELL_X + x - 1, WELL_HEIGHT, BLACK, BRIGHT, ' ');

    /* Well */
    for (y = 0; y < 2; y++)
        for (x = 0; x < WELL_WIDTH; x++)
            _puts(WELL_X + x * 2, y, BLACK, BLACK, "  ");
    for (y = 2; y < WELL_HEIGHT; y++)
        for (x = 0; x < WELL_WIDTH; x++)
            if (well[y][x])
                if (cleared_rows[0] == y || cleared_rows[1] == y ||
                    cleared_rows[2] == y || cleared_rows[3] == y)
                    _puts(WELL_X + x * 2, y, BLACK, BRIGHT, "  ");
                else
                    _puts(WELL_X + x * 2, y, BLACK, well[y][x], "  ");
            else
                _puts(WELL_X + x * 2, y, BROWN, BLACK, "  "); /* FIXME */

    /* Ghost */
    if (!game_over)
        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++)
                if (TETRIS[current.i][current.r][y][x])
                    _puts(WELL_X + current.x * 2 + x * 2, current.g + y,
                        TETRIS[current.i][current.r][y][x], BLACK, "::");

    /* Current */
    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++)
            if (TETRIS[current.i][current.r][y][x])
                _puts(WELL_X + current.x * 2 + x * 2, current.y + y, BLACK,
                     TETRIS[current.i][current.r][y][x], "  ");

    /* Preview */
    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++)
            if (TETRIS[bag[current.p]][0][y][x])
                _puts(PREVIEW_X + x * 2, PREVIEW_Y + y, BLACK,
                     TETRIS[bag[current.p]][0][y][x], "  ");
            else
                _puts(PREVIEW_X + x * 2, PREVIEW_Y + y, BLACK, BLACK, "  ");

status:
    if (paused)
        _puts(STATUS_X + 2, STATUS_Y, BRIGHT, BLACK, "PAUSED");
    if (game_over)
        _puts(STATUS_X, STATUS_Y, BRIGHT, BLACK, "GAME OVER");

    /* Score */
    _puts(SCORE_X + 2, SCORE_Y, GREEN, BLACK, "SCORE");
    _puts(SCORE_X, SCORE_Y + 2, BRIGHT, BLACK, itoa(score, 10, 10));

    /* Level */
    _puts(LEVEL_X + 2, LEVEL_Y, GREEN, BLACK, "LEVEL");
    _puts(LEVEL_X, LEVEL_Y + 2, BRIGHT, BLACK, itoa(level, 10, 10));
}

EFI_STATUS
EFIAPI
efi_main (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    InitializeLib(ImageHandle, SystemTable);
    ConOut = SystemTable->ConOut;
    ConIn = SystemTable->ConIn;
    memset (well, 0, sizeof (well));
    score = 0;
    level = 1;
    speed = INITIAL_SPEED;
    paused = false;
    game_over = false;
    SIMPLE_TEXT_OUTPUT_MODE mode;
    /* Save the current console cursor position and attributes */
    memcpy(&mode, ConOut->Mode, sizeof(SIMPLE_TEXT_OUTPUT_MODE));
    uefi_call_wrapper (ConOut->EnableCursor, 2, ConOut, 0);

    clear(BLACK);
    draw_about();
    /* Music: Mario Bros. Mushroom Powerup */
    speaker_play(523, 35);
    speaker_play(392, 35);
    speaker_play(523, 35);
    speaker_play(659, 35);
    speaker_play(784, 35);
    speaker_play(1047, 35);
    speaker_play(784, 35);
    speaker_play(415, 35);
    speaker_play(523, 35);
    speaker_play(622, 35);
    speaker_play(831, 35);
    speaker_play(622, 35);
    speaker_play(831, 35);
    speaker_play(1046, 35);
    speaker_play(1244, 35);
    speaker_play(1661, 35);
    speaker_play(1244, 35);
    speaker_play(466, 35);
    speaker_play(587, 35);
    speaker_play(698, 35);
    speaker_play(932, 35);
    speaker_play(1195, 35);
    speaker_play(1397, 35);
    speaker_play(1865, 35);
    speaker_play(1397, 35);
    /* Wait a full second to calibrate timing. */
    uint32_t itpms;
    tps();
    itpms = tpms; while (tpms == itpms) tps();
    itpms = tpms; while (tpms == itpms) tps();

    /* Initialize game state. Shuffle bag of tetriminos until first tetrimino
     * is not S or Z. */
    do { shuffle(bag, BAG_SIZE); } while (bag[0] == 4 || bag[0] == 6);
    spawn();
    ghost();
    clear(BLACK);
    draw();

    bool debug = false, help = true, statistics = false;
    int last_key;
loop:
    tps();
    if (!debug && !statistics)
        help = true;

    if (debug) {
        uint32_t i;
        _puts(0,  0, GRAY,   BLACK, "RTC sec:");
        _puts(10, 0, GREEN,  BLACK, itoa(rtcs(), 16, 2));
        _puts(0,  1, GRAY,   BLACK, "ticks/ms:");
        _puts(10, 1, GREEN,  BLACK, itoa(tpms, 10, 10));
        _puts(0,  2, GRAY,   BLACK, "key:");
        _puts(10, 2, GREEN,  BLACK, itoa(last_key, 16, 2));
        _puts(0,  3, GRAY,   BLACK, "i,r,p:");
        _puts(10, 3, GREEN,  BLACK, itoa(current.i, 10, 1));
        _putc(11, 3, GREEN,  BLACK, ',');
        _puts(12, 3, GREEN,  BLACK, itoa(current.r, 10, 1));
        _putc(13, 3, GREEN,  BLACK, ',');
        _puts(14, 3, GREEN,  BLACK, itoa(current.p, 10, 1));
        _puts(0,  4, GRAY,   BLACK, "x,y,g:");
        _puts(10, 4, GREEN,  BLACK, itoa(current.x, 10, 3));
        _putc(13, 4, GREEN,  BLACK, ',');
        _puts(14, 4, GREEN,  BLACK, itoa(current.y, 10, 3));
        _putc(17, 4, GREEN,  BLACK, ',');
        _puts(18, 4, GREEN,  BLACK, itoa(current.g, 10, 3));
        _puts(0,  5, GRAY,   BLACK, "bag:");
        for (i = 0; i < 7; i++)
            _puts(10 + i * 2, 5, GREEN, BLACK, itoa(bag[i], 10, 1));
        _puts(0,  6, GRAY,   BLACK, "speed:");
        _puts(10, 6, GREEN,  BLACK, itoa(speed, 10, 10));
        for (i = 0; i < TIMER__LENGTH; i++) {
            _puts(0,  7 + i, GRAY,   BLACK, "timer:");
            _puts(10, 7 + i, GREEN,  BLACK, itoa(timers[i], 10, 10));
        }
    }

    if (help) {
        _puts(1, 12, GRAY,   BLACK, "LEFT");
        _puts(7, 12, BLUE,   BLACK, "- Move left");
        _puts(1, 13, GRAY,   BLACK, "RIGHT");
        _puts(7, 13, BLUE,   BLACK, "- Move right");
        _puts(1, 14, GRAY,   BLACK, "UP");
        _puts(7, 14, BLUE,   BLACK, "- Rotate clockwise");
        _puts(1, 15, GRAY,   BLACK, "DOWN");
        _puts(7, 15, BLUE,   BLACK, "- Soft drop");
        _puts(1, 16, GRAY,   BLACK, "ENTER");
        _puts(7, 16, BLUE,   BLACK, "- Hard drop");
        _puts(1, 17, GRAY,   BLACK, "P");
        _puts(7, 17, BLUE,   BLACK, "- Pause");
        _puts(1, 18, GRAY,   BLACK, "ESC");
        _puts(7, 18, BLUE,   BLACK, "- Exit");
        _puts(1, 19, GRAY,   BLACK, "S");
        _puts(7, 19, BLUE,   BLACK, "- Toggle statistics");
        _puts(1, 20, GRAY,   BLACK, "D");
        _puts(7, 20, BLUE,   BLACK, "- Toggle debug info");
        _puts(1, 21, GRAY,   BLACK, "H");
        _puts(7, 21, BLUE,   BLACK, "- Toggle help");
    }

    if (statistics) {
        uint8_t i, x, y;
        for (i = 0; i < 7; i++) {
            for (y = 0; y < 4; y++)
                for (x = 0; x < 4; x++)
                    if (TETRIS[i][0][y][x])
                        _puts(5 + x * 2, 1 + i * 3 + y, BLACK,
                             TETRIS[i][0][y][x], "  ");
            _puts(14, 2 + i * 3, BLUE, BLACK, itoa(stats[i], 10, 10));
        }
    }

    bool updated = false;

    int key;
    if ((key = scan())) {
        last_key = key;
        switch(key) {
        case KEY_D:
            debug = !debug;
            if (debug)
                help = statistics = false;
            clear(BLACK);
            break;
        case KEY_H:
            help = !help;
            if (help)
                debug = statistics = false;
            clear(BLACK);
            break;
        case KEY_S:
            statistics = !statistics;
            if (statistics)
                debug = help = false;
            clear(BLACK);
            break;
        case KEY_R:
        case KEY_ESC:
            goto fail;
        case KEY_LEFT:
            move(-1, 0);
            break;
        case KEY_RIGHT:
            move(1, 0);
            break;
        case KEY_DOWN:
            soft_drop();
            break;
        case KEY_UP:
        case KEY_SPACE:
            rotate();
            break;
        case KEY_ENTER:
            drop();
            break;
        case KEY_P:
            if (game_over)
                break;
            clear(BLACK);
            paused = !paused;
            break;
        }
        updated = true;
    }

    if (!paused && !game_over && interval(TIMER_UPDATE, speed)) {
        update();
        updated = true;
    }

    if (cleared_rows[0] && wait(TIMER_CLEAR, CLEAR_DELAY)) {
        clear_rows();
        updated = true;
    }

    if (updated) {
        ghost();
        draw();
    }
    
    if (level_up) {
        paused = true;
        speaker_play(400, 120);
        speaker_play(500, 120);
        speaker_play(600, 120);
        speaker_play(800, 120);
        level_up = 0;
        paused = false;
    }
    if (game_over) {
        /* U Can't Touch This  Artist: MC Hammer Author: Paolo Montesel (@kenoph) */
        /* 147 2 130 1 123 1 110 1 440 1 440 1 82 1 98 1 392 1 392 1 123 1 110 1 440 1 */
        speaker_play(147, 400);
        speaker_play(130, 200);
        speaker_play(123, 200);
        speaker_play(110, 200);
        speaker_play(440, 200);
        speaker_play(440, 200);
        speaker_play(82, 200);
        speaker_play(98, 200);
        speaker_play(392, 200);
        speaker_play(392, 200);
        speaker_play(123, 200);
        speaker_play(110, 200);
        speaker_play(440, 200);
        goto fail;
    }

    goto loop;
fail:
    uefi_call_wrapper (ConOut->EnableCursor, 2, ConOut, mode.CursorVisible);
    uefi_call_wrapper (ConOut->SetCursorPosition, 3,
                       ConOut, mode.CursorColumn, mode.CursorRow);
    uefi_call_wrapper (ConOut->SetAttribute, 2, ConOut, mode.Attribute);
    return EFI_SUCCESS;
}
