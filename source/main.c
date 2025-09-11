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
#include <stdbool.h>

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000

#define CFG_DIR         "sdmc:/3ds/ftpc"
#define CFG_PATH        CFG_DIR "/config.ini"   // host=..., port=...

static u32 *socBuf = NULL;

/* -------------------- small helpers -------------------- */

static void clamp_port(int *p){ if(*p<1)*p=1; if(*p>65535)*p=65535; }

static void rstrip(char *s){
    size_t n=strlen(s);
    while(n && (s[n-1]=='\r' || s[n-1]=='\n' || s[n-1]==' ' || s[n-1]=='\t')) s[--n]=0;
}

static ssize_t recv_line(int sock, char *buf, size_t maxlen){
    size_t n=0; char c;
    while(n+1<maxlen){
        ssize_t r=recv(sock,&c,1,0);
        if(r<=0) break;
        buf[n++]=c;
        if(n>=2 && buf[n-2]=='\r' && buf[n-1]=='\n') break;
    }
    buf[n]=0;
    return (ssize_t)n;
}

static int send_cmd(int sock, const char *fmt, ...){
    char cmd[512];
    va_list ap; va_start(ap,fmt);
    vsnprintf(cmd,sizeof(cmd),fmt,ap);
    va_end(ap);
    size_t len=strnlen(cmd,sizeof(cmd));
    if(len+2<sizeof(cmd)){ cmd[len++]='\r'; cmd[len++]='\n'; cmd[len]=0; }
    return (int)send(sock,cmd,len,0);
}

static int get_reply_code(const char *line){
    if(strlen(line)<3) return -1;
    if(!isdigit((unsigned char)line[0])||!isdigit((unsigned char)line[1])||!isdigit((unsigned char)line[2])) return -1;
    return (line[0]-'0')*100+(line[1]-'0')*10+(line[2]-'0');
}

// "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)"
static int parse_pasv(const char *line, char *ipOut, size_t ipOutLen, int *portOut){
    const char *p=strchr(line,'(');
    if(!p) return -1;
    int h1,h2,h3,h4,p1,p2;
    if(sscanf(p+1,"%d,%d,%d,%d,%d,%d",&h1,&h2,&h3,&h4,&p1,&p2)!=6) return -2;
    snprintf(ipOut,ipOutLen,"%d.%d.%d.%d",h1,h2,h3,h4);
    *portOut=(p1<<8)|p2;
    return 0;
}

static int connect_tcp(const char *host,int port){
    char portStr[16]; snprintf(portStr,sizeof(portStr),"%d",port);
    struct addrinfo hints; memset(&hints,0,sizeof(hints));
    hints.ai_family=AF_INET; hints.ai_socktype=SOCK_STREAM;
    struct addrinfo *res=NULL;
    if(getaddrinfo(host,portStr,&hints,&res)!=0) return -1;
    int s=-1;
    for(struct addrinfo *ai=res; ai; ai=ai->ai_next){
        s=socket(ai->ai_family,ai->ai_socktype,ai->ai_protocol);
        if(s<0) continue;
        if(connect(s,ai->ai_addr,ai->ai_addrlen)==0) break;
        close(s); s=-1;
    }
    freeaddrinfo(res);
    return s;
}

/* -------------------- config (host+port) -------------------- */

static void ensure_cfg_dir(void){
    mkdir("sdmc:/3ds",0777);
    mkdir(CFG_DIR,0777);
}

static void save_config(const char *host,int port){
    ensure_cfg_dir();
    FILE *f=fopen(CFG_PATH,"w");
    if(!f) return;
    fprintf(f,"host=%s\n",host);
    fprintf(f,"port=%d\n",port);
    fclose(f);
}

static bool load_config(char *host,size_t hostlen,int *port){
    FILE *f=fopen(CFG_PATH,"r");
    if(!f) return false;
    bool saw_any=false;
    char line[128];
    while(fgets(line,sizeof(line),f)){
        rstrip(line);
        if(strncmp(line,"host=",5)==0){
            const char *v=line+5; struct in_addr a;
            if(inet_pton(AF_INET,v,&a)==1){
                inet_ntop(AF_INET,&a,host,hostlen);
                saw_any=true;
            }
        }else if(strncmp(line,"port=",5)==0){
            long p=strtol(line+5,NULL,10);
            if(p>0 && p<=65535){ *port=(int)p; saw_any=true; }
        }
    }
    fclose(f);
    return saw_any;
}

/* -------------------- IP:PORT prompt -------------------- */

static bool prompt_ip_port(char *host,size_t hostlen,int *port){
    char initial[64]; snprintf(initial,sizeof(initial),"%s:%d",host,*port);
    for(;;){
        SwkbdState kb; swkbdInit(&kb,SWKBD_TYPE_NORMAL,2,21); // "255.255.255.255:65535"
        swkbdSetHintText(&kb,"Enter IPv4:port (e.g., 192.168.1.222:5000)");
        swkbdSetInitialText(&kb,initial);
        swkbdSetButton(&kb,SWKBD_BUTTON_LEFT,"Cancel",false);
        swkbdSetButton(&kb,SWKBD_BUTTON_RIGHT,"OK",true);

        char out[64]={0};
        SwkbdButton btn=swkbdInputText(&kb,out,sizeof(out));
        if(btn!=SWKBD_BUTTON_RIGHT) return false; // canceled

        char *colon=strchr(out,':');
        if(!colon){ printf("\nNeed format IPv4:port\n"); gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank(); continue; }
        *colon=0;
        const char *ipstr=out;
        const char *pstr=colon+1;

        struct in_addr a; long p=strtol(pstr,NULL,10);
        if(inet_pton(AF_INET,ipstr,&a)==1 && p>0 && p<=65535){
            inet_ntop(AF_INET,&a,host,hostlen);
            *port=(int)p; clamp_port(port);
            return true;
        }
        printf("\nInvalid IPv4 or port.\n");
        gfxFlushBuffers(); gfxSwapBuffers();
        gspWaitForVBlank();
        snprintf(initial,sizeof(initial),"%s:%d",host,*port);
    }
}

/* -------------------- simple FTP ops -------------------- */

static bool ftp_pwd(int ctrl, char *outPath, size_t len){
    char line[1024];
    send_cmd(ctrl,"PWD");
    if(recv_line(ctrl,line,sizeof(line))<=0) return false;
    int code=get_reply_code(line);
    if(code!=257) return false;
    const char *q1=strchr(line,'\"');
    if(!q1) return false;
    const char *q2=strchr(q1+1,'\"');
    if(!q2) return false;
    size_t n=(size_t)(q2-q1-1);
    if(n>=len) n=len-1;
    memcpy(outPath,q1+1,n); outPath[n]=0;
    return true;
}

static bool ftp_cwd(int ctrl, const char *name){
    char line[1024];
    send_cmd(ctrl,"CWD %s",name);
    if(recv_line(ctrl,line,sizeof(line))<=0) return false;
    int code=get_reply_code(line);
    return (code>=200 && code<300);
}

static bool ftp_cdup(int ctrl){
    char line[1024];
    send_cmd(ctrl,"CDUP");
    if(recv_line(ctrl,line,sizeof(line))<=0) return false;
    int code=get_reply_code(line);
    return (code>=200 && code<300);
}

static int ftp_open_data(int ctrl){
    char line[1024], ip[64]; int port=0;
    send_cmd(ctrl,"PASV");
    if(recv_line(ctrl,line,sizeof(line))<=0) return -1;
    if(parse_pasv(line,ip,sizeof(ip),&port)!=0) return -1;
    return connect_tcp(ip,port);
}

static char* ftp_read_data_all(int dataSock, size_t *outSize){
    size_t cap=8192, sz=0;
    char *buf=(char*)malloc(cap+1);
    if(!buf) return NULL;
    for(;;){
        if(sz+4096>cap){
            cap*=2;
            char *nbuf=(char*)realloc(buf,cap+1);
            if(!nbuf){ free(buf); return NULL; }
            buf=nbuf;
        }
        ssize_t r=recv(dataSock, buf+sz, 4096, 0);
        if(r<=0) break;
        sz+=r;
    }
    buf[sz]=0;
    if(outSize) *outSize=sz;
    return buf;
}

// TYPE A + NLST (current dir only)
static bool ftp_nlst(int ctrl, char ***outNames, int *outCount){
    *outNames = NULL; *outCount = 0;

    char line[1024];
    send_cmd(ctrl,"TYPE A");            // ASCII for listing
    recv_line(ctrl,line,sizeof(line));  // ignore reply

    int data = ftp_open_data(ctrl);
    if (data < 0) return false;

    send_cmd(ctrl,"NLST");
    if (recv_line(ctrl,line,sizeof(line)) <= 0) { close(data); return false; }

    size_t sz = 0;
    char *all = ftp_read_data_all(data, &sz);
    close(data);
    if (!all) return false;

    recv_line(ctrl,line,sizeof(line)); // final 226

    int cap = 64, cnt = 0;
    char **names = (char**)malloc(sizeof(char*) * cap);
    if (!names) { free(all); return false; }

    char *p = all, *lineStart = all;
    while (*p) {
        if (*p == '\r' || *p == '\n') {
            *p = 0;
            if (*lineStart && strcmp(lineStart, ".") != 0) {
                if (cnt >= cap) { cap *= 2; char **nn=(char**)realloc(names,sizeof(char*)*cap); if(!nn){ free(names); free(all); return false; } names=nn; }
                names[cnt++] = strdup(lineStart);
            }
            p++;
            while (*p == '\r' || *p == '\n') p++;
            lineStart = p;
            continue;
        }
        p++;
    }
    if (*lineStart && strcmp(lineStart, ".") != 0) {
        if (cnt >= cap) { cap *= 2; names=(char**)realloc(names,sizeof(char*)*cap); if(!names){ free(all); return false; } }
        names[cnt++] = strdup(lineStart);
    }

    free(all);
    *outNames = names; *outCount = cnt;
    return true;
}

/* -------------------- download -------------------- */

static void sanitize_name(const char *in, char *out, size_t len){
    size_t i=0;
    for(; i<len-1 && in[i]; ++i){
        char c=in[i];
        if(c=='/'||c=='\\'||c==':') c='_';
        out[i]=c;
    }
    out[i]=0;
}

static void ensure_download_dir(void){
    mkdir("sdmc:/3ds",0777); // fine if already exists
}

static bool ftp_retr_to_file(int ctrl, const char *remoteName, const char *localPath){
    char line[1024];

    // Binary mode for file data
    send_cmd(ctrl,"TYPE I");
    recv_line(ctrl,line,sizeof(line)); // ignore reply (usually 200)

    int data = ftp_open_data(ctrl);
    if (data < 0) return false;

    send_cmd(ctrl,"RETR %s", remoteName);
    if (recv_line(ctrl,line,sizeof(line)) <= 0) { close(data); return false; }
    int code = get_reply_code(line);
    if (code >= 400) { close(data); return false; } // e.g., 550

    FILE *f = fopen(localPath,"wb");
    if(!f){ close(data); return false; }

    printf("Downloading '%s'\n-> %s\n", remoteName, localPath);
    gfxFlushBuffers(); gfxSwapBuffers();

    char buf[16*1024];
    ssize_t r;
    size_t total = 0;
    size_t dotEvery = 128*1024; // print a dot every 128KB
    size_t nextDot = dotEvery;

    while((r=recv(data,buf,sizeof(buf),0))>0){
        fwrite(buf,1,(size_t)r,f);
        total += (size_t)r;
        if(total >= nextDot){
            printf("."); gfxFlushBuffers(); gfxSwapBuffers();
            nextDot += dotEvery;
        }
    }
    close(data);
    fclose(f);

    recv_line(ctrl,line,sizeof(line)); // expect 226 Transfer complete
    code = get_reply_code(line);
    bool ok = (code>=200 && code<300);

    printf(ok? "\nDone (%lu bytes)\n" : "\nDownload failed.\n", (unsigned long)total);
    gfxFlushBuffers(); gfxSwapBuffers();
    return ok;
}

/* -------------------- browser state -------------------- */

typedef struct { char name[256]; } Entry;

typedef struct {
    Entry *items;
    int count;
    int selected;
} Listing;

static void free_listing(Listing *L){
    if(!L || !L->items) return;
    free(L->items);
    L->items=NULL; L->count=0; L->selected=0;
}

// Build listing for current dir only; add ".." at top (except at "/")
static bool build_listing(int ctrl, const char *cwdPath, Listing *L){
    free_listing(L);
    char **names=NULL; int n=0;
    if(!ftp_nlst(ctrl,&names,&n)) return false;

    int addUp = (strcmp(cwdPath,"/")!=0);
    int total = n + addUp;
    if(total<=0){ L->items=NULL; L->count=0; L->selected=0; return true; }

    L->items = (Entry*)calloc(total, sizeof(Entry));
    L->count = 0; L->selected = 0;

    if(addUp){
        snprintf(L->items[L->count].name, sizeof(L->items[L->count].name), "..");
        L->count++;
    }
    for(int i=0;i<n;i++){
        if(!names[i] || !*names[i]) { free(names[i]); continue; }
        if(strcmp(names[i],"..")==0){ free(names[i]); continue; }
        strncpy(L->items[L->count].name, names[i], sizeof(L->items[L->count].name)-1);
        L->count++;
        free(names[i]);
    }
    free(names);
    return true;
}

/* ----- rendering (about 30 rows) ----- */

static void render_listing(const char *cwdPath, const Listing *L){
    consoleClear();
    printf("FTP Browser - %s\n", cwdPath);
    printf("Up/Down: Move   A: Enter/Download   B: Up   START: Exit\n\n");

    if(!L || L->count==0){ printf("(Empty)\n"); goto done; }

    const int visibleRows = 27; // ~30 total incl. header
    int top = 0;
    if (L->selected >= visibleRows) top = L->selected - (visibleRows - 1);
    if (top < 0) top = 0;
    int maxTop = (L->count > visibleRows) ? (L->count - visibleRows) : 0;
    if (top > maxTop) top = maxTop;

    for (int i = top; i < L->count && i < top + visibleRows; i++){
        printf("%c %s\n", (i == L->selected ? '>' : ' '), L->items[i].name);
    }

done:
    gfxFlushBuffers();
    gfxSwapBuffers();
}

/* ----- input flushing to avoid immediate exit after screens ----- */

static void flush_input(void){
    for(int i=0;i<3;i++){ hidScanInput(); gspWaitForVBlank(); }
    while(hidKeysHeld()){ hidScanInput(); gspWaitForVBlank(); }
}

/* -------------------- main -------------------- */

int main(int argc,char **argv){
    gfxInitDefault();
    consoleInit(GFX_TOP,NULL);

    fsInit();  // hbmenu mounts sdmc:/ for .3dsx; fsInit is fine

    // Network init
    socBuf=(u32*)memalign(SOC_ALIGN,SOC_BUFFERSIZE);
    if(!socBuf){ printf("memalign failed\n"); goto exit_all; }
    if(socInit(socBuf,SOC_BUFFERSIZE)!=0){ printf("socInit failed\n"); goto exit_all; }

    char ftpHost[64]="192.168.1.50";
    int  ftpPort=5000;
    const char *ftpUser="anonymous";

    if(load_config(ftpHost,sizeof(ftpHost),&ftpPort))
        printf("Loaded config: %s:%d\n",ftpHost,ftpPort);
    else
        printf("Using defaults: %s:%d\n",ftpHost,ftpPort);

    // Pre-connect UI
    while(aptMainLoop()){
        printf("\rConnect to: %-15s:%-5d   ", ftpHost, ftpPort);
        gfxFlushBuffers(); gfxSwapBuffers();
        hidScanInput();
        u32 kDown=hidKeysDown();
        if(kDown & KEY_START) goto net_cleanup;

        if(kDown & KEY_X){
            bool changed=prompt_ip_port(ftpHost,sizeof(ftpHost),&ftpPort);
            printf(changed? "\nSet to %s:%d\n" : "\nUnchanged.\n", ftpHost, ftpPort);
            gfxFlushBuffers(); gfxSwapBuffers();
        }

        if(kDown & KEY_A) break;
        gspWaitForVBlank();
    }

    // Connect control socket
    printf("\nConnecting to %s:%d ...\n", ftpHost, ftpPort);
    gfxFlushBuffers(); gfxSwapBuffers();
    int ctrl=connect_tcp(ftpHost,ftpPort);
    if(ctrl<0){
        printf("Control connect failed.\nPress START to exit.\n");
        gfxFlushBuffers(); gfxSwapBuffers();
        while(aptMainLoop()){ hidScanInput(); if(hidKeysDown()&KEY_START) break; gspWaitForVBlank(); }
        goto net_cleanup;
    }

    char line[1024];
    if(recv_line(ctrl,line,sizeof(line))>0){ printf("%s",line); gfxFlushBuffers(); gfxSwapBuffers(); }

    // Anonymous login
    send_cmd(ctrl,"USER %s",ftpUser);
    if(recv_line(ctrl,line,sizeof(line))>0){ printf("%s",line); gfxFlushBuffers(); gfxSwapBuffers(); }
    int code=get_reply_code(line);
    if(code==331){
        send_cmd(ctrl,"PASS");
        if(recv_line(ctrl,line,sizeof(line))>0){ printf("%s",line); gfxFlushBuffers(); gfxSwapBuffers(); }
        code=get_reply_code(line);
    }
    if(code!=230){ printf("Login failed (code %d)\n",code); gfxFlushBuffers(); gfxSwapBuffers(); goto close_ctrl; }

    // Save config on successful login
    save_config(ftpHost,ftpPort);
    printf("(Saved config: %s:%d)\n", ftpHost, ftpPort);
    gfxFlushBuffers(); gfxSwapBuffers();

    // Current dir and initial listing
    char cwd[512]="/";
    ftp_pwd(ctrl,cwd,sizeof(cwd));

    Listing list={0};
    if(!build_listing(ctrl,cwd,&list)){
        printf("LIST failed.\nPress START.\n");
        gfxFlushBuffers(); gfxSwapBuffers();
        while(aptMainLoop()){ hidScanInput(); if(hidKeysDown()&KEY_START) break; gspWaitForVBlank(); }
        goto close_ctrl;
    }

    render_listing(cwd,&list);
    flush_input();

    // Browser loop
    bool dirty = false;
    while(aptMainLoop()){
        hidScanInput();
        u32 kd = hidKeysDown();
        if(kd & KEY_START) break;

        if((kd & KEY_DUP) && list.count>0 && list.selected>0){
            list.selected--; dirty = true;
        }
        if((kd & KEY_DDOWN) && list.count>0 && list.selected<list.count-1){
            list.selected++; dirty = true;
        }

        // Up one level
        if(kd & KEY_B){
            if(strcmp(cwd,"/")!=0 && ftp_cdup(ctrl)){
                ftp_pwd(ctrl,cwd,sizeof(cwd));
                build_listing(ctrl,cwd,&list);
                render_listing(cwd,&list);
                flush_input();
                dirty = false;
                continue;
            }
        }

        // A: enter dir or download file to sdmc:/3ds/
        if((kd & KEY_A) && list.count>0){
            const char *name = list.items[list.selected].name;
            if(strcmp(name,"..")==0){
                if(strcmp(cwd,"/")!=0 && ftp_cdup(ctrl)){
                    ftp_pwd(ctrl,cwd,sizeof(cwd));
                    build_listing(ctrl,cwd,&list);
                    render_listing(cwd,&list);
                    flush_input();
                    dirty = false;
                    continue;
                }
            }else if(ftp_cwd(ctrl,name)){
                // It was a directory: refresh view
                ftp_pwd(ctrl,cwd,sizeof(cwd));
                build_listing(ctrl,cwd,&list);
                render_listing(cwd,&list);
                flush_input();
                dirty = false;
                continue;
            }else{
                // Not a dir -> download
                ensure_download_dir();
                char clean[256]; sanitize_name(name, clean, sizeof(clean));
                char dest[512];  snprintf(dest,sizeof(dest),"sdmc:/3ds/%s", clean);
                ftp_retr_to_file(ctrl, name, dest);

                printf("Press any key to continue...\n");
                gfxFlushBuffers(); gfxSwapBuffers();
                flush_input();

                // re-render listing after message
                render_listing(cwd,&list);
            }
        }

        if (dirty){
            render_listing(cwd,&list);
            dirty = false;
        }

        gspWaitForVBlank();
    }

    // Quit cleanly
    send_cmd(ctrl,"QUIT");
    recv_line(ctrl,line,sizeof(line)); // ignore text

close_ctrl:
    close(ctrl);
net_cleanup:
    socExit();
exit_all:
    if(socBuf) free(socBuf);
    fsExit();
    gfxExit();
    return 0;
}
