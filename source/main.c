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
#include <sys/stat.h>

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000

#define CFG_DIR         "sdmc:/3ds/ftpc"
#define LAST_IP_PATH    CFG_DIR "/last_ip.txt"

static u32 *socBuf = NULL;

/* ---------- small FTP/socket helpers ---------- */

static ssize_t recv_line(int sock, char *buf, size_t maxlen) {
    size_t n = 0; char c;
    while (n + 1 < maxlen) {
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
    va_list ap; va_start(ap, fmt);
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

// "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)"
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
    char portStr[16]; snprintf(portStr, sizeof(portStr), "%d", port);
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

static void clamp_port(int *p) { if (*p < 1) *p = 1; if (*p > 65535) *p = 65535; }

/* ---------- persistence (last IP) ---------- */

static void ensure_cfg_dir(void) {
    mkdir("sdmc:/3ds", 0777);
    mkdir(CFG_DIR, 0777);
}

static bool load_last_ip(char *ip, size_t len) {
    FILE *f = fopen(LAST_IP_PATH, "r");
    if (!f) return false;
    if (!fgets(ip, (int)len, f)) { fclose(f); return false; }
    fclose(f);
    ip[strcspn(ip, "\r\n")] = 0;  // trim newline(s)
    struct in_addr addr;
    return inet_pton(AF_INET, ip, &addr) == 1;
}

static void save_last_ip(const char *ip) {
    ensure_cfg_dir();
    FILE *f = fopen(LAST_IP_PATH, "w");
    if (!f) return;
    fputs(ip, f); fputc('\n', f);
    fclose(f);
}

/* ---------- touchscreen IP editor ---------- */

static bool prompt_ip(char *ip, size_t len) {
    for (;;) {
        SwkbdState kb;
        swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 15);           // len up to "255.255.255.255"
        swkbdSetHintText(&kb, "Enter IPv4 (e.g., 192.168.0.10)");
        swkbdSetInitialText(&kb, ip);
        swkbdSetButton(&kb, SWKBD_BUTTON_LEFT,  "Cancel", false);
        swkbdSetButton(&kb, SWKBD_BUTTON_RIGHT, "OK",     true);

        static char out[16];
        SwkbdButton btn = swkbdInputText(&kb, out, sizeof(out));
        if (btn != SWKBD_BUTTON_RIGHT) return false;        // canceled

        struct in_addr addr;
        if (inet_pton(AF_INET, out, &addr) == 1) {
            strncpy(ip, out, len);
            ip[len-1] = '\0';
            return true;                                    // valid IP set
        }
        printf("\nInvalid IPv4. Try again.\n");
        gspWaitForVBlank();
    }
}

/* ---------- main app ---------- */

int main(int argc, char **argv) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    // (Optional) FS init — hbmenu normally mounts sdmc:/ already for .3dsx apps
    fsInit();

    // Network (SOC) init
    socBuf = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if (!socBuf) { printf("memalign failed\n"); goto exit_all; }
    if (socInit(socBuf, SOC_BUFFERSIZE) != 0) { printf("socInit failed\n"); goto exit_all; }

    char ftpHost[64] = "192.168.1.50";  // default; replaced if saved value exists
    int  ftpPort     = 5000;            // default port 5000
    const char *ftpUser = "anonymous";  // anonymous login (no password)

    if (load_last_ip(ftpHost, sizeof(ftpHost))) {
        printf("Loaded last IP: %s\n", ftpHost);
    } else {
        printf("No saved IP. Using default: %s\n", ftpHost);
    }

    printf("Controls:\n");
    printf("  X = Edit IP (touch keyboard)\n");
    printf("  Up/Down=±1  Left/Right=±10  L/R=±100 (port)\n");
    printf("  A = Connect   START = Exit\n\n");

    // Pre-connect UI loop (edit IP/port)
    while (aptMainLoop()) {
        printf("\rHost: %-15s   Port: %5d   ", ftpHost, ftpPort);
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_START) goto net_cleanup;

        if (kDown & KEY_X) {
            bool changed = prompt_ip(ftpHost, sizeof(ftpHost));
            printf(changed ? "\nIP set to %s\n" : "\nIP unchanged.\n", ftpHost);
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

    // Anonymous login: USER anonymous, send PASS only if server requests it (331)
    send_cmd(ctrl, "USER %s", ftpUser);
    if (recv_line(ctrl, line, sizeof(line)) > 0) printf("%s", line);
    int code = get_reply_code(line);

    if (code == 331) {
        // Some servers require PASS even for anonymous; send empty password
        send_cmd(ctrl, "PASS");
        if (recv_line(ctrl, line, sizeof(line)) > 0) printf("%s", line);
        code = get_reply_code(line);
    }
    if (code != 230) { printf("Login failed (code %d)\n", code); goto close_ctrl; }

    // Save IP on successful login
    save_last_ip(ftpHost);
    printf("(Saved last IP: %s)\n", ftpHost);

    // Binary mode + PASV LIST
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
exit_all:
    if (socBuf) free(socBuf);
    fsExit();
    gfxExit();
    return 0;
}
