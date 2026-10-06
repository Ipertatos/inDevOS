#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <time.h>
#include <sys/ioctl.h>
#include <fcntl.h>

// Restore terminal settings on exit
static struct termios orig_termios;

static void restore_terminal() {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
    // Show cursor and clear screen
    printf("\033[?25h\033[0m\033[2J\033[H");
    fflush(stdout);
}

static void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(restore_terminal);

    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);   // Disable canonical mode and echo
    raw.c_cc[VMIN]  = 0;               // Non-blocking read
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

static void get_terminal_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        *cols = 80;
        *rows = 24;
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    }
}

int main() {
    enable_raw_mode();

    // Hide cursor
    printf("\033[?25l");
    fflush(stdout);

    // Seed RNG
    srand((unsigned)time(NULL));

    int height, width;
    get_terminal_size(&height, &width);

    // Allocate y-positions for each column
    int *ypos = (int *)malloc(width * sizeof(int));
    if (!ypos) {
        fprintf(stderr, "Memory allocation failed\n");
        return 1;
    }
    for (int x = 0; x < width; ++x) {
        ypos[x] = rand() % height;
    }

    // Set stdout to non-buffered mode for smoother output
    setvbuf(stdout, NULL, _IONBF, 0);

    // Make stdin non-blocking
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    while (1) {
        // Clear screen
        printf("\033[2J");

        // Draw each column
        for (int x = 0; x < width; ++x) {
            int y_head = ypos[x];
            int y_trail = y_head - 1;

            char head_char = (char)(33 + rand() % 94);
            char trail_char = (char)(33 + rand() % 94);

            // Draw head (bright white)
            if (y_head >= 0 && y_head < height) {
                printf("\033[%d;%dH\033[97;1m%c", y_head + 1, x + 1, head_char);
            }

            // Draw trail (green)
            if (y_trail >= 0 && y_trail < height) {
                printf("\033[%d;%dH\033[32m%c", y_trail + 1, x + 1, trail_char);
            }

            // Update y position
            ypos[x] = (y_head + 1) % height;
        }

        // Reset attributes
        printf("\033[0m");
        fflush(stdout);

        // Frame delay ~20 FPS
        usleep(50000);

        // Check for SPACE key press to exit
        char ch;
        if (read(STDIN_FILENO, &ch, 1) == 1 && ch == ' ') {
            break;
        }
    }

    free(ypos);
    return 0;
} 