#include "../asset_read.h"
#include <enDjinn/enj_ctrl.h>
#include <enDjinn/enj_qfont.h>
#include <enDjinn/enj_render.h>
#include <enDjinn/enj_mode.h>
#include <enDjinn/enj_state.h>
#include <aicaflow/host.h>
#include <aicaflow/codec.h>
#include <aicaflow/bank.h>
#if !defined(PLAYER_SHARED_BANK_FILE) && !defined(PLAYER_SONG_BANK_FILE)
#error "A music player requires PLAYER_SHARED_BANK_FILE or PLAYER_SONG_BANK_FILE"
#endif
#define PLAYER_BANKED 1
#ifdef PLAYER_ROOM_PROGRAM
#include <aicaflow/dsp.h>
#endif
#include <malloc.h>
#include <stdalign.h>
#include <string.h>
#include "songs.h"

#ifndef PLAYER_TITLE
#error "PLAYER_TITLE is required"
#endif
#ifndef PLAYER_MODE_NAME
#error "PLAYER_MODE_NAME is required"
#endif
#ifndef PLAYER_SELFTEST_CASES
#error "PLAYER_SELFTEST_CASES is required"
#endif
#ifndef PLAYER_SELFTEST_EXIT_SONG
#error "PLAYER_SELFTEST_EXIT_SONG is required"
#endif
#ifndef PLAYER_SONG_ROWS
#define PLAYER_SONG_ROWS 21
#endif
#ifndef PLAYER_EXPECTED_SONGS
#error "PLAYER_EXPECTED_SONGS is required"
#endif
#ifndef PLAYER_VIZ_FROM_INDEX
#define PLAYER_VIZ_FROM_INDEX 0
#endif
#ifndef PLAYER_SONG_GAIN
#define PLAYER_SONG_GAIN(index) 255u
#endif
#ifndef PLAYER_VISUAL_FILE
#define PLAYER_VISUAL_FILE(index) NULL
#endif
#ifndef PLAYER_LIST_BYTES
#define PLAYER_LIST_BYTES(index) songs[(index)].bytes
#endif
#ifndef PLAYER_LIST_HEADING
#define PLAYER_LIST_HEADING "SIZE KiB"
#endif

static afx_asset_t asset;
static afx_instance_t instance;
static afx_bank_t player_bank;
static int selected, playing = -1, loaded = -1;
static bool room_cached, test_exit, paused;
static uint8_t last_ltrigger, last_rtrigger;
static uint32_t paused_ms;
static uint32_t rate_num, rate_den, duration_ms, started;
static char message[80] = "Select a song and press A";
static uint8_t *visual_data, *visual;
static uint32_t visual_frames;
static FILE *visual_file;
static uint8_t *visual_loading_data;
static size_t visual_loading_size, visual_loading_bytes;
static uint8_t spectrum_bars[32];
static uint32_t spectrum_frame = UINT32_MAX;

enum { SONG_COUNT = sizeof(songs) / sizeof(*songs), SONG_ROWS = PLAYER_SONG_ROWS,
       VISUAL_HEADER_BYTES = 12, VISUAL_BANDS = 32, VISUAL_RATE = 60,
       SPECTRUM_X = 350, SPECTRUM_TOP = 3, SPECTRUM_HEIGHT = 19,
       SPECTRUM_SUBLEVELS = 4, SPECTRUM_DECAY = 5, VISUAL_READ_BYTES = 32768,
       TRIGGER_PRESSED = 128 };
_Static_assert(SONG_COUNT == PLAYER_EXPECTED_SONGS && SONG_ROWS < SONG_COUNT, "playlist layout");

static void visual_cancel(void);
static void update(void *unused);
static enj_mode_t mode = {.mode_updater=update, .name=PLAYER_MODE_NAME};

static void text(unsigned line, const char *s) {
    enj_qfont_write(s,10,line*16,PVR_LIST_PT_POLY);
}
static void progress_bar(char bar[23], uint32_t done, uint32_t total) {
    unsigned filled=total ? (unsigned)((uint64_t)done*20/total) : 0;
    if (filled>20) filled=20;
    bar[0]='[';
    for (unsigned i=0;i<20;++i) bar[i+1]=i<filled ? '=' : '-';
    bar[21]=']'; bar[22]=0;
}
static void loading_progress(uint32_t done, uint32_t total) {
    char bar[23], line[80];
    progress_bar(bar,done,total);
    snprintf(line,sizeof(line),"%s %u%%",bar,total ? (unsigned)((uint64_t)done*100/total) : 0);
    text(28,line);
}
static uint32_t playback_ms(void) {
    uint32_t now=afx_status_timer_ticks();
    return (int32_t)(now-started)>0 ? now-started : 0;
}
static void spectrum(uint32_t ms) {
    if (!visual || !visual_frames) return;
    uint32_t frame=(uint32_t)((uint64_t)ms*VISUAL_RATE/1000);
    if (frame>=visual_frames) frame=visual_frames-1;
    if (frame!=spectrum_frame) {
        uint32_t elapsed=1;
        if (spectrum_frame!=UINT32_MAX && frame<spectrum_frame) memset(spectrum_bars,0,sizeof(spectrum_bars));
        else if (spectrum_frame!=UINT32_MAX) elapsed=frame-spectrum_frame;
        if (elapsed>VISUAL_RATE) elapsed=VISUAL_RATE;
        for (unsigned band=0;band<VISUAL_BANDS;++band) {
            unsigned target=visual[frame*VISUAL_BANDS+band], decay=SPECTRUM_DECAY*elapsed;
            unsigned dropped=spectrum_bars[band]>decay ? spectrum_bars[band]-decay : 0;
            spectrum_bars[band]=target>dropped ? target : dropped;
        }
        spectrum_frame=frame;
    }
    for (unsigned band=0;band<VISUAL_BANDS;++band) {
        if (band<4) enj_qfont_color_set(130,70,255);
        else if (band<8) enj_qfont_color_set(40,130,255);
        else if (band<12) enj_qfont_color_set(20,220,255);
        else if (band<16) enj_qfont_color_set(30,245,140);
        else if (band<20) enj_qfont_color_set(150,255,50);
        else if (band<24) enj_qfont_color_set(255,230,40);
        else if (band<28) enj_qfont_color_set(255,140,30);
        else enj_qfont_color_set(255,50,100);
        unsigned fill=(spectrum_bars[band]*SPECTRUM_HEIGHT*SPECTRUM_SUBLEVELS+254)/255;
        unsigned height=fill/SPECTRUM_SUBLEVELS, partial=fill%SPECTRUM_SUBLEVELS;
        for (unsigned row=0;row<height;++row)
            enj_qfont_write("#",SPECTRUM_X+(int)band*9,(SPECTRUM_TOP+SPECTRUM_HEIGHT-1-(int)row)*16,PVR_LIST_PT_POLY);
        if (partial) {
            static const char edge[] = ".:*";
            char cell[] = {edge[partial-1],0};
            enj_qfont_write(cell,SPECTRUM_X+(int)band*9,(SPECTRUM_TOP+SPECTRUM_HEIGHT-1-(int)height)*16,PVR_LIST_PT_POLY);
        }
    }
}
static void render(void *unused) {
    (void)unused;
    enj_qfont_color_set(230,230,230);
    text(0,PLAYER_TITLE);
    enj_qfont_color_set(180,180,180);
    enj_qfont_write("SPECTRUM 32 x 60 Hz",SPECTRUM_X,2*16,PVR_LIST_PT_POLY);
    enj_qfont_color_set(230,230,230);
    text(1,PLAYER_LIST_HEADING);
    int first=selected-SONG_ROWS/2;
    if (first<0) first=0;
    if (first>SONG_COUNT-SONG_ROWS) first=SONG_COUNT-SONG_ROWS;
    for (int row=0;row<SONG_ROWS;++row) {
        int i=first+row;
        char line[42];
        unsigned kib=(PLAYER_LIST_BYTES(i)+1023)/1024;
        if (kib>9999) kib=9999;
        snprintf(line,sizeof(line),"%c %c %.28s %4u KiB",i==selected ? '>' : ' ',i==playing ? '*' : ' ',
                 songs[i].title, kib);
        text(row+2,line);
    }
    char line[100];
    snprintf(line, sizeof(line), "%c %c %s", selected == playing ? '*' : '>',
             playing >= 0 ? '*' : ' ', songs[selected].title);
#ifdef PLAYER_SONG_PROFILE
    {
        size_t used=strlen(line);
        snprintf(line+used,sizeof(line)-used,"  DSP %s  gain %u/255",
                 PLAYER_SONG_PROFILE(selected),(unsigned)PLAYER_SONG_GAIN(selected));
    }
#endif
    text(26, line);
    unsigned command_ratio = songs[selected].stream_bytes + songs[selected].setup_bytes;
    command_ratio = command_ratio ? (unsigned)(((uint64_t)songs[selected].command_baseline_bytes * 10 + command_ratio / 2) / command_ratio) : 10;
    snprintf(line, sizeof(line), "AICA %luK CHNLS %u S %luK/%u STR %luK TPL %luK %u/%lu %u.%ux",
             (unsigned long)((songs[selected].bytes + 1023) / 1024), (unsigned)songs[selected].channel_count,
             (unsigned long)((songs[selected].sample_bytes + 1023) / 1024), (unsigned)songs[selected].sample_count,
             (unsigned long)((songs[selected].stream_bytes + 1023) / 1024),
             (unsigned long)((songs[selected].setup_bytes + 1023) / 1024), (unsigned)songs[selected].setup_count,
             (unsigned long)songs[selected].note_count, command_ratio / 10, command_ratio % 10);
    text(27, line);
    char time[80]="";
    uint32_t ms=0;
    char bar[23];
    if (playing>=0) {
        ms=paused ? paused_ms : playback_ms();
        progress_bar(bar,ms,duration_ms);
        snprintf(time,sizeof(time),"%s %lu:%02lu / %lu:%02lu",bar,(unsigned long)(ms/60000),(unsigned long)(ms/1000%60),(unsigned long)(duration_ms/60000),(unsigned long)(duration_ms/1000%60));
    }
    spectrum(ms);
    enj_qfont_color_set(230,230,230);
    if (visual_loading_data) loading_progress(visual_loading_bytes,visual_loading_size);
    else text(28, playing >= 0 ? time : message);
    text(29,"UP/DOWN select  A play/pause  B stop  L/R page  LEFT/RIGHT seek 10s");
}

static FILE *open_asset(const char *name) {
    char path[160];
    snprintf(path, sizeof(path), "/pc/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(path, sizeof(path), ENJ_CBASEPATH "%s", name); f = fopen(path, "rb"); }
    if (f) setvbuf(f,NULL,_IONBF,0);
    return f;
}
#ifdef PLAYER_SHARED_BANK_FILE
static int shared_bank_load(void) {
    char path[160];
    snprintf(path,sizeof(path),"/pc/%s",PLAYER_SHARED_BANK_FILE);
    int result=afx_bank_load_file(&player_bank,path);
    if (!result) return result;
    snprintf(path,sizeof(path),ENJ_CBASEPATH "%s",PLAYER_SHARED_BANK_FILE);
    return afx_bank_load_file(&player_bank,path);
}
#endif
#ifdef PLAYER_BANKED
#ifdef PLAYER_SONG_BANK_FILE
static int song_bank_load(const char *name) {
    char path[160];
    snprintf(path,sizeof(path),"/pc/%s",name);
    int result=afx_bank_load_file(&player_bank,path);
    if (!result) return result;
    snprintf(path,sizeof(path),ENJ_CBASEPATH "%s",name);
    return afx_bank_load_file(&player_bank,path);
}
#endif
static int bank_control_load(const char *name, afx_asset_t *out) {
    FILE *file=open_asset(name);
    if (!file || fseek(file,0,SEEK_END)) { if (file) fclose(file); return -AFX_BAD_BOUNDS; }
    long length=ftell(file);
    if (length<=0 || length>AFX_ASSET_LIMIT || fseek(file,0,SEEK_SET)) { fclose(file); return -AFX_BAD_BOUNDS; }
    uint8_t *data=memalign(32,(size_t)length);
    if (!data) { fclose(file); return -AFX_NO_HOST_RAM; }
    int result = -AFX_BAD_BOUNDS;
    if (asset_read(data,(size_t)length,file)==(size_t)length && !ferror(file)) {
        uint64_t ticks;
        result=afx_flow_duration(data,(uint32_t)length,&ticks,&rate_num,&rate_den);
        if (!result) {
            duration_ms=(uint32_t)(ticks*1000u*rate_den/rate_num);
            result=afx_bank_flow_upload(&player_bank,data,(uint32_t)length,out);
        }
    }
    fclose(file); free(data);
    if (result || !*out) return result;
    char index_name[160];
    size_t name_bytes=strlen(name);
    if (name_bytes < 5 || name_bytes >= sizeof(index_name) || strcmp(name + name_bytes - 4,".afx"))
        return -AFX_BAD_BOUNDS;
    memcpy(index_name,name,name_bytes + 1);
    memcpy(index_name + name_bytes - 4,".afc",5);
    file=open_asset(index_name);
    if (!file) return AFX_OK; /* Seeking is optional; normal playback needs only AFB+AFX. */
    if (fseek(file,0,SEEK_END)) { fclose(file); return -AFX_BAD_BOUNDS; }
    length=ftell(file);
    if (length<=0 || length>AFX_ASSET_LIMIT || fseek(file,0,SEEK_SET)) { fclose(file); return -AFX_BAD_BOUNDS; }
    data=memalign(32,(size_t)length);
    if (!data) { fclose(file); return -AFX_NO_HOST_RAM; }
    if (asset_read(data,(size_t)length,file)!=(size_t)length || ferror(file)) result=-AFX_BAD_BOUNDS;
    else result=afx_flow_seek_index_load_memory(*out,data,(uint32_t)length);
    fclose(file); free(data);
    if (result) { (void)afx_asset_free(*out); *out=0; }
    return result;
}
#endif
static int load_firmware(void) {
    FILE *f=open_asset("aicaflow.drv");
    if (!f) return -AFX_BAD_FIRMWARE;
    long size=fseek(f,0,SEEK_END) ? -1 : ftell(f);
    int r=-AFX_BAD_FIRMWARE;
    uint8_t *data=NULL;
    if (size>=AFX_FIRMWARE_INFO_OFFSET+AFX_FIRMWARE_INFO_BYTES &&
        size<=AFX_ASSET_LIMIT && !fseek(f,0,SEEK_SET)) {
        data=memalign(32,size);
        if (!data) r=-AFX_NO_HOST_RAM;
        else if (asset_read(data,size,f)==(size_t)size && !ferror(f))
            r=afx_init(data,(uint32_t)size);
    }
    fclose(f);
    free(data);
    return r;
}
static int wait_state(uint32_t wanted) {
    for (unsigned i=0; i<2000; ++i) {
        afx_instance_status_t s;
        if (afx_update()<0) return -AFX_TIMEOUT;
        if (!afx_instance_status(instance, &s)) {
            if (s.result) return -(int)s.result;
            if (s.state==wanted) return 0;
            if (s.state==AFX_ERROR || s.state==AFX_DONE) return -AFX_BAD_COMMAND;
        }
        thd_sleep(1);
    }
    return -AFX_TIMEOUT;
}
static int stop(void) {
    visual_cancel();
    if (instance) {
        afx_instance_status_t s;
        if (afx_update()<0 || afx_instance_status(instance, &s)) return -AFX_TIMEOUT;
        if (s.state!=AFX_DONE && s.state!=AFX_ERROR) {
            int r=afx_instance_stop(instance);
            if (r) return r;
            for (unsigned i=0; i<2000; ++i) {
                afx_update();
                if (!afx_instance_status(instance, &s) && (s.state==AFX_DONE || s.state==AFX_ERROR)) break;
                thd_sleep(1);
            }
            if (s.state!=AFX_DONE && s.state!=AFX_ERROR) return -AFX_TIMEOUT;
        }
        int r=afx_instance_recycle(instance);
        if (r) return r;
        unsigned i;
        for (i=0; i<2000; ++i) {
            afx_update();
            if (afx_instance_status(instance, &s)==-AFX_STALE_GENERATION) break;
            thd_sleep(1);
        }
        if (i==2000) return -AFX_TIMEOUT;
        instance=0;
    }
    playing=-1;
    paused=false;
    return 0;
}
static int unload(void) {
    int r=stop();
    if (r) return r;
    if (asset) {
        r = afx_asset_free(asset);
        if (r) return r;
    }
#ifdef PLAYER_SONG_BANK_FILE
    if (player_bank.asset) {
        r = afx_bank_release(&player_bank);
        if (r) return r;
    }
#endif
    if (room_cached) (void)afx_dsp_scene_disable();
    asset=0; loaded=-1; room_cached=false;
    free(visual_data); visual_data=visual=NULL; visual_frames=0;
    return 0;
}
static void visual_cancel(void) {
    if (visual_file) fclose(visual_file);
    visual_file=NULL;
    free(visual_loading_data); visual_loading_data=NULL;
    visual_loading_size=visual_loading_bytes=0;
}
static void load_visual_begin(int index) {
    visual_cancel();
    free(visual_data); visual_data=visual=NULL; visual_frames=0;
    memset(spectrum_bars,0,sizeof(spectrum_bars)); spectrum_frame=UINT32_MAX;
    char name[64];
    const char *path=PLAYER_VISUAL_FILE(index);
    if (path) snprintf(name,sizeof(name),"%s",path);
    else {
#if PLAYER_VIZ_FROM_INDEX
        snprintf(name,sizeof(name),"%02d.afv",index+1);
#else
        snprintf(name,sizeof(name),"%s",songs[index].file);
        char *extension=strrchr(name,'.');
        if (!extension) return;
        snprintf(extension,5,".afv");
#endif
    }
    FILE *file=open_asset(name);
    if (!file || fseek(file,0,SEEK_END)) { if (file) fclose(file); return; }
    long size=ftell(file);
    if (size<VISUAL_HEADER_BYTES || fseek(file,0,SEEK_SET)) { fclose(file); return; }
    visual_loading_data=memalign(32,size);
    if (!visual_loading_data) { fclose(file); return; }
    visual_file=file; visual_loading_size=(size_t)size;
}
static void load_visual_chunk(void) {
    if (!visual_file) return;
    size_t count=visual_loading_size-visual_loading_bytes;
    if (count>VISUAL_READ_BYTES) count=VISUAL_READ_BYTES;
    size_t got=asset_read(visual_loading_data+visual_loading_bytes,count,visual_file);
    if (!got || ferror(visual_file)) { visual_cancel(); return; }
    visual_loading_bytes+=got;
    if (visual_loading_bytes<visual_loading_size) return;
    fclose(visual_file); visual_file=NULL;
    uint8_t *data=visual_loading_data;
    size_t size=visual_loading_size;
    visual_loading_data=NULL; visual_loading_size=visual_loading_bytes=0;
    uint32_t frames=(uint32_t)data[8]|(uint32_t)data[9]<<8|(uint32_t)data[10]<<16|(uint32_t)data[11]<<24;
    if (memcmp(data,"VIZ1",4) || data[4]!=1 || data[5]!=VISUAL_BANDS ||
        data[6]!=VISUAL_RATE || data[7] || !frames || frames>(uint32_t)(size-VISUAL_HEADER_BYTES)/VISUAL_BANDS ||
        size!=VISUAL_HEADER_BYTES+(size_t)frames*VISUAL_BANDS) { free(data); return; }
    free(visual_data); visual_data=data; visual=data+VISUAL_HEADER_BYTES; visual_frames=frames;
}
static int room(const char *name) {
    if (room_cached) return AFX_OK;
#ifdef PLAYER_ROOM_PROGRAM
    (void)name;
    afx_dsp_program_t program;
    int r=PLAYER_ROOM_PROGRAM(&program);
    if (!r) r=afx_dsp_scene_program(&program,sizeof(program));
    if (!r) room_cached=true;
    return r;
#else
    alignas(32) static uint8_t data[AFX_DSP_PROGRAM_BYTES];
    {
        FILE *f=open_asset(name);
        if (!f) return -AFX_BAD_BOUNDS;
        size_t n=asset_read(data,sizeof(data),f);
        int extra=fgetc(f); fclose(f);
        if (n!=sizeof(data) || extra!=EOF) return -AFX_BAD_BOUNDS;
    }
    int r=afx_dsp_scene_program(data,sizeof(data));
    if (!r) room_cached=true;
    return r;
#endif
}
static int start(void) {
    int index=loaded;
    int r=afx_instance_activate(asset,&instance);
    if (!r) r=wait_state(AFX_RUNNING);
    if (!r) r=afx_instance_gain(instance,PLAYER_SONG_GAIN(index));
    started=afx_instance_start_tick(instance);
#ifdef PLAYER_SHARED_BANK_FILE
    if (!r && songs[index].dsp) r=room(songs[index].dsp);
    if (!r && room_cached) r=afx_dsp_scene_returns(songs[index].dsp != NULL);
#else
    if (!r && songs[index].dsp) r=room(songs[index].dsp);
#endif
    if (r) { stop(); return r; }
    playing=index;
    snprintf(message,sizeof(message),"Playing: %s",songs[index].title);
    load_visual_begin(index);
    printf("PLAYER PLAY %d\n",index+1);
    return 0;
}
static int play(int index) {
    if (index==loaded && asset) {
        int r=stop();
        return r ? r : start();
    }
    int r=unload();
    if (r) return r;
#ifdef PLAYER_SONG_BANK_FILE
    r=song_bank_load(PLAYER_SONG_BANK_FILE(index));
    if (r) return r;
#endif
    r=bank_control_load(songs[index].file,&asset);
    if (r) {
#ifdef PLAYER_SONG_BANK_FILE
        (void)afx_bank_release(&player_bank);
#endif
        return r;
    }
    loaded=index;
    return start();
}

static int seek(int seconds) {
    if (playing<0) return 0;
    int64_t target=(int64_t)(paused ? paused_ms : playback_ms())+seconds*1000;
    if (target<0) target=0;
    if (target>=duration_ms) {
        int r=stop();
        if (!r) snprintf(message,sizeof(message),"End of song - select next song and press A");
        return r;
    }
    if (paused) { paused_ms=(uint32_t)target; return 0; }
    int r=afx_instance_pause(instance);
    if (!r) r=wait_state(AFX_PAUSED);
    if (!r) r=afx_instance_seek(instance,(uint32_t)((uint64_t)target*rate_num/(1000ull*rate_den)));
    if (!r) r=wait_state(AFX_RUNNING);
    if (!r) started=afx_status_timer_ticks()-(uint32_t)target;
    printf("PLAYER SEEK %lld result=%d\n",(long long)target,r);
    return r;
}
static int finish(uint32_t state) {
    int current=playing, next=current+1;
    int r=stop();
    if (r) return r;
    if (state==AFX_DONE && songs[current].wraps) { selected=0; return play(0); }
    if (state==AFX_DONE && next<SONG_COUNT) {
        selected=next;
        return play(next);
    }
    snprintf(message,sizeof(message),state==AFX_DONE ? "End of playlist" : "Playback error - press A to retry");
    return 0;
}
static int toggle(void) {
    if (playing!=selected) return play(selected);
    int r;
    if (paused) {
        r=afx_instance_seek(instance,(uint32_t)((uint64_t)paused_ms*rate_num/(1000ull*rate_den)));
        if (!r) r=wait_state(AFX_RUNNING);
        if (!r) { started=afx_status_timer_ticks()-paused_ms; paused=false; }
    } else {
        r=afx_instance_pause(instance);
        if (!r) r=wait_state(AFX_PAUSED);
        if (!r) { paused_ms=playback_ms(); paused=true; }
    }
    if (!r) snprintf(message,sizeof(message),"%s: %s",paused ? "Paused" : "Playing",songs[playing].title);
    return r;
}
#ifdef PLAYER_FRAME_TEST
#ifndef PLAYER_FRAME_TEST_HEADER
#error "PLAYER_FRAME_TEST_HEADER is required with PLAYER_FRAME_TEST"
#endif
#include PLAYER_FRAME_TEST_HEADER
#endif
int main(void) {
    enj_state_init_defaults();
    if (enj_state_startup()) return 1;
    enj_qfont_color_set(230,230,230);
    int r=load_firmware();
    printf("PLAYER FIRMWARE result=%d\n",r);
#ifdef PLAYER_SHARED_BANK_FILE
    if (!r) r=shared_bank_load();
    printf("PLAYER BANK result=%d\n",r);
#endif
    if (r) return 1;
#ifdef PLAYER_FRAME_TEST
    selected=69;
    if (play(selected)) return 1;
#endif
    if (!enj_mode_push(&mode)) { afx_shutdown(); return 1; }
    enj_state_run();
    unload();
#ifdef PLAYER_BANKED
    (void)afx_bank_release(&player_bank);
#endif
    afx_shutdown();
    printf("PLAYER EXITED\n");
    return 0;
}
static void update(void *unused) {
    (void)unused;
    if (enj_state_get()->flags.shut_down) return;
    afx_update();
    enj_ctrlr_state_t **states=enj_ctrl_get_states(), *pad=NULL;
    for (size_t i=0;i<enj_ctrl_states_length() && !pad;++i) pad=states[i];
#ifdef PLAYER_FRAME_TEST
    pad=frame_test_input();
#endif
    int r=0;
    if (pad) {
        bool ltrigger=pad->ltrigger>=TRIGGER_PRESSED && last_ltrigger<TRIGGER_PRESSED;
        bool rtrigger=pad->rtrigger>=TRIGGER_PRESSED && last_rtrigger<TRIGGER_PRESSED;
        last_ltrigger=pad->ltrigger; last_rtrigger=pad->rtrigger;
        if (pad->button.UP==ENJ_BUTTON_DOWN_THIS_FRAME) selected=(selected+SONG_COUNT-1)%SONG_COUNT;
        if (pad->button.DOWN==ENJ_BUTTON_DOWN_THIS_FRAME) selected=(selected+1)%SONG_COUNT;
        if (ltrigger) selected=(selected+SONG_COUNT-SONG_ROWS)%SONG_COUNT;
        if (rtrigger) selected=(selected+SONG_ROWS)%SONG_COUNT;
        if (pad->button.B==ENJ_BUTTON_DOWN_THIS_FRAME) { r=stop(); snprintf(message,sizeof(message),"Stopped"); }
        else if (pad->button.A==ENJ_BUTTON_DOWN_THIS_FRAME) r=toggle();
        else if (pad->button.LEFT==ENJ_BUTTON_DOWN_THIS_FRAME) r=seek(-10);
        else if (pad->button.RIGHT==ENJ_BUTTON_DOWN_THIS_FRAME) r=seek(10);
    } else last_ltrigger=last_rtrigger=0;
    if (!r && visual_file) load_visual_chunk();
    if (r) { stop(); snprintf(message,sizeof(message),"Could not play/seek (%d). Press A to retry.",r); }
    if (playing>=0) {
        afx_instance_status_t s;
        if (!afx_instance_status(instance,&s) && (s.state==AFX_DONE || s.state==AFX_ERROR)) {
            r=finish(s.state);
            if (r) { stop(); snprintf(message,sizeof(message),"Could not load next song (%d). Press A to retry.",r); }
        }
    }
    enj_render_list_add(PVR_LIST_PT_POLY, render, NULL);
    if (test_exit) enj_state_flag_shutdown(NULL);
}
