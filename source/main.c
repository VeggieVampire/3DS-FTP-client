#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <ctype.h>

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000

static u32 *socBuf = NULL;

static ssize_t recv_line(int sock, char *buf, size_t maxlen) {
    size_t n = 0;
    while (n + 1 < maxlen) {
        char c;
        ssize_t r = recv(sock, &c, 1, 0);
        if (r <= 0) break;
        buf[n++] = c;
        if (n >= 2 && buf[n-2] == '\r' && buf[n-1] == '\n') break;
    }
    buf[n] = '\0';
    return (ssize_t)n;
}

static int send_cmd(int sock, const char *fmt, ...) {
    char cmd[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    size_t len = strnlen(cmd, sizeof(cmd));
    if (len + 2 < sizeof(cmd)) {
        cmd[len++] = '\r';
        cmd[len++] = '\n';
        cmd[len] = '\0';
    }
    return (int)send(sock, cmd, len, 0);
}

static int get_reply_code(const char *line) {
    if (strlen(line) < 3) return -1;
    if (!isdigit((unsigned char)line[0]) ||
        !isdigit((unsigned char)line[1]) ||
        !isdigit((unsigned char)line[2])) return -1;
    return (line[0]-'0')*100 + (line[1]-'0')*10 + (line[2]-'0');
}

// Parse "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)"
static int parse_pasv(const char *line, char *ipOut, size_t ipOutLen, int *portOut) {
    const char *p = strchr(line, '(');
    if (!p) return -1;
    int h1,h2,h3,h4,p1,p2;
    if (sscanf(p+1, "%d,%d,%d,%d,%d,%d", &h1,&h2,&h3,&h4,&p1,&p2) != 6) return -2;
    snprintf(ipOut, ipOutLen, "%d.%d.%d.%d", h1,h2,h3,h4);
    *portOut = (p1 << 8) | p2;
    return 0;
}

static int connect_tcp(const char *host, int port) {
    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%d", port);
    struct addrinfo hints; memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, portStr, &hints, &res) != 0) return -1;

    int s = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s < 0) continue;
        if (connect(s, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(s); s = -1;
    }
    freeaddrinfo(res);
    return s;
}

static void clamp_port(int *p) {
    if (*p < 1) *p = 1;
    if (*p > 65535) *p = 65535;
}

// Touchscreen IP editor using the 3DS software keyboard.
// Returns true if a valid IPv4 address was set, false if user canceled.
static bool prompt_ip(char *ip, size_t len) {
    while (1) {
        SwkbdState kb;
        swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 15);     // up to "255.255.255.255"
        swkbdSetHintText(&kb, "Enter server IPv4 address");
        swkbdSetInitialText(&kb, ip);
        swkbdSetButton(&kb, SWKBD_BUTTON_LEFT,  "Cancel", false);
        swkbdSetButton(&kb, SWKBD_BUTTON_RIGHT, "OK",     true);

        static char out[16];
        SwkbdButton btn = swkbdInputText(&kb, out, sizeof(out));
        if (btn != SWKBD_BUTTON_RIGHT) return false;  // canceled

        struct in_addr addr;
        if (inet_pton(AF_INET, out, &addr) == 1) {
            strncpy(ip, out, len);
            ip[len-1] = '\0';
            return true;                              // valid IP set
        }

        // Show a quick message and loop back to keyboard
        printf("\nInvalid IPv4. Try again.\n");
        gspWaitForVBlank();
    }
}

int main(int argc, char **argv) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    socBuf = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if (!socBuf) { printf("memalign failed\n"); goto exit; }
    if (socInit(socBuf, SOC_BUFFERSIZE) != 0) { printf("socInit failed\n"); goto exit; }

    char ftpHost[64] = "192.168.1.50";  // editable IP (press X to change)
    int  ftpPort     = 5000;            // default to 5000 (change with D-Pad/L/R)
    const char *ftpUser = "anonymous";  // anonymous login (no password)

    printf("3DS FTP Client (Anonymous)\n");
    printf("Host: %s   Port: %d\n", ftpHost, ftpPort);
    printf("Controls:\n");
    printf("  X = Edit IP (touch keyboard)\n");
    printf("  Up/Down = +/-1    Left/Right = +/-10    L/R = +/-100 (port)\n");
    printf("  A = Connect    START = Exit\n\n");

    // Pre-connect UI loop (edit IP & port)
    while (aptMainLoop()) {
        printf("\rHost: %-15s   Port: %5d   ", ftpHost, ftpPort);
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_START) goto net_cleanup;
        if (kDown & KEY_X) {
            bool changed = prompt_ip(ftpHost, sizeof(ftpHost));
            if (changed) {
                printf("\nIP set to %s\n", ftpHost);
            } else {
                printf("\nIP unchanged.\n");
            }
        }
        if (kDown & KEY_DUP)    { ftpPort += 1;   clamp_port(&ftpPort); }
        if (kDown & KEY_DDOWN)  { ftpPort -= 1;   clamp_port(&ftpPort); }
        if (kDown & KEY_DRIGHT) { ftpPort += 10;  clamp_port(&ftpPort); }
        if (kDown & KEY_DLEFT)  { ftpPort -= 10;  clamp_port(&ftpPort); }
        if (kDown & KEY_R)      { ftpPort += 100; clamp_port(&ftpPort); }
        if (kDown & KEY_L)      { ftpPort -= 100; clamp_port(&ftpPort); }
        if (kDown & KEY_A) break;
        gspWaitForVBlank();
    }

    printf("\nConnecting to %s:%d ...\n", ftpHost, ftpPort);
    int ctrl = connect_tcp(ftpHost, ftpPort);
    if (ctrl < 0) {
        printf("Control connect failed.\nPress START to exit.\n");
        while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & KEY_START) break; gspWaitForVBlank(); }
        goto net_cleanup;
    }

    char line[1024];
    if (recv_line(ctrl, line, sizeof(line)) > 0) printf("%s", line);

    // USER anonymous (no PASS unless asked)
    send_cmd(ctrl, "USER %s", ftpUser);
    if (recv_line(ctrl, line, sizeof(line)) > 0) printf("%s", line);
    int code = get_reply_code(line);
    if (code == 331) {
        send_cmd(ctrl, "PASS"); // empty password
        if (recv_line(ctrl, line, sizeof(line)) > 0) printf("%s", line);
        code = get_reply_code(line);
    }
    if (code != 230) { printf("Login failed (code %d)\n", code); goto close_ctrl; }

    send_cmd(ctrl, "TYPE I");
    if (recv_line(ctrl, line, sizeof(line)) > 0) printf("%s", line);

    send_cmd(ctrl, "PASV");
    if (recv_line(ctrl, line, sizeof(line)) > 0) printf("%s", line);

    char pasvIp[64]; int pasvPort = 0;
    if (parse_pasv(line, pasvIp, sizeof(pasvIp), &pasvPort) != 0) { printf("PASV parse failed\n"); goto close_ctrl; }

    int data = connect_tcp(pasvIp, pasvPort);
    if (data < 0) { printf("Data connect failed\n"); goto close_ctrl; }

    send_cmd(ctrl, "LIST");
    if (recv_line(ctrl, line, sizeof(line)) > 0) printf("%s", line);

    char buf[2048]; ssize_t r;
    while ((r = recv(data, buf, sizeof(buf)-1, 0)) > 0) {
        buf[r] = '\0';
        printf("%s", buf);
        hidScanInput();
        if (hidKeysDown() & KEY_B) break;
        gspWaitForVBlank();
    }
    close(data);

    if (recv_line(ctrl, line, sizeof(line)) > 0) printf("%s", line);

    send_cmd(ctrl, "QUIT");
    if (recv_line(ctrl, line, sizeof(line)) > 0) printf("%s", line);

    printf("\nPress START to exit.\n");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }

close_ctrl:
    close(ctrl);
net_cleanup:
    socExit();
exit:
    if (socBuf) free(socBuf);
    gfxExit();
    return 0;
}
