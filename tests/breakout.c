/*
 * Breakout - A brick-breaking game for inDevOS
 * Use A/D or Left/Right arrow to move the paddle
 * Score is saved to scores.txt
 */
#include <stdint.h>

typedef unsigned char bool;
#define true 1
#define false 0

/* Syscall numbers - must match kernel syscall.h */
#define SYS_OPEN  3
#define SYS_CLOSE 4
#define SYS_READ  1
#define SYS_WRITE 2
#define SYS_FB_CLEAR    63
#define SYS_FB_FILLRECT 64
#define SYS_FB_GET_WIDTH 65
#define SYS_FB_GET_HEIGHT 66
#define SYS_KB_READ      67
#define SYS_GET_TERMINATE_FLAG 69
#define O_RDONLY  0x00
#define O_WRONLY  0x01
#define O_CREAT   0x04
#define O_TRUNC   0x08

static inline uint64_t syscall(uint64_t num, uint64_t arg1, uint64_t arg2,
                              uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    uint64_t result;
    register uint64_t r10_reg __asm__("r10") = arg4;
    register uint64_t r8_reg __asm__("r8") = arg5;
    __asm__ volatile (
        "syscall"
        : "=a" (result)
        : "a" (num), "D" (arg1), "S" (arg2), "d" (arg3), "r" (r10_reg), "r" (r8_reg)
        : "memory", "rcx", "r11"
    );
    return result;
}

static int sys_open(const char *path, int flags) {
    return (int)syscall(SYS_OPEN, (uint64_t)path, (uint64_t)flags, 0, 0, 0);
}
static int sys_close(int fd) {
    return (int)syscall(SYS_CLOSE, (uint64_t)fd, 0, 0, 0, 0);
}
static int sys_read(int fd, void *buf, uint64_t count) {
    return (int)syscall(SYS_READ, (uint64_t)fd, (uint64_t)buf, count, 0, 0);
}
static int sys_write(int fd, const void *buf, uint64_t count) {
    return (int)syscall(SYS_WRITE, (uint64_t)fd, (uint64_t)buf, count, 0, 0);
}

/* Framebuffer/keyboard via syscalls (safe from Ring 3) */
static void clear_screen(void) { syscall(SYS_FB_CLEAR, 0, 0, 0, 0, 0); }
static uint64_t fb_get_width(void) { return syscall(SYS_FB_GET_WIDTH, 0, 0, 0, 0, 0); }
static uint64_t fb_get_height(void) { return syscall(SYS_FB_GET_HEIGHT, 0, 0, 0, 0, 0); }
static void fillrect(uint32_t color, uint64_t x, uint64_t y, uint64_t w, uint64_t h) {
    syscall(SYS_FB_FILLRECT, color, x, y, w, h);
}
static int kb_read(char *buf, unsigned int maxlen) {
    return (int)syscall(SYS_KB_READ, (uint64_t)buf, maxlen, 0, 0, 0);
}
static int get_terminate_flag(void) {
    return (int)syscall(SYS_GET_TERMINATE_FLAG, 0, 0, 0, 0, 0);
}
static void sys_exit(int status) {
    syscall(0 /* SYS_EXIT */, (uint64_t)status, 0, 0, 0, 0);
    for (;;) {} /* unreachable – prevent ret */
}

/* Simple helpers */
uint32_t rng_state = 12345;
uint32_t random(void) {
    rng_state = rng_state * 1103515245 + 12345;
    return rng_state;
}

static void utoa(unsigned int v, char *buf) {
    char tmp[12];
    int i = 0;
    if (v == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    while (v > 0) {
        tmp[i++] = '0' + (v % 10);
        v /= 10;
    }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

/* Draw a digit (0-9) at x,y using 5x6 blocky font, scale 2 */
static void draw_digit(uint32_t color, uint64_t x, uint64_t y, int d) {
    /* 5 wide, 6 tall, bit 4=left */
    static const uint8_t digits[10][6] = {
        {0x0E, 0x11, 0x11, 0x11, 0x11, 0x0E}, /* 0 */
        {0x04, 0x0C, 0x04, 0x04, 0x04, 0x0E}, /* 1 */
        {0x0E, 0x11, 0x01, 0x0E, 0x10, 0x1F}, /* 2 */
        {0x0E, 0x11, 0x06, 0x01, 0x11, 0x0E}, /* 3 */
        {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02}, /* 4 */
        {0x1F, 0x10, 0x1E, 0x01, 0x11, 0x0E}, /* 5 */
        {0x0E, 0x10, 0x1E, 0x11, 0x11, 0x0E}, /* 6 */
        {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08}, /* 7 */
        {0x0E, 0x11, 0x0E, 0x11, 0x11, 0x0E}, /* 8 */
        {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x0E}, /* 9 */
    };
    if (d < 0 || d > 9) return;
    const uint8_t *p = digits[d];
    const uint64_t scale = 2;
    for (int row = 0; row < 6; row++) {
        for (int col = 0; col < 5; col++) {
            if (p[row] & (1 << (4 - col))) {
                fillrect(color, x + (uint64_t)col * scale, y + (uint64_t)row * scale, scale, scale);
            }
        }
    }
}

static void draw_number(uint32_t color, uint64_t x, uint64_t y, unsigned int n) {
    char buf[12];
    utoa(n, buf);
    uint64_t dx = 0;
    for (int i = 0; buf[i]; i++) {
        draw_digit(color, x + dx, y, buf[i] - '0');
        dx += 14; /* 5*2 + spacing */
    }
}

/* Load high score from scores.txt (file format: score\nhigh_score or just score) */
static unsigned int load_high_score(void) {
    int fd = sys_open("/scores.txt", O_RDONLY);
    if (fd < 0) return 0;
    char buf[128];
    int n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    unsigned int max = 0, cur = 0;
    for (int i = 0; buf[i]; i++) {
        if (buf[i] >= '0' && buf[i] <= '9') {
            cur = cur * 10 + (buf[i] - '0');
        } else {
            if (cur > max) max = cur;
            cur = 0;
        }
    }
    if (cur > max) max = cur;
    return max;
}

/* Save score and high score to scores.txt */
static void save_score(unsigned int score, unsigned int high) {
    int fd = sys_open("/scores.txt", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return;
    char buf[48];
    int i = 0;
    utoa(score, buf);
    while (buf[i]) i++;
    buf[i++] = '\n';
    utoa(high, buf + i);
    while (buf[i]) i++;
    buf[i++] = '\n';
    sys_write(fd, buf, (uint64_t)i);
    sys_close(fd);
}

void _start(void) {
    clear_screen();

    uint64_t width = fb_get_width();
    uint64_t height = fb_get_height();

    unsigned int score = 0;
    unsigned int high_score = load_high_score();
    const unsigned int points_per_brick = 10;

    /* Game constants */
    const uint64_t paddle_width = width / 10;
    const uint64_t paddle_height = 10;
    const uint64_t paddle_y = height - 30;
    uint64_t paddle_x = width / 2 - paddle_width / 2;

    const uint64_t ball_size = 8;
    uint64_t ball_x = width / 2 - ball_size / 2;
    uint64_t ball_y = height / 2;
    int64_t ball_dx = (random() % 2 == 0 ? 3 : -3);
    int64_t ball_dy = -3;

    /* Bricks */
#define BRICK_ROWS 5
#define BRICK_COLS 10
#define NUM_BRICKS (BRICK_ROWS * BRICK_COLS)
    struct {
        uint64_t x, y, w, h;
        bool active;
    } bricks[NUM_BRICKS];

    uint64_t brick_w = (width - (BRICK_COLS + 1) * 10) / BRICK_COLS;
    uint64_t brick_h = 20;
    int brick_idx = 0;
    for (int row = 0; row < BRICK_ROWS; row++) {
        for (int col = 0; col < BRICK_COLS; col++) {
            bricks[brick_idx].x = col * (brick_w + 10) + 10;
            bricks[brick_idx].y = row * (brick_h + 5) + 50;
            bricks[brick_idx].w = brick_w;
            bricks[brick_idx].h = brick_h;
            bricks[brick_idx].active = true;
            brick_idx++;
        }
    }

    int active_count = NUM_BRICKS;
    bool game_over = false;

    /* Initial full draw - done once to populate the screen */
    clear_screen();
    for (int i = 0; i < NUM_BRICKS; i++) {
        fillrect(0x00FF00, bricks[i].x, bricks[i].y, bricks[i].w, bricks[i].h);
    }
    fillrect(0xFFFFFF, paddle_x, paddle_y, paddle_width, paddle_height);
    fillrect(0xFF0000, ball_x, ball_y, ball_size, ball_size);
    fillrect(0x333333, 8, 8, 140, 52);
    draw_number(0xFFFFFF, 12, 12, score);
    draw_number(0xFFFF00, 12, 34, high_score);

    /* Track previous positions for dirty-rect erase */
    uint64_t prev_ball_x = ball_x, prev_ball_y = ball_y;
    uint64_t prev_paddle_x = paddle_x;
    unsigned int prev_score = score;
    unsigned int prev_high = high_score;

    /* Game loop */
    while (!game_over) {
        if (get_terminate_flag()) {
            save_score(score, high_score);
            break;
        }

        /* Keyboard input */
        char kb_buf[32];
        int klen = kb_read(kb_buf, sizeof(kb_buf));
        for (int i = 0; i < klen; i++) {
            char ch = kb_buf[i];
            if (ch == 'a' || ch == 'A' || ch == 75) {
                if (paddle_x > 10) paddle_x -= 10;
            } else if (ch == 'd' || ch == 'D' || ch == 77) {
                if (paddle_x + paddle_width < width - 10) paddle_x += 10;
            }
        }

        /* Erase old ball and paddle positions before moving */
        fillrect(0x000000, prev_ball_x, prev_ball_y, ball_size, ball_size);
        fillrect(0x000000, prev_paddle_x, paddle_y, paddle_width, paddle_height);

        /* Ball movement */
        ball_x += ball_dx;
        ball_y += ball_dy;

        /* Wall collisions */
        if (ball_x >= width - ball_size || ball_x <= 0) ball_dx = -ball_dx;
        if (ball_y <= 0) ball_dy = -ball_dy;

        /* Bottom - lose */
        if (ball_y >= height - ball_size) {
            if (score > high_score) high_score = score;
            save_score(score, high_score);
            game_over = true;
            break;
        }

        /* Paddle collision */
        if (ball_y + ball_size >= paddle_y && ball_y <= paddle_y + paddle_height &&
            ball_x + ball_size >= paddle_x && ball_x <= paddle_x + paddle_width) {
            ball_dy = -ball_dy;
            ball_dx += (ball_x - (paddle_x + paddle_width / 2)) / 20;
            if (ball_dx > 6) ball_dx = 6;
            if (ball_dx < -6) ball_dx = -6;
        }

        /* Brick collisions - erase destroyed bricks immediately */
        for (int i = 0; i < NUM_BRICKS; i++) {
            if (bricks[i].active) {
                uint64_t bx = bricks[i].x, by = bricks[i].y;
                uint64_t bw = bricks[i].w, bh = bricks[i].h;

                if (ball_x + ball_size >= bx && ball_x <= bx + bw &&
                    ball_y + ball_size >= by && ball_y <= by + bh) {
                    bricks[i].active = false;
                    active_count--;
                    score += points_per_brick;
                    if (score > high_score) high_score = score;
                    ball_dy = -ball_dy;
                    fillrect(0x000000, bx, by, bw, bh);
                    break;
                }
            }
        }

        /* Win */
        if (active_count == 0) {
            if (score > high_score) high_score = score;
            save_score(score, high_score);
            game_over = true;
        }

        /* Draw new ball and paddle at updated positions */
        fillrect(0xFF0000, ball_x, ball_y, ball_size, ball_size);
        fillrect(0xFFFFFF, paddle_x, paddle_y, paddle_width, paddle_height);

        /* Redraw score box only when score changes */
        if (score != prev_score || high_score != prev_high) {
            fillrect(0x333333, 8, 8, 140, 52);
            draw_number(0xFFFFFF, 12, 12, score);
            draw_number(0xFFFF00, 12, 34, high_score);
            prev_score = score;
            prev_high = high_score;
        }

        /* Restore score box if ball erased part of it */
        if (prev_ball_x < 148 && prev_ball_y < 60) {
            fillrect(0x333333, 8, 8, 140, 52);
            draw_number(0xFFFFFF, 12, 12, score);
            draw_number(0xFFFF00, 12, 34, high_score);
        }

        prev_ball_x = ball_x;
        prev_ball_y = ball_y;
        prev_paddle_x = paddle_x;

        for (volatile uint64_t delay = 0; delay < 5000000; delay++) {
            __asm__ volatile ("nop");
        }
    }

    clear_screen();
    sys_exit(0);
}