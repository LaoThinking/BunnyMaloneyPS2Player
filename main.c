#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <packet.h>
#include <graph.h>
#include <gsKit.h>
#include <gsToolkit.h>
#include <dmaKit.h>
#include <loadfile.h>
#include <malloc.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <libpad.h>
#include <audsrv.h>
#include <timer.h>
#include <libmc.h>
#include <fcntl.h>

#define MAX_VOLUME 100

#ifndef PAD_TRIANGLE
#define PAD_TRIANGLE 0x1000
#endif
#ifndef PAD_START
#define PAD_START    0x0008
#endif
#ifndef PAD_L3
#define PAD_L3       0x0002
#endif
#ifndef PAD_R3
#define PAD_R3       0x0004
#endif
#ifndef PAD_STATE_READY
#define PAD_STATE_READY 0x06
#endif

void get_filepath(char *dest, const char *filename) {
    /* WATCHON_TRAX_V4_1_PATH_AUDIO_FIX: V4 importer writes artwork to
       project/Music/, not project/Textures/Music/. */
    if (filename && strncmp(filename, "Music/", 6) == 0)
        snprintf(dest, 512, "host:%s", filename);
    else
        snprintf(dest, 512, "host:Textures/%s", filename);
}

void get_videopath(char *dest, const char *filename) {
    snprintf(dest, 512, "host:Audio/%s", filename);
}

void show_debug_screen(GSGLOBAL *gsGlobal, u8 r, u8 g, u8 b) {
    printf("[STEP] Drawing clear screen RGB(%d,%d,%d)...\n", r, g, b);
    fflush(stdout);

    gsKit_clear(gsGlobal, GS_SETREG_RGBAQ(r, g, b, 0x80, 0x00));
    gsKit_queue_exec(gsGlobal);
    gsKit_sync_flip(gsGlobal);

    printf("[STEP] Screen flip complete.\n");
    fflush(stdout);
}

/* Service the menu music stream while individual textures are loaded.
   Implementation appears later; it is inactive when no music is playing. */
static void watchon_music_pump(void);

int load_ui_texture(GSGLOBAL *gsGlobal, GSTEXTURE *tex, const char *filename) {
    char path[512];

    get_filepath(path, filename);
    memset(tex, 0, sizeof(GSTEXTURE));

    printf("[TEXTURE] Loading: %s...\n", path);
    fflush(stdout);

    /*
     * Normal gsKit BMP loading does all of this automatically:
     *
     * 1. Reads the BMP into EE memory
     * 2. Allocates GS VRAM
     * 3. Uploads the texture
     * 4. Frees tex->Mem
     *
     * Therefore tex->Mem == NULL after a successful load is NORMAL.
     */
    int result = gsKit_texture_bmp(gsGlobal, tex, path);

    if (result < 0 || tex->Vram == GSKIT_ALLOC_ERROR) {
        printf("[ERROR] Failed to load BMP: %s (Result: %d)\n",
               path, result);
        fflush(stdout);

        tex->Vram = 0;
        return 0;
    }

    printf("[TEXTURE] OK: %s (%dx%d, PSM=%d, VRAM=0x%X)\n",
           filename,
           tex->Width,
           tex->Height,
           tex->PSM,
           tex->Vram);
    fflush(stdout);

    /* V4.1: resource loading can span several audio frames. Feed the PCM
       queue between texture uploads; never wait for audio buffer space. */
    watchon_music_pump();
    return 1;
}

void play_video_file(GSGLOBAL *gsGlobal, const char *audio_filename, const char *folder_name);

static int audsrv_initialized = 0;

/*
 * Used by Play All.
 *
 * 0 = playback was cancelled / failed
 * 1 = video and audio finished normally
 */
static int playback_completed_naturally = 0;

/*
 * OPTIONS:
 *
 * 1 = L3 video skip enabled
 * 0 = L3 video skip disabled
 *
 * Default is OFF. When a valid Slot 1 memory card is available,
 * this value is loaded from/saved to that card.
 */
static int option_l3_skip_enabled = 0;

void init_audio_subsystem(void) {
    if (!audsrv_initialized) {
        printf("[STEP] Loading rom0:LIBSD...\n");
        fflush(stdout);
        SifLoadModule("rom0:LIBSD", 0, NULL);

        printf("[STEP] Loading host:audsrv.irx...\n");
        fflush(stdout);
        int ret = SifLoadModule("host:audsrv.irx", 0, NULL);
        if (ret < 0) {
            printf("[WARNING] Could not load host:audsrv.irx (ret = %d). Audio disabled.\n", ret);
            fflush(stdout);
            return;
        }

        printf("[STEP] Initializing audsrv...\n");
        fflush(stdout);
        if (audsrv_init() == 0) {
            audsrv_initialized = 1;
            printf("[STEP] Audio subsystem initialized successfully.\n");
            fflush(stdout);
        } else {
            printf("[WARNING] audsrv_init failed!\n");
            fflush(stdout);
        }
    }
}

/* BUNNY_UI_SFX_BEGIN: mono ADPCM, bounded non-overlapping menu effects */
#define UI_SFX_CONFIRM  0
#define UI_SFX_BACK     1
#define UI_SFX_MOVE     2
#define UI_SFX_BOOT     3
#define UI_SFX_COUNT    4
#define UI_SFX_VOICE_BASE 5
#define UI_SFX_VOLUME 48
#define UI_SFX_NAV_VOICES 6
#define UI_SFX_NAV_VOLUME 42
#define UI_SFX_BOOT_VOICE 13
#define UI_SFX_BOOT_VOLUME 48

static audsrv_adpcm_t ui_sfx_samples[UI_SFX_COUNT];
static int ui_sfx_loaded[UI_SFX_COUNT] = {0, 0, 0, 0};
static int ui_sfx_initialized = 0;

static void ui_sfx_load_one(int which, const char *filename)
{
    char path[128];
    FILE *fp;
    long size;
    u8 *bytes;
    int ret;

    snprintf(path, sizeof(path), "host:Audio/UI/%s", filename);
    fp = fopen(path, "rb");
    if (!fp) {
        printf("[UI SFX] Missing %s (sound disabled)\n", path);
        return;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return;
    }
    size = ftell(fp);
    if (size < 32 || size > 262144 || fseek(fp, 0, SEEK_SET) != 0) {
        printf("[UI SFX] Invalid size: %s\n", path);
        fclose(fp);
        return;
    }

    /* SIF/DMA transfer buffers benefit from 64-byte alignment. */
    bytes = (u8 *)memalign(64, (size_t)size);
    if (!bytes) {
        fclose(fp);
        return;
    }

    if (fread(bytes, 1, (size_t)size, fp) != (size_t)size) {
        printf("[UI SFX] Read failed: %s\n", path);
        free(bytes);
        fclose(fp);
        return;
    }
    fclose(fp);

    /* APCM header byte 5 is the channel count: force 1, not stereo. */
    if (memcmp(bytes, "APCM", 4) != 0 || bytes[5] != 1) {
        printf("[UI SFX] Invalid/non-mono ADPCM: %s (channels=%u)\n",
               path, (unsigned int)bytes[5]);
        free(bytes);
        return;
    }

    ret = audsrv_load_adpcm(&ui_sfx_samples[which], bytes, (int)size);
    free(bytes);
    if (ret < 0) {
        printf("[UI SFX] Upload failed (%d): %s\n", ret, path);
        return;
    }

    ui_sfx_loaded[which] = 1;
    printf("[UI SFX] Loaded MONO %s (%ld bytes)\n", path, size);
}

static void ui_sfx_init(void)
{
    int ret;
    int which;
    if (ui_sfx_initialized || !audsrv_initialized)
        return;
    ui_sfx_initialized = 1;

    ret = audsrv_adpcm_init();
    if (ret < 0) {
        printf("[UI SFX] ADPCM init failed (%d); UI sounds disabled.\n", ret);
        return;
    }

    ui_sfx_load_one(UI_SFX_CONFIRM, "confirm.adp");
    ui_sfx_load_one(UI_SFX_BACK, "back.adp");
    ui_sfx_load_one(UI_SFX_MOVE, "navigate.adp");
    ui_sfx_load_one(UI_SFX_BOOT, "loading_switch.adp");

    /* Confirm / Back keep their original fixed voices and volume. */
    for (which = UI_SFX_CONFIRM; which <= UI_SFX_BACK; ++which)
        audsrv_adpcm_set_volume_and_pan(
            UI_SFX_VOICE_BASE + which, UI_SFX_VOLUME, 0);

    /* Preconfigure every navigation voice before the first input.
       Multiple already-uploaded copies are NOT needed: only voices. */
    for (which = 0; which < UI_SFX_NAV_VOICES; ++which)
        audsrv_adpcm_set_volume_and_pan(
            UI_SFX_VOICE_BASE + UI_SFX_MOVE + which,
            UI_SFX_NAV_VOLUME, 0);

    /* Dedicated voice 13, outside confirm/back and navigation voices 5..12. */
    audsrv_adpcm_set_volume_and_pan(
        UI_SFX_BOOT_VOICE, UI_SFX_BOOT_VOLUME, 0);
}

static void ui_sfx_play(int which)
{
    static int nav_next_voice = 0;
    int voice;
    int attempt;
    int candidate;
    int result;

    if (which < 0 || which >= UI_SFX_COUNT || !ui_sfx_loaded[which])
        return;

    if (which == UI_SFX_MOVE) {
        /* Play immediately on the FIRST AVAILABLE navigation voice.
           The previous 1-voice implementation silently rejected
           D-pad presses while its ~300 ms sample was still playing.

           Cycle through 6 voices: no busy-wait, queue, timer,
           audio reset or effect re-upload on button input. */
        for (attempt = 0; attempt < UI_SFX_NAV_VOICES; ++attempt) {
            candidate = (nav_next_voice + attempt) % UI_SFX_NAV_VOICES;
            voice = UI_SFX_VOICE_BASE + UI_SFX_MOVE + candidate;
            result = audsrv_ch_play_adpcm(voice, &ui_sfx_samples[which]);
            if (result >= 0) {
                nav_next_voice = (candidate + 1) % UI_SFX_NAV_VOICES;
                return;
            }
        }
        /* At extreme input rates all 6 voices could be busy. */
        return;
    }

    /* Leave confirm/back exactly as in the working mono version. */
    voice = UI_SFX_VOICE_BASE + which;
    (void)audsrv_ch_play_adpcm(voice, &ui_sfx_samples[which]);
}


/* BUNNY_LOADING_TRANSITION_SFX_V1: called only on a NEW loading banner. */
static void boot_loading_transition_sfx(void)
{
    if (ui_sfx_initialized && ui_sfx_loaded[UI_SFX_BOOT]) {
        /* One short clip per banner change, never per animation frame. */
        (void)audsrv_ch_play_adpcm(
            UI_SFX_BOOT_VOICE, &ui_sfx_samples[UI_SFX_BOOT]);
    }
}

/* BUNNY_UI_SFX_END */

static int pad_initialized = 0;
static char padBuf[256] __attribute__((aligned(64)));

void init_pad_subsystem(void) {
    if (!pad_initialized) {
        printf("[STEP] Loading rom0:XSIO2MAN...\n");
        fflush(stdout);
        SifLoadModule("rom0:XSIO2MAN", 0, NULL);

        printf("[STEP] Loading XPADMAN/PADMAN...\n");
        fflush(stdout);
        if (SifLoadModule("rom0:XPADMAN", 0, NULL) < 0) {
            SifLoadModule("rom0:PADMAN", 0, NULL);
        }
        
        printf("[STEP] Calling padInit(0)...\n");
        fflush(stdout);
        padInit(0);

        printf("[STEP] Calling padPortOpen(0, 0)...\n");
        fflush(stdout);
        if (padPortOpen(0, 0, padBuf) == 0) {
            printf("[WARNING] padPortOpen failed!\n");
            fflush(stdout);
        } else {
            pad_initialized = 1;
            printf("[STEP] Controller driver initialized successfully.\n");
            fflush(stdout);
        }
    }
}


/* ===== MEMORY CARD SETTINGS ===== */
/* 0 = presentation only; original Memory Card code is preserved. */
#define ENABLE_MEMORY_CARD_IO 0

#define SETTINGS_MC_PORT       0
#define SETTINGS_MC_SLOT       0

#define SETTINGS_SAVE_DIR      "/BUNNYMALONEY"
#define SETTINGS_SAVE_GLOB     "/BUNNYMALONEY/*"
#define SETTINGS_SAVE_FILE     "/BUNNYMALONEY/settings.dat"

#define SETTINGS_MAGIC         0x424D5331U
#define SETTINGS_VERSION       1U
#define SETTINGS_CHECK_XOR     0xB04E2026U


typedef struct BunnySettingsSave {

    u32 magic;
    u32 version;

    u32 l3_skip_enabled;

    u32 checksum;

    u8 reserved[48];

} __attribute__((aligned(64)))
BunnySettingsSave;


static int memory_card_initialized =
    0;


/*
 * Tiny integrity check.
 *
 * This is not intended as encryption.
 * It simply lets us detect an incomplete/corrupted settings file
 * and fall safely back to the defaults.
 */
static u32 memory_card_settings_checksum(
    const BunnySettingsSave *save)
{
    return
        save->magic ^
        save->version ^
        save->l3_skip_enabled ^
        SETTINGS_CHECK_XOR;
}


/*
 * All libmc operations except mcInit() are asynchronous.
 *
 * Wait here for the request to actually finish and return
 * its real result.
 */
static int memory_card_wait(
    const char *operation)
{
    int cmd =
        0;

    int result =
        -9999;


    int sync_result =
        mcSync(
            MC_WAIT,
            &cmd,
            &result
        );


    if (sync_result < 0) {

        printf(
            "[MEMCARD ERROR] %s: "
            "mcSync failed (%d).\n",
            operation,
            sync_result
        );

        fflush(stdout);

        return -9999;
    }


    return result;
}


/*
 * Slot 1 Memory Card services.
 *
 * XSIO2MAN is already loaded earlier by the controller
 * subsystem, so only XMCMAN/XMCSERV need loading here.
 */
static int init_memory_card_subsystem(void)
{
    int ret;

#if !ENABLE_MEMORY_CARD_IO
    printf("[BOOT] Content-only mode: Memory Card I/O disabled.\n");
    fflush(stdout);
    return 1;
#endif

    if (memory_card_initialized)
        return 1;


    printf(
        "[MEMCARD] Initializing Slot 1 services...\n"
    );

    fflush(stdout);


    ret =
        SifLoadModule(
            "rom0:XMCMAN",
            0,
            NULL
        );


    if (ret < 0) {

        printf(
            "[MEMCARD ERROR] "
            "Could not load XMCMAN (%d).\n",
            ret
        );

        fflush(stdout);

        return 0;
    }


    ret =
        SifLoadModule(
            "rom0:XMCSERV",
            0,
            NULL
        );


    if (ret < 0) {

        printf(
            "[MEMCARD ERROR] "
            "Could not load XMCSERV (%d).\n",
            ret
        );

        fflush(stdout);

        return 0;
    }


    ret =
        mcInit(
            MC_TYPE_XMC
        );


    if (ret < 0) {

        printf(
            "[MEMCARD ERROR] "
            "mcInit failed (%d).\n",
            ret
        );

        fflush(stdout);

        return 0;
    }


    memory_card_initialized =
        1;


    printf(
        "[MEMCARD] Memory Card subsystem ready.\n"
    );

    fflush(stdout);


    return 1;
}


/*
 * Check:
 *
 *   PS2 Memory Card
 *   Slot 1
 *   formatted
 */
static int memory_card_check_slot(void)
{
    int type =
        MC_TYPE_NONE;

    int free_clusters =
        0;

    int formatted =
        MC_UNFORMATTED;

    int result;

#if !ENABLE_MEMORY_CARD_IO
    return 1;
#endif

    if (!memory_card_initialized)
        return 0;


    if (
        mcGetInfo(
            SETTINGS_MC_PORT,
            SETTINGS_MC_SLOT,

            &type,
            &free_clusters,
            &formatted
        ) < 0
    ) {

        printf(
            "[MEMCARD ERROR] "
            "mcGetInfo request failed.\n"
        );

        fflush(stdout);

        return 0;
    }


    result =
        memory_card_wait(
            "mcGetInfo"
        );


    printf(
        "[MEMCARD] Slot 1: "
        "result=%d type=%d free=%d formatted=%d\n",

        result,
        type,
        free_clusters,
        formatted
    );

    fflush(stdout);


    /*
     * libmc:
     *
     *   0  = same card
     *  -1  = newly detected formatted card
     *  -2  = newly detected unformatted card
     * <-2  = access/card error
     */
    if (
        result < -1 ||
        type != MC_TYPE_PS2 ||
        formatted != MC_FORMATTED
    ) {

        return 0;
    }


    return 1;
}


/*
 * Create our save directory the first time the app is run.
 */
static int memory_card_ensure_settings_directory(void)
{
    static sceMcTblGetDir entry
        __attribute__((aligned(64)));

    int result;


    memset(
        &entry,
        0,
        sizeof(entry)
    );


    /*
     * Search INSIDE the Bunny Maloney directory.
     *
     * mcGetDir is a search operation, so use a wildcard.
     *
     * Result:
     *
     *   >= 0 = directory exists
     *   -4   = path does not exist
     */
    if (
        mcGetDir(
            SETTINGS_MC_PORT,
            SETTINGS_MC_SLOT,

            SETTINGS_SAVE_GLOB,

            0,
            1,

            &entry
        ) < 0
    ) {

        printf(
            "[MEMCARD ERROR] "
            "Directory check request failed.\n"
        );

        fflush(stdout);

        return 0;
    }


    result =
        memory_card_wait(
            "mcGetDir(settings directory)"
        );


    if (result >= 0) {

        printf(
            "[MEMCARD] Settings directory exists.\n"
        );

        fflush(stdout);

        return 1;
    }


    /*
     * Anything except -4 is an actual access/path error.
     */
    if (result != -4) {

        printf(
            "[MEMCARD ERROR] "
            "Directory check failed (%d).\n",
            result
        );

        fflush(stdout);

        return 0;
    }


    printf(
        "[MEMCARD] Creating save directory %s...\n",
        SETTINGS_SAVE_DIR
    );

    fflush(stdout);


    if (
        mcMkDir(
            SETTINGS_MC_PORT,
            SETTINGS_MC_SLOT,
            SETTINGS_SAVE_DIR
        ) < 0
    ) {

        printf(
            "[MEMCARD ERROR] "
            "Directory create request failed.\n"
        );

        fflush(stdout);

        return 0;
    }


    result =
        memory_card_wait(
            "mcMkDir"
        );


    if (result < 0) {

        /*
         * Verify one more time.
         *
         * This safely handles an already-existing directory too.
         */
        memset(
            &entry,
            0,
            sizeof(entry)
        );


        if (
            mcGetDir(
                SETTINGS_MC_PORT,
                SETTINGS_MC_SLOT,

                SETTINGS_SAVE_GLOB,

                0,
                1,

                &entry
            ) >= 0
        ) {

            int verify_result =
                memory_card_wait(
                    "mcGetDir(verify directory)"
                );


            if (verify_result >= 0) {

                printf(
                    "[MEMCARD] Settings directory already exists.\n"
                );

                fflush(stdout);

                return 1;
            }
        }


        printf(
            "[MEMCARD ERROR] "
            "Could not create settings directory (%d).\n",
            result
        );

        fflush(stdout);

        return 0;
    }


    printf(
        "[MEMCARD] Settings directory created.\n"
    );

    fflush(stdout);


    return 1;
}


/*
 * Save all current persistent Options.
 *
 * Right now this contains one value:
 * L3 VIDEO SKIP.
 */
static int memory_card_save_settings(void)
{
    BunnySettingsSave save
        __attribute__((aligned(64)));

    BunnySettingsSave verify
        __attribute__((aligned(64)));

    int fd;
    int result;

    int seek_result;

    int verify_fd;
    int verify_read;

#if !ENABLE_MEMORY_CARD_IO
    /* Display completion without writing anything. */
    return 1;
#endif

    printf(
        "[MEMCARD] Saving L3 Video Skip = %s\n",
        option_l3_skip_enabled
        ? "ON"
        : "OFF"
    );

    fflush(stdout);


    if (
        !memory_card_initialized ||
        !memory_card_check_slot()
    ) {

        printf(
            "[MEMCARD] Settings not saved: "
            "no usable card in Slot 1.\n"
        );

        fflush(stdout);

        return 0;
    }


    if (
        !memory_card_ensure_settings_directory()
    ) {

        return 0;
    }


    memset(
        &save,
        0,
        sizeof(save)
    );


    save.magic =
        SETTINGS_MAGIC;

    save.version =
        SETTINGS_VERSION;

    save.l3_skip_enabled =
        option_l3_skip_enabled
        ? 1U
        : 0U;


    save.checksum =
        memory_card_settings_checksum(
            &save
        );


    /*
     * Open the existing settings file, or create it.
     */
    if (
        mcOpen(
            SETTINGS_MC_PORT,
            SETTINGS_MC_SLOT,

            SETTINGS_SAVE_FILE,

            O_WRONLY |
            O_CREAT
        ) < 0
    ) {

        printf(
            "[MEMCARD ERROR] "
            "Save open request failed.\n"
        );

        fflush(stdout);

        return 0;
    }


    fd =
        memory_card_wait(
            "mcOpen(write)"
        );


    if (fd < 0) {

        printf(
            "[MEMCARD ERROR] "
            "Could not open settings file (%d).\n",
            fd
        );

        fflush(stdout);

        return 0;
    }


    /*
     * Explicitly start at byte zero.
     *
     * This guarantees an existing OFF save gets overwritten
     * when the user changes the option to ON.
     */
    if (
        mcSeek(
            fd,
            0,
            SEEK_SET
        ) < 0
    ) {

        printf(
            "[MEMCARD ERROR] "
            "Save seek request failed.\n"
        );

        fflush(stdout);


        mcClose(
            fd
        );

        memory_card_wait(
            "mcClose(after seek request error)"
        );

        return 0;
    }


    seek_result =
        memory_card_wait(
            "mcSeek(write)"
        );


    if (seek_result != 0) {

        printf(
            "[MEMCARD ERROR] "
            "Save seek returned %d instead of 0.\n",
            seek_result
        );

        fflush(stdout);


        mcClose(
            fd
        );

        memory_card_wait(
            "mcClose(after seek error)"
        );

        return 0;
    }


    /*
     * Write complete settings structure.
     */
    if (
        mcWrite(
            fd,
            &save,
            sizeof(save)
        ) < 0
    ) {

        printf(
            "[MEMCARD ERROR] "
            "Save write request failed.\n"
        );

        fflush(stdout);


        mcClose(
            fd
        );

        memory_card_wait(
            "mcClose(after write error)"
        );

        return 0;
    }


    result =
        memory_card_wait(
            "mcWrite"
        );


    if (
        result !=
        (int)sizeof(save)
    ) {

        printf(
            "[MEMCARD ERROR] "
            "Settings write incomplete (%d/%d).\n",

            result,
            (int)sizeof(save)
        );

        fflush(stdout);


        mcClose(
            fd
        );

        memory_card_wait(
            "mcClose(after short write)"
        );

        return 0;
    }


    /*
     * Force cached data to the Memory Card.
     */
    if (
        mcFlush(
            fd
        ) < 0
    ) {

        printf(
            "[MEMCARD ERROR] "
            "mcFlush request failed.\n"
        );

        fflush(stdout);


        mcClose(
            fd
        );

        memory_card_wait(
            "mcClose(after flush request error)"
        );

        return 0;
    }


    result =
        memory_card_wait(
            "mcFlush"
        );


    if (result < 0) {

        printf(
            "[MEMCARD ERROR] "
            "mcFlush failed (%d).\n",
            result
        );

        fflush(stdout);


        mcClose(
            fd
        );

        memory_card_wait(
            "mcClose(after flush error)"
        );

        return 0;
    }


    if (
        mcClose(
            fd
        ) < 0
    ) {

        printf(
            "[MEMCARD ERROR] "
            "Save close request failed.\n"
        );

        fflush(stdout);

        return 0;
    }


    result =
        memory_card_wait(
            "mcClose(write)"
        );


    if (result < 0) {

        printf(
            "[MEMCARD ERROR] "
            "Save close failed (%d).\n",
            result
        );

        fflush(stdout);

        return 0;
    }


    /*
     * ============================================================
     * READ-BACK VERIFICATION
     * ============================================================
     *
     * Don't merely assume the save worked.
     *
     * Reopen settings.dat and confirm the card really contains
     * the new ON/OFF value.
     */
    memset(
        &verify,
        0,
        sizeof(verify)
    );


    if (
        mcOpen(
            SETTINGS_MC_PORT,
            SETTINGS_MC_SLOT,

            SETTINGS_SAVE_FILE,

            O_RDONLY
        ) < 0
    ) {

        printf(
            "[MEMCARD ERROR] "
            "Verification open request failed.\n"
        );

        fflush(stdout);

        return 0;
    }


    verify_fd =
        memory_card_wait(
            "mcOpen(verify)"
        );


    if (verify_fd < 0) {

        printf(
            "[MEMCARD ERROR] "
            "Could not reopen settings for verification (%d).\n",
            verify_fd
        );

        fflush(stdout);

        return 0;
    }


    if (
        mcRead(
            verify_fd,
            &verify,
            sizeof(verify)
        ) < 0
    ) {

        printf(
            "[MEMCARD ERROR] "
            "Verification read request failed.\n"
        );

        fflush(stdout);


        mcClose(
            verify_fd
        );

        memory_card_wait(
            "mcClose(after verify read error)"
        );

        return 0;
    }


    verify_read =
        memory_card_wait(
            "mcRead(verify)"
        );


    if (
        mcClose(
            verify_fd
        ) >= 0
    ) {

        memory_card_wait(
            "mcClose(verify)"
        );
    }


    if (
        verify_read !=
            (int)sizeof(verify) ||

        verify.magic !=
            SETTINGS_MAGIC ||

        verify.version !=
            SETTINGS_VERSION ||

        verify.l3_skip_enabled >
            1U ||

        verify.checksum !=
            memory_card_settings_checksum(
                &verify
            ) ||

        verify.l3_skip_enabled !=
            save.l3_skip_enabled
    ) {

        printf(
            "[MEMCARD ERROR] "
            "Saved settings verification FAILED.\n"
        );

        printf(
            "[MEMCARD ERROR] "
            "Wanted L3=%u, read back L3=%u, bytes=%d.\n",

            (unsigned int)save.l3_skip_enabled,
            (unsigned int)verify.l3_skip_enabled,
            verify_read
        );

        fflush(stdout);

        return 0;
    }


    printf(
        "[MEMCARD] Settings saved and verified. "
        "L3 Video Skip = %s\n",

        verify.l3_skip_enabled
        ? "ON"
        : "OFF"
    );

    fflush(stdout);


    return 1;
}


/*
 * Load Options from Slot 1.
 *
 * Return:
 *
 *   0 = load failed / default OFF used
 *   1 = existing settings loaded
 *   2 = no settings file exists; create default OFF save
 *   3 = settings file invalid; rewrite default OFF save
 */
static int memory_card_load_settings(void)
{
    BunnySettingsSave save
        __attribute__((aligned(64)));

    int fd;
    int result;

#if !ENABLE_MEMORY_CARD_IO
    /* First-boot presentation, independent of card contents. */
    option_l3_skip_enabled = 0;
    return 2;
#endif

    /*
     * OFF is always the safe/default setting.
     */
    option_l3_skip_enabled =
        0;


    if (
        !memory_card_initialized ||
        !memory_card_check_slot()
    ) {

        return 0;
    }


    if (
        mcOpen(
            SETTINGS_MC_PORT,
            SETTINGS_MC_SLOT,

            SETTINGS_SAVE_FILE,

            O_RDONLY
        ) < 0
    ) {

        printf(
            "[MEMCARD ERROR] "
            "Load open request failed.\n"
        );

        fflush(stdout);

        return 0;
    }


    fd =
        memory_card_wait(
            "mcOpen(read)"
        );


    /*
     * No Bunny Maloney settings file exists.
     *
     * IMPORTANT:
     * Do not save here.
     *
     * Return to the boot presentation first so it can display
     * SAVING MEMORY CARD before the real write begins.
     */
    if (fd < 0) {

        printf(
            "[MEMCARD] No settings save found. "
            "Default OFF needs to be created.\n"
        );

        fflush(stdout);


        return 2;
    }


    memset(
        &save,
        0,
        sizeof(save)
    );


    if (
        mcRead(
            fd,
            &save,
            sizeof(save)
        ) < 0
    ) {

        printf(
            "[MEMCARD ERROR] "
            "Load read request failed.\n"
        );

        fflush(stdout);


        mcClose(
            fd
        );

        memory_card_wait(
            "mcClose(after read error)"
        );


        return 0;
    }


    result =
        memory_card_wait(
            "mcRead"
        );


    if (
        mcClose(
            fd
        ) >= 0
    ) {

        memory_card_wait(
            "mcClose(read)"
        );
    }


    /*
     * Validate the file before trusting it.
     *
     * If invalid, return 3 so the boot presentation can visibly
     * enter its SAVING phase before repairing the file.
     */
    if (
        result !=
            (int)sizeof(save) ||

        save.magic !=
            SETTINGS_MAGIC ||

        save.version !=
            SETTINGS_VERSION ||

        save.l3_skip_enabled >
            1U ||

        save.checksum !=
            memory_card_settings_checksum(
                &save
            )
    ) {

        printf(
            "[MEMCARD] Settings save invalid. "
            "Default OFF needs to be rewritten.\n"
        );

        fflush(stdout);


        option_l3_skip_enabled =
            0;


        return 3;
    }


    option_l3_skip_enabled =
        save.l3_skip_enabled
        ? 1
        : 0;


    printf(
        "[MEMCARD] Settings loaded. "
        "L3 Video Skip = %s\n",

        option_l3_skip_enabled
        ? "ON"
        : "OFF"
    );

    fflush(stdout);


    return 1;
}



/* ===== END MEMORY CARD SETTINGS ===== */


static GSTEXTURE test_bar_tex, loading_text_tex, disclaimer_text_tex, logo_tex, press_start_tex;
static GSTEXTURE play_norm_tex, play_sel_tex, ep_norm_tex, ep_sel_tex, cred_norm_tex, cred_sel_tex;
static GSTEXTURE opt_norm_tex, opt_sel_tex;
static GSTEXTURE l3_on_norm_tex, l3_on_sel_tex, l3_off_norm_tex, l3_off_sel_tex;
static GSTEXTURE credits_bg_tex, thumb_ep1, thumb_ep2, thumb_ep3, button_norm_tex, button_sel_tex;

/* ===== EPISODE TITLE TEXTURES ===== */
static GSTEXTURE ep1_title_tex;
static GSTEXTURE ep2_title_tex;
static GSTEXTURE ep3_title_tex;

static void trax_v2_load_title(GSGLOBAL *gsGlobal);

int load_all_menu_textures(GSGLOBAL *gsGlobal) {
    printf("[STEP] Clearing menu texture VRAM...\n");
    fflush(stdout);

    gsKit_vram_clear(gsGlobal);

    /*
     * These Main Menu texture descriptors pointed into the
     * previous VRAM layout.
     *
     * The VRAM allocation has now been reset, so invalidate
     * them completely.
     */
    memset(&play_norm_tex, 0, sizeof(play_norm_tex));
    memset(&play_sel_tex,  0, sizeof(play_sel_tex));

    memset(&ep_norm_tex, 0, sizeof(ep_norm_tex));
    memset(&ep_sel_tex,  0, sizeof(ep_sel_tex));

    memset(&cred_norm_tex, 0, sizeof(cred_norm_tex));
    memset(&cred_sel_tex,  0, sizeof(cred_sel_tex));

    memset(&opt_norm_tex, 0, sizeof(opt_norm_tex));
    memset(&opt_sel_tex,  0, sizeof(opt_sel_tex));

    memset(&l3_on_norm_tex,  0, sizeof(l3_on_norm_tex));
    memset(&l3_on_sel_tex,   0, sizeof(l3_on_sel_tex));
    memset(&l3_off_norm_tex, 0, sizeof(l3_off_norm_tex));
    memset(&l3_off_sel_tex,  0, sizeof(l3_off_sel_tex));

    /*
     * IMPORTANT:
     * Do not load every application texture at the same time.
     *
     * For the Press Start screen we only need these two.
     * Other screens will get their own texture loaders later.
     */
    int success = 1;

    success &= load_ui_texture(gsGlobal, &logo_tex, "logo.bmp");
    success &= load_ui_texture(gsGlobal, &press_start_tex, "press_start.bmp");
    /* V2 test notification is loaded only while Press Start owns VRAM. */
    trax_v2_load_title(gsGlobal);

    if (success) {
        printf("[STEP] Press Start textures loaded successfully.\n");
    } else {
        printf("[ERROR] Press Start screen texture loading failed.\n");
    }

    fflush(stdout);
    return success;
}


/* ===== BUNNY MALONEY BOOT UI ===== */

/*
 * These textures are temporary.
 *
 * They are shown BEFORE the normal Press Start textures are loaded,
 * so they don't consume permanent menu VRAM.
 */

static int boot_load_texture(
    GSGLOBAL *gsGlobal,
    GSTEXTURE *tex,
    const char *filename)
{
    char path[512];

    memset(tex, 0, sizeof(GSTEXTURE));

    snprintf(
        path,
        sizeof(path),
        "host:Textures/%s",
        filename
    );

    printf("[BOOT] Loading %s...\n", path);
    fflush(stdout);

    int result =
        gsKit_texture_bmp(
            gsGlobal,
            tex,
            path
        );

    if (result < 0 ||
        tex->Vram == GSKIT_ALLOC_ERROR) {

        printf(
            "[BOOT ERROR] Could not load %s\n",
            filename
        );

        fflush(stdout);
        return 0;
    }

    printf(
        "[BOOT] OK: %s (%dx%d)\n",
        filename,
        tex->Width,
        tex->Height
    );

    fflush(stdout);

    return 1;
}


static void boot_draw_texture(
    GSGLOBAL *gsGlobal,
    GSTEXTURE *tex,
    float x,
    float y)
{
    gsKit_prim_sprite_texture(
        gsGlobal,
        tex,

        x,
        y,

        0.0f,
        0.0f,

        x + tex->Width,
        y + tex->Height,

        tex->Width,
        tex->Height,

        1,

        GS_SETREG_RGBAQ(
            0x80,
            0x80,
            0x80,
            0x80,
            0x00
        )
    );
}



/*
 * ============================================================
 * MEMORY CARD OPAQUE BACKGROUND
 * ============================================================
 *
 * Memory Card screens must NEVER blend with the previous
 * loading screen.
 */
static void boot_memory_card_opaque_black(
    GSGLOBAL *gsGlobal)
{
    int previous_alpha =
        gsGlobal->PrimAlphaEnable;


    /*
     * Completely disable blending while replacing the background.
     */
    gsGlobal->PrimAlphaEnable =
        GS_SETTING_OFF;


    gsKit_clear(
        gsGlobal,

        GS_SETREG_RGBAQ(
            0,
            0,
            0,
            0x00,
            0
        )
    );


    /*
     * Physically cover the entire framebuffer as well.
     */
    gsKit_prim_sprite(
        gsGlobal,

        0.0f,
        0.0f,

        640.0f,
        448.0f,

        0,

        GS_SETREG_RGBAQ(
            0,
            0,
            0,
            0x00,
            0
        )
    );


    /*
     * Restore alpha for the Memory Card artwork itself.
     */
    gsGlobal->PrimAlphaEnable =
        previous_alpha;
}


static void boot_memory_card_clear_both_buffers(
    GSGLOBAL *gsGlobal)
{
    int i;


    /*
     * Destroy the loading screen in BOTH double-buffered
     * framebuffers before CHECKING MEMORY CARD appears.
     */
    for (
        i = 0;
        i < 4;
        i++
    ) {

        boot_memory_card_opaque_black(
            gsGlobal
        );


        gsKit_queue_exec(
            gsGlobal
        );

        gsKit_sync_flip(
            gsGlobal
        );
    }
}


static void boot_show_memory_card_status(
    GSGLOBAL *gsGlobal,
    GSTEXTURE *tex,
    int frames)
{
    int i;


    /* Only trigger when a new texture is about to be DISPLAYED. */
    if (frames > 0 && tex && tex->Vram != 0) {
        boot_loading_transition_sfx();
    }


    for (
        i = 0;
        i < frames;
        i++
    ) {

        /*
         * Every single Memory Card frame begins with REAL,
         * fully opaque black.
         */
        boot_memory_card_opaque_black(
            gsGlobal
        );


        if (
            tex &&
            tex->Vram != 0
        ) {

            float x =
                (
                    640.0f -
                    (float)tex->Width
                ) /
                2.0f;

            float y =
                176.0f;


            boot_draw_texture(
                gsGlobal,
                tex,
                x,
                y
            );


            /*
             * Moving activity marker.
             */
            float phase =
                (float)(
                    i %
                    36
                ) /
                35.0f;


            float marker_x =
                154.0f +
                (
                    332.0f *
                    phase
                );


            gsKit_prim_sprite(
                gsGlobal,

                marker_x,
                276.0f,

                marker_x + 34.0f,
                280.0f,

                2,

                GS_SETREG_RGBAQ(
                    0xFF,
                    0xB8,
                    0x18,
                    0x80,
                    0
                )
            );
        }


        gsKit_queue_exec(
            gsGlobal
        );

        gsKit_sync_flip(
            gsGlobal
        );
    }
}


static void boot_show_memory_card_message(
    GSGLOBAL *gsGlobal,
    GSTEXTURE *tex,
    int frames)
{
    int i;


    /* Only trigger when a new texture is about to be DISPLAYED. */
    if (frames > 0 && tex && tex->Vram != 0) {
        boot_loading_transition_sfx();
    }


    for (
        i = 0;
        i < frames;
        i++
    ) {

        /*
         * SAVE SUCCESSFUL / LOAD SUCCESSFUL are opaque too.
         */
        boot_memory_card_opaque_black(
            gsGlobal
        );


        if (
            tex &&
            tex->Vram != 0
        ) {

            float x =
                (
                    640.0f -
                    (float)tex->Width
                ) /
                2.0f;

            float y =
                176.0f;


            boot_draw_texture(
                gsGlobal,
                tex,
                x,
                y
            );
        }


        gsKit_queue_exec(
            gsGlobal
        );

        gsKit_sync_flip(
            gsGlobal
        );
    }
}


/* WATCHON_DISCLAIMER_WAIT_X_V1: Draw original disclaimer, plus Cross prompt. */
static void boot_draw_waiting_disclaimer(
    GSGLOBAL *gsGlobal,
    GSTEXTURE *disclaimer,
    int disclaimer_ok,
    GSTEXTURE *cross_icon,
    GSTEXTURE *continue_text,
    int cross_ok,
    int continue_text_ok,
    int shade)
{
    float scale = 1.0f;
    float w, h, x, y;
    float prompt_y;

    const float icon_x = 297.0f;
    const float icon_size = 32.0f;

    const float text_x = 338.0f;
    const float text_w = 128.0f;
    const float text_h = 40.0f;

    boot_memory_card_opaque_black(gsGlobal);

    if (disclaimer_ok) {
        w = disclaimer->Width * scale;
        h = disclaimer->Height * scale;

        if (w > 520.0f)
            scale *= 520.0f / w;

        w = disclaimer->Width * scale;
        h = disclaimer->Height * scale;

        if (h > 260.0f)
            scale *= 260.0f / h;

        w = disclaimer->Width * scale;
        h = disclaimer->Height * scale;

        x = (640.0f - w) * 0.5f;
        y = (448.0f - h) * 0.5f - 20.0f;

        if (y < 24.0f)
            y = 24.0f;

        gsKit_prim_sprite_texture(
            gsGlobal,
            disclaimer,
            x, y,
            0.0f, 0.0f,
            x + w, y + h,
            disclaimer->Width,
            disclaimer->Height,
            1,
            GS_SETREG_RGBAQ(
                shade, shade, shade, 0x80, 0
            )
        );
    } else {
        y = 80.0f;
        h = 220.0f;
    }

    prompt_y = y + h + 13.0f;

    if (prompt_y > 380.0f)
        prompt_y = 380.0f;

    /* Original square Cross icon. */
    if (cross_ok) {
        gsKit_prim_sprite_texture(
            gsGlobal,
            cross_icon,
            icon_x,
            prompt_y + 4.0f,
            0.0f, 0.0f,
            icon_x + icon_size,
            prompt_y + 4.0f + icon_size,
            cross_icon->Width,
            cross_icon->Height,
            2,
            GS_SETREG_RGBAQ(
                shade, shade, shade, 0x80, 0
            )
        );
    }

    /* Separate Aileron-rendered Continue text. */
    if (continue_text_ok) {
        gsKit_prim_sprite_texture(
            gsGlobal,
            continue_text,
            text_x,
            prompt_y,
            0.0f, 0.0f,
            text_x + text_w,
            prompt_y + text_h,
            continue_text->Width,
            continue_text->Height,
            2,
            GS_SETREG_RGBAQ(
                shade, shade, shade, 0x80, 0
            )
        );
    }
}


static void show_boot_screens(
    GSGLOBAL *gsGlobal)
{
    GSTEXTURE loading_text;
    GSTEXTURE test_bar;
    GSTEXTURE disclaimer;

    GSTEXTURE checking_memory_card;
    GSTEXTURE loading_memory_card;
    GSTEXTURE saving_memory_card;
    GSTEXTURE save_successful;
    GSTEXTURE load_successful;
    GSTEXTURE no_memory_card;

    int loading_ok;
    int bar_ok;
    int disclaimer_ok;

    int checking_card_ok;
    int loading_card_ok;
    int saving_card_ok;
    int save_success_ok;
    int load_success_ok;
    int no_card_ok;

    int card_ready;
    int settings_load_result;
    int settings_save_result;

    int i;

    const int BLACK_DELAY_FRAMES = 60;

    /*
     * Deliberately readable Memory Card presentation.
     *
     * At roughly 60 UI frames/sec:
     *
     * 90 = ~1.50 sec
     * 75 = ~1.25 sec
     * 30 = ~0.50 sec
     */
    /*
     * Memory Card presentation timing.
     *
     * NTSC UI runs at roughly 60 frames/sec.
     */
    const int MEMCARD_CHECK_FRAMES        = 240; /* ~4.0 sec */
    const int MEMCARD_LOAD_FRAMES         = 75;  /* ~1.25 sec */
    const int MEMCARD_SAVE_FRAMES         = 318; /* ~5.3 sec */

    const int MEMCARD_SAVE_SUCCESS_FRAMES = 180; /* ~3.0 sec */
    const int MEMCARD_LOAD_SUCCESS_FRAMES = 75;  /* ~1.25 sec */

    const int MEMCARD_ERROR_FRAMES        = 90;
    const int MEMCARD_BLACK_FRAMES        = 30;

    /*
     * 11 seconds of actual loading animation.
     *
     * 1 second black +
     * 11 second loading =
     * bar reaches 100% at about 12 seconds.
     */
    const int LOADING_FRAMES = 660;

    const int LOADING_FADE_FRAMES = 6;

    const int DISCLAIMER_FADE_IN  = 30;
    const int DISCLAIMER_HOLD     = 240;
    const int DISCLAIMER_FADE_OUT = 45;

    /*
     * ONE shared loading-bar layout.
     *
     * Both the animation and fade-out use these exact values,
     * preventing the bar from suddenly changing thickness.
     */
    const float BAR_SCALE_X = 0.85f;
    const float BAR_SCALE_Y = 0.32f;
    const float BAR_Y       = 380.0f;
    const float BAR_BORDER  = 1.5f;


    /*
     * ------------------------------------------------------------
     * IRREGULAR LOADING BEHAVIOUR
     * ------------------------------------------------------------
     *
     * Equal progress values = completely stopped.
     *
     * Very short intervals with large percentage differences =
     * sudden loading jumps.
     *
     * It always moves forward, but it should FEEL unpredictable.
     */

    static const int progress_frame[] = {
          0,
         42,
         88,
        104,
        165,
        215,
        230,
        290,
        344,
        359,
        420,
        474,
        489,
        545,
        586,
        601,
        630,
        646,
        660
    };

    static const float progress_value[] = {
          0.0f,

          5.0f,
          5.0f,     /* pause */

         17.0f,     /* sudden jump */

         22.0f,

         22.0f,     /* long pause */

         38.0f,     /* sudden jump */

         46.0f,

         46.0f,     /* pause */

         63.0f,     /* jump */

         70.0f,

         70.0f,     /* pause */

         83.0f,     /* jump */

         88.0f,

         88.0f,     /* pause */

         96.0f,

         96.0f,     /* tiny pause */

         99.0f,

        100.0f
    };


    const int PROGRESS_POINTS =
        sizeof(progress_frame) /
        sizeof(progress_frame[0]);


    printf("[BOOT] Starting boot presentation...\n");
    fflush(stdout);

    gsGlobal->PrimAlphaEnable =
        GS_SETTING_ON;


    /*
     * ============================================================
     * LOAD YOUR ORIGINAL LOADING ASSETS
     * ============================================================
     */

    gsKit_vram_clear(gsGlobal);

    loading_ok =
        boot_load_texture(
            gsGlobal,
            &loading_text,
            "loading_text.bmp"
        );

    bar_ok =
        boot_load_texture(
            gsGlobal,
            &test_bar,
            "test_bar.bmp"
        );


    /*
     * ============================================================
     * FIRST SECOND = NOTHING
     * ============================================================
     */

    for (i = 0;
         i < BLACK_DELAY_FRAMES;
         i++) {

        gsKit_clear(
            gsGlobal,
            GS_SETREG_RGBAQ(
                0,
                0,
                0,
                0x80,
                0
            )
        );

        gsKit_queue_exec(gsGlobal);
        gsKit_sync_flip(gsGlobal);
    }


    /*
     * ============================================================
     * ACTUAL LOADING SCREEN
     * ============================================================
     */

    for (i = 0;
         i <= LOADING_FRAMES;
         i++) {

        int point = 0;

        while (
            point < PROGRESS_POINTS - 2 &&
            i > progress_frame[point + 1]
        ) {
            point++;
        }


        int frame_a =
            progress_frame[point];

        int frame_b =
            progress_frame[point + 1];

        float value_a =
            progress_value[point];

        float value_b =
            progress_value[point + 1];


        float section_amount =
            0.0f;


        if (frame_b > frame_a) {

            section_amount =
                (float)(i - frame_a) /
                (float)(frame_b - frame_a);
        }


        if (section_amount < 0.0f)
            section_amount = 0.0f;

        if (section_amount > 1.0f)
            section_amount = 1.0f;


        float progress =
            value_a +
            (
                value_b -
                value_a
            ) *
            section_amount;


        progress /= 100.0f;


        gsKit_clear(
            gsGlobal,
            GS_SETREG_RGBAQ(
                0,
                0,
                0,
                0x80,
                0
            )
        );


        /*
         * --------------------------------------------------------
         * LOADING TEXT
         * --------------------------------------------------------
         */

        if (loading_ok) {

            float scale =
                0.70f;

            float w =
                loading_text.Width *
                scale;

            float h =
                loading_text.Height *
                scale;

            float x =
                (640.0f - w) /
                2.0f;

            float y =
                345.0f;


            gsKit_prim_sprite_texture(
                gsGlobal,
                &loading_text,

                x,
                y,

                0.0f,
                0.0f,

                x + w,
                y + h,

                loading_text.Width,
                loading_text.Height,

                1,

                GS_SETREG_RGBAQ(
                    0x80,
                    0x80,
                    0x80,
                    0x80,
                    0
                )
            );
        }


        /*
         * --------------------------------------------------------
         * YOUR test_bar.bmp
         * --------------------------------------------------------
         */

        if (bar_ok) {

            /*
             * ONE size for the bar.
             * The fade section uses these exact same values.
             */

            float bar_w =
                test_bar.Width *
                BAR_SCALE_X;

            float bar_h =
                test_bar.Height *
                BAR_SCALE_Y;

            float bar_x =
                (640.0f - bar_w) /
                2.0f;

            float bar_y =
                BAR_Y;


            /* WATCHON_LOADING_BAR_OPAQUE_FRAME_V2 - solid backing, drawn before textures.
               Bar texture covers the middle; only the slim rim stays visible.
               Disable alpha for this one frame to avoid blending it away. */
            {
                int saved_alpha = gsGlobal->PrimAlphaEnable;
                gsGlobal->PrimAlphaEnable = GS_SETTING_OFF;
                gsKit_prim_sprite(
                    gsGlobal,
                    bar_x - BAR_BORDER, bar_y - BAR_BORDER,
                    bar_x + bar_w + BAR_BORDER, bar_y + bar_h + BAR_BORDER,
                    0, GS_SETREG_RGBAQ(0xB8, 0xB8, 0xB8, 0x80, 0)
                );
                gsGlobal->PrimAlphaEnable = saved_alpha;
            }

            /*
             * Dim version of YOUR entire test_bar.bmp.
             */

            gsKit_prim_sprite_texture(
                gsGlobal,
                &test_bar,

                bar_x,
                bar_y,

                0.0f,
                0.0f,

                bar_x + bar_w,
                bar_y + bar_h,

                test_bar.Width,
                test_bar.Height,

                1,

                GS_SETREG_RGBAQ(
                    0x20,
                    0x20,
                    0x20,
                    0x80,
                    0
                )
            );


            /*
             * Bright portion revealed from left to right.
             */

            if (progress > 0.0f) {

                float visible_w =
                    bar_w *
                    progress;

                float visible_u =
                    test_bar.Width *
                    progress;


                gsKit_prim_sprite_texture(
                    gsGlobal,
                    &test_bar,

                    bar_x,
                    bar_y,

                    0.0f,
                    0.0f,

                    bar_x +
                    visible_w,

                    bar_y +
                    bar_h,

                    visible_u,
                    test_bar.Height,

                    2,

                    GS_SETREG_RGBAQ(
                        0x80,
                        0x80,
                        0x80,
                        0x80,
                        0
                    )
                );
            }


            /*
             * WHITE OUTLINE
             *
             * Drawn LAST as four separate edges.
             * test_bar.bmp therefore cannot cover it.
             */


            /* TOP */

            gsKit_prim_sprite(
                gsGlobal,

                bar_x - BAR_BORDER,
                bar_y - BAR_BORDER,

                bar_x + bar_w + BAR_BORDER,
                bar_y,

                3,

                GS_SETREG_RGBAQ(
                    0xFF,
                    0xFF,
                    0xFF,
                    0x80,
                    0
                )
            );


            /* BOTTOM */

            gsKit_prim_sprite(
                gsGlobal,

                bar_x - BAR_BORDER,
                bar_y + bar_h,

                bar_x + bar_w + BAR_BORDER,
                bar_y + bar_h + BAR_BORDER,

                3,

                GS_SETREG_RGBAQ(
                    0xFF,
                    0xFF,
                    0xFF,
                    0x80,
                    0
                )
            );


            /* LEFT */

            gsKit_prim_sprite(
                gsGlobal,

                bar_x - BAR_BORDER,
                bar_y,

                bar_x,
                bar_y + bar_h,

                3,

                GS_SETREG_RGBAQ(
                    0xFF,
                    0xFF,
                    0xFF,
                    0x80,
                    0
                )
            );


            /* RIGHT */

            gsKit_prim_sprite(
                gsGlobal,

                bar_x + bar_w,
                bar_y,

                bar_x + bar_w + BAR_BORDER,
                bar_y + bar_h,

                3,

                GS_SETREG_RGBAQ(
                    0xFF,
                    0xFF,
                    0xFF,
                    0x80,
                    0
                )
            );
        }


        gsKit_queue_exec(gsGlobal);
        gsKit_sync_flip(gsGlobal);
    }


    /*
     * ============================================================
     * LOADING SCREEN FADE OUT
     * ============================================================
     */

    for (i = 0;
         i < LOADING_FADE_FRAMES;
         i++) {

        float fade =
            1.0f -
            (
                (float)i /
                (float)(
                    LOADING_FADE_FRAMES - 1
                )
            );


        int full_shade =
            (int)(128.0f * fade);

        int dark_shade =
            (int)(32.0f * fade);


        gsKit_clear(
            gsGlobal,
            GS_SETREG_RGBAQ(
                0,
                0,
                0,
                0x80,
                0
            )
        );


        if (loading_ok) {

            float scale =
                0.70f;

            float w =
                loading_text.Width *
                scale;

            float h =
                loading_text.Height *
                scale;

            float x =
                (640.0f - w) /
                2.0f;


            gsKit_prim_sprite_texture(
                gsGlobal,
                &loading_text,

                x,
                345.0f,

                0.0f,
                0.0f,

                x + w,
                345.0f + h,

                loading_text.Width,
                loading_text.Height,

                1,

                GS_SETREG_RGBAQ(
                    full_shade,
                    full_shade,
                    full_shade,
                    0x80,
                    0
                )
            );
        }


        if (bar_ok) {

            /*
             * EXACT SAME SIZE as the animated bar.
             *
             * This is the important fix.
             */

            float bar_w =
                test_bar.Width *
                BAR_SCALE_X;

            float bar_h =
                test_bar.Height *
                BAR_SCALE_Y;

            float bar_x =
                (640.0f - bar_w) /
                2.0f;

            float bar_y =
                BAR_Y;


            /*
             * Texture colours use 0x80 as their normal
             * modulation level.
             */

            int outline_shade =
                (int)(
                    255.0f *
                    fade
                );


            /* WATCHON_LOADING_BAR_OPAQUE_FRAME_V2 - fade the solid border
               at the same speed as the loading bar and original outline. */
            {
                int saved_alpha = gsGlobal->PrimAlphaEnable;
                int rim_shade = (int)(184.0f * fade);
                gsGlobal->PrimAlphaEnable = GS_SETTING_OFF;
                gsKit_prim_sprite(
                    gsGlobal,
                    bar_x - BAR_BORDER, bar_y - BAR_BORDER,
                    bar_x + bar_w + BAR_BORDER, bar_y + bar_h + BAR_BORDER,
                    0, GS_SETREG_RGBAQ(rim_shade, rim_shade, rim_shade, 0x80, 0)
                );
                gsGlobal->PrimAlphaEnable = saved_alpha;
            }

            /*
             * Fading dim background.
             */

            gsKit_prim_sprite_texture(
                gsGlobal,
                &test_bar,

                bar_x,
                bar_y,

                0.0f,
                0.0f,

                bar_x + bar_w,
                bar_y + bar_h,

                test_bar.Width,
                test_bar.Height,

                1,

                GS_SETREG_RGBAQ(
                    dark_shade,
                    dark_shade,
                    dark_shade,
                    0x80,
                    0
                )
            );


            /*
             * Fading completed bar.
             */

            gsKit_prim_sprite_texture(
                gsGlobal,
                &test_bar,

                bar_x,
                bar_y,

                0.0f,
                0.0f,

                bar_x + bar_w,
                bar_y + bar_h,

                test_bar.Width,
                test_bar.Height,

                2,

                GS_SETREG_RGBAQ(
                    full_shade,
                    full_shade,
                    full_shade,
                    0x80,
                    0
                )
            );


            /*
             * Fade the outline too.
             */


            /* TOP */

            gsKit_prim_sprite(
                gsGlobal,

                bar_x - BAR_BORDER,
                bar_y - BAR_BORDER,

                bar_x + bar_w + BAR_BORDER,
                bar_y,

                3,

                GS_SETREG_RGBAQ(
                    outline_shade,
                    outline_shade,
                    outline_shade,
                    0x80,
                    0
                )
            );


            /* BOTTOM */

            gsKit_prim_sprite(
                gsGlobal,

                bar_x - BAR_BORDER,
                bar_y + bar_h,

                bar_x + bar_w + BAR_BORDER,
                bar_y + bar_h + BAR_BORDER,

                3,

                GS_SETREG_RGBAQ(
                    outline_shade,
                    outline_shade,
                    outline_shade,
                    0x80,
                    0
                )
            );


            /* LEFT */

            gsKit_prim_sprite(
                gsGlobal,

                bar_x - BAR_BORDER,
                bar_y,

                bar_x,
                bar_y + bar_h,

                3,

                GS_SETREG_RGBAQ(
                    outline_shade,
                    outline_shade,
                    outline_shade,
                    0x80,
                    0
                )
            );


            /* RIGHT */

            gsKit_prim_sprite(
                gsGlobal,

                bar_x + bar_w,
                bar_y,

                bar_x + bar_w + BAR_BORDER,
                bar_y + bar_h,

                3,

                GS_SETREG_RGBAQ(
                    outline_shade,
                    outline_shade,
                    outline_shade,
                    0x80,
                    0
                )
            );
        }


        gsKit_queue_exec(gsGlobal);
        gsKit_sync_flip(gsGlobal);
    }


    /*
     * ============================================================
     * MEMORY CARD CHECK / SETTINGS LOAD
     * ============================================================
     *
     * Deliberately paced PS2-style boot flow:
     *
     * Loading complete
     *   ->
     * CHECKING MEMORY CARD...
     *   ->
     * LOADING MEMORY CARD...
     *
     * First boot / missing settings:
     *
     * SAVING MEMORY CARD...
     *   ->
     * SAVE SUCCESSFUL
     *
     * Existing settings:
     *
     * LOAD SUCCESSFUL
     *
     * Then:
     *
     * solid black
     *   ->
     * Disclaimer
     */

    /*
     * Destroy the completed loading screen in BOTH framebuffers
     * before loading or displaying the Memory Card UI.
     *
     * No transparency is allowed here.
     */
    boot_memory_card_clear_both_buffers(
        gsGlobal
    );


    gsKit_vram_clear(
        gsGlobal
    );


    memset(
        &checking_memory_card,
        0,
        sizeof(checking_memory_card)
    );

    memset(
        &loading_memory_card,
        0,
        sizeof(loading_memory_card)
    );

    memset(
        &saving_memory_card,
        0,
        sizeof(saving_memory_card)
    );

    memset(
        &save_successful,
        0,
        sizeof(save_successful)
    );

    memset(
        &load_successful,
        0,
        sizeof(load_successful)
    );

    memset(
        &no_memory_card,
        0,
        sizeof(no_memory_card)
    );


    checking_card_ok =
        boot_load_texture(
            gsGlobal,
            &checking_memory_card,
            "checking_memory_card.bmp"
        );


    loading_card_ok =
        boot_load_texture(
            gsGlobal,
            &loading_memory_card,
            "loading_memory_card.bmp"
        );


    saving_card_ok =
        boot_load_texture(
            gsGlobal,
            &saving_memory_card,
            "saving_memory_card.bmp"
        );


    save_success_ok =
        boot_load_texture(
            gsGlobal,
            &save_successful,
            "save_successful.bmp"
        );


    load_success_ok =
        boot_load_texture(
            gsGlobal,
            &load_successful,
            "load_successful.bmp"
        );


    no_card_ok =
        boot_load_texture(
            gsGlobal,
            &no_memory_card,
            "no_memory_card.bmp"
        );


    /*
     * CHECKING MEMORY CARD...
     */
    if (checking_card_ok) {

        boot_show_memory_card_status(
            gsGlobal,
            &checking_memory_card,
            MEMCARD_CHECK_FRAMES
        );
    }


    card_ready =
        init_memory_card_subsystem() &&
        memory_card_check_slot();


    if (card_ready) {

        /*
         * LOADING MEMORY CARD...
         */
        if (loading_card_ok) {

            boot_show_memory_card_status(
                gsGlobal,
                &loading_memory_card,
                MEMCARD_LOAD_FRAMES
            );
        }


        settings_load_result =
            memory_card_load_settings();


        printf(
            "[BOOT] Memory Card settings result: %d\n",
            settings_load_result
        );

        fflush(stdout);


        /*
         * Existing valid save.
         */
        if (
            settings_load_result ==
            1
        ) {

            if (load_success_ok) {

                boot_show_memory_card_message(
                    gsGlobal,
                    &load_successful,
                    MEMCARD_LOAD_SUCCESS_FRAMES
                );
            }

        } else if (
            settings_load_result ==
            2 ||
            settings_load_result ==
            3
        ) {

            /*
             * Missing or invalid settings.
             *
             * Show SAVING first, THEN perform the actual write.
             */
            if (saving_card_ok) {

                boot_show_memory_card_status(
                    gsGlobal,
                    &saving_memory_card,
                    MEMCARD_SAVE_FRAMES
                );
            }


            settings_save_result =
                memory_card_save_settings();


            if (settings_save_result) {

                /*
                 * SAVE SUCCESSFUL
                 */
                if (save_success_ok) {

                    boot_show_memory_card_message(
                        gsGlobal,
                        &save_successful,
                        MEMCARD_SAVE_SUCCESS_FRAMES
                    );
                }

            } else {

                /*
                 * Write failed or card vanished.
                 */
                option_l3_skip_enabled =
                    0;


                if (no_card_ok) {

                    boot_show_memory_card_message(
                        gsGlobal,
                        &no_memory_card,
                        MEMCARD_ERROR_FRAMES
                    );
                }
            }

        } else {

            /*
             * Existing save could not be loaded.
             */
            option_l3_skip_enabled =
                0;


            if (no_card_ok) {

                boot_show_memory_card_message(
                    gsGlobal,
                    &no_memory_card,
                    MEMCARD_ERROR_FRAMES
                );
            }
        }

    } else {

        /*
         * No card / unformatted card / access failure.
         */
        option_l3_skip_enabled =
            0;


        printf(
            "[BOOT] No usable Memory Card in Slot 1. "
            "Using default settings.\n"
        );

        fflush(stdout);


        if (no_card_ok) {

            boot_show_memory_card_message(
                gsGlobal,
                &no_memory_card,
                MEMCARD_ERROR_FRAMES
            );
        }
    }


    /*
     * ============================================================
     * GUARANTEED OPAQUE BLACK HANDOFF
     * ============================================================
     *
     * IMPORTANT:
     *
     * gsKit_clear() is itself a GS primitive.
     *
     * The previous Memory Card transition cleared while alpha was
     * enabled, which can allow a previous framebuffer to remain
     * faintly visible underneath the Disclaimer.
     *
     * Force alpha OFF and repeatedly overwrite/flip BOTH buffers.
     */
    {
        int previous_alpha =
            gsGlobal->PrimAlphaEnable;


        gsGlobal->PrimAlphaEnable =
            GS_SETTING_OFF;


        for (
            i = 0;
            i < MEMCARD_BLACK_FRAMES;
            i++
        ) {

            gsKit_clear(
                gsGlobal,

                GS_SETREG_RGBAQ(
                    0,
                    0,
                    0,
                    0x00,
                    0
                )
            );


            gsKit_queue_exec(
                gsGlobal
            );

            gsKit_sync_flip(
                gsGlobal
            );
        }


        gsGlobal->PrimAlphaEnable =
            previous_alpha;
    }


    /* WATCHON_DISCLAIMER_WAIT_X_V1
     * Original disclaimer_text.bmp remains unchanged.
     * Wait indefinitely for a NEW Cross press before fading out.
     */
    {
        GSTEXTURE disclaimer_cross_icon;
        GSTEXTURE disclaimer_continue_text;
        int cross_ok;
        int continue_text_ok;
        int released_cross = 0;
        int shade;
        struct padButtonStatus boot_buttons;

        gsKit_vram_clear(gsGlobal);
        disclaimer_ok = boot_load_texture(
            gsGlobal, &disclaimer, "disclaimer_text.bmp");
        cross_ok = boot_load_texture(
            gsGlobal, &disclaimer_cross_icon, "disclaimer_cross.bmp");

        continue_text_ok = boot_load_texture(
            gsGlobal,
            &disclaimer_continue_text,
            "disclaimer_continue_text.bmp");

        /* Preserve existing fade-in duration. */
        for (i = 0; i < DISCLAIMER_FADE_IN; ++i) {
            shade = (DISCLAIMER_FADE_IN > 1)
                ? (128 * i) / (DISCLAIMER_FADE_IN - 1) : 128;
            boot_draw_waiting_disclaimer(gsGlobal,
                &disclaimer, disclaimer_ok,
                &disclaimer_cross_icon,
                &disclaimer_continue_text,
                0, 0, shade);
            gsKit_queue_exec(gsGlobal);
            gsKit_sync_flip(gsGlobal);
        }

        /*
         * Wait approximately 3 seconds AFTER fade-in.
         * The disclaimer stays visible, but X Continue is hidden.
         */
        for (i = 0; i < 180; ++i) {
            boot_draw_waiting_disclaimer(
                gsGlobal,
                &disclaimer, disclaimer_ok,
                &disclaimer_cross_icon,
                &disclaimer_continue_text,
                0, 0, 128);
            gsKit_queue_exec(gsGlobal);
            gsKit_sync_flip(gsGlobal);
        }

        /*
         * Show X Continue and wait indefinitely for a NEW press.
         * No automatic fade-out.
         */
        printf("[BOOT] Disclaimer waiting for X...\n");
        fflush(stdout);
        for (;;) {
            boot_draw_waiting_disclaimer(gsGlobal,
                &disclaimer, disclaimer_ok,
                &disclaimer_cross_icon,
                &disclaimer_continue_text,
                cross_ok, continue_text_ok, 128);
            gsKit_queue_exec(gsGlobal);
            gsKit_sync_flip(gsGlobal);

            if (pad_initialized && padGetState(0, 0) == PAD_STATE_STABLE) {
                memset(&boot_buttons, 0, sizeof(boot_buttons));
                if (padRead(0, 0, &boot_buttons) != 0) {
                    const int pressed_cross =
                        (((u32)(0xffff ^ boot_buttons.btns)) & PAD_CROSS) != 0;
                    if (!pressed_cross) {
                        released_cross = 1;
                    } else if (released_cross) {
                        /* Use the EXISTING working confirm sound. */
                        ui_sfx_play(UI_SFX_CONFIRM);
                        printf("[BOOT] Disclaimer confirmed with X.\n");
                        fflush(stdout);
                        break;
                    }
                }
            }
        }

        /* Keep original fade-out, but start it ONLY after X. */
        for (i = 0; i < DISCLAIMER_FADE_OUT; ++i) {
            shade = (DISCLAIMER_FADE_OUT > 1)
                ? (128 * (DISCLAIMER_FADE_OUT - 1 - i)) /
                  (DISCLAIMER_FADE_OUT - 1)
                : 0;
            boot_draw_waiting_disclaimer(gsGlobal,
                &disclaimer, disclaimer_ok,
                &disclaimer_cross_icon,
                &disclaimer_continue_text,
                cross_ok, continue_text_ok, shade);
            gsKit_queue_exec(gsGlobal);
            gsKit_sync_flip(gsGlobal);
        }

        memset(&disclaimer_cross_icon, 0, sizeof(disclaimer_cross_icon));
        memset(&disclaimer_continue_text, 0, sizeof(disclaimer_continue_text));
    }

    /*
     * ============================================================
     * CLEAN MAIN-MENU HANDOFF
     * ============================================================
     */

    gsKit_vram_clear(gsGlobal);

    memset(
        &loading_text,
        0,
        sizeof(loading_text)
    );

    memset(
        &test_bar,
        0,
        sizeof(test_bar)
    );

    memset(
        &disclaimer,
        0,
        sizeof(disclaimer)
    );


    /*
     * Explicitly overwrite both GS buffers several times.
     *
     * This is what prevents the disclaimer from showing underneath
     * the logo / Press Start screen afterwards.
     */

    /*
     * The loading/disclaimer presentation uses alpha blending.
     *
     * Disable it for the final framebuffer destruction so absolutely
     * none of the loading screen or disclaimer can survive underneath
     * the Press Start screen.
     */
    gsGlobal->PrimAlphaEnable =
        GS_SETTING_OFF;

    for (i = 0;
         i < 8;
         i++) {

        gsKit_clear(
            gsGlobal,
            GS_SETREG_RGBAQ(
                0,
                0,
                0,
                0x00,
                0
            )
        );

        gsKit_queue_exec(gsGlobal);
        gsKit_sync_flip(gsGlobal);
    }

    /*
     * The title/menu UI needs alpha again for its fading and artwork.
     */
    gsGlobal->PrimAlphaEnable =
        GS_SETTING_ON;


    printf(
        "[BOOT] Boot presentation finished.\n"
    );

    fflush(stdout);
}




/* ===== END BUNNY MALONEY BOOT UI ===== */





/* WATCHON_TRAX_PRESS_START_V3_1_BMP24_FIX
 * Independent panel / spinning disc; visual-only PS2 demonstration.
 * Begins 90 UI frames after title screen opens, before Press Start text.
 * One loaded graphite gradient panel, 16 low-memory circular disc frames.
 * This code never reconfigures PCM, ADPCM, episode timing, or menu input.
 */
/* WATCHON_MUSIC_V4_BEGIN: lightweight menu soundtrack; episode player is separate. */
#define WATCHON_MAX_TRACKS 160
#define WATCHON_MUSIC_CHUNK 2048

typedef struct WatchonTrack {
    char id[8];
    char title[56];
    char artist[56];
    char album[56];
    unsigned int duration_ms;
} WatchonTrack;
static WatchonTrack watchon_tracks[WATCHON_MAX_TRACKS];
static int watchon_count = 0;
static int watchon_current = -1;
static int watchon_music_enabled = 1;
static int watchon_order_random = 0;
static int watchon_assets_dirty = 0;
static int watchon_settings_focus = 0;
static int watchon_settings_scroll = 0;
static int watchon_options_focus = 0;
static FILE *watchon_music_file = NULL;
static int watchon_stream_eof = 0;
static int watchon_eof_grace_frames = 0;
static int watchon_music_playing = 0;
/* WATCHON_TRAX_V4_2_AILERON_CLOCK_SYNC: elapsed-time recovery, menu soundtrack only. */
static u64 watchon_track_start_ticks = 0;
static GSTEXTURE watchon_option_norm_tex, watchon_option_sel_tex, watchon_font_tex, watchon_font_gold_tex;


/* WATCHON_TRAX_V3_5_VRAM_COMPACT: only eight 64x64 BMP24 frames in GS VRAM. */
#define TRAX_V2_DISC_COUNT 8
#define TRAX_V2_PANEL_VERSIONS 4
static GSTEXTURE trax_v2_panel;
static GSTEXTURE trax_v2_discs[TRAX_V2_DISC_COUNT];
static int trax_v2_ready = 0;
static int trax_v2_frame = 0;
static int trax_v2_started = 0;
/* WATCHON_TRAX_V3_4_GLOBAL_OVERLAY: menu-wide timing / lifecycle. */
#define TRAX_V2_TOTAL_FRAMES (30 + 25 + 16 + 420 + 22 + 42)
static int trax_v2_delay_remaining = 90; /* ~1.5 seconds after title first draws */

static float trax_v2_clamp(float x)
{
    return (x < 0.0f) ? 0.0f : ((x > 1.0f) ? 1.0f : x);
}

static float trax_v2_cubic(float t)
{
    float p = 1.0f - trax_v2_clamp(t);
    return 1.0f - p * p * p;
}

static float trax_v2_smooth(float t)
{
    t = trax_v2_clamp(t);
    return t*t*(3.0f - 2.0f*t);
}

/* WATCHON_TRAX_V3_3_EARLIER_IMPACT: no pre-contact overshoot.
 * Disc speeds into panel, then the existing IMPACT stage handles rebound.
 */
static float trax_v2_impact_approach(float t)
{
    t = trax_v2_clamp(t);
    return t * t;
}

static void trax_v2_load_title(GSGLOBAL *gsGlobal)
{
    int i;
    char name[64];
    int variant;
    trax_v2_ready = 0;
    /* Do not restart the notification when changing menu screens. */
    memset(&trax_v2_panel, 0, sizeof(trax_v2_panel));
    memset(trax_v2_discs, 0, sizeof(trax_v2_discs));

    variant = (int)((GetTimerSystemTime() / 100000U) % TRAX_V2_PANEL_VERSIONS);
    if (watchon_count>0 && watchon_current>=0)
        snprintf(name,sizeof(name),"Music/Panels/%s_%d.bmp",
                 watchon_tracks[watchon_current].id,variant);
    else
        snprintf(name,sizeof(name),"TRAX/panel_%d.bmp",variant);
    if (!load_ui_texture(gsGlobal, &trax_v2_panel, name)) {
        printf("[TRAX V2] Unable to load title panel; skipping preview.\n");
        return;
    }
    for (i=0; i<TRAX_V2_DISC_COUNT; ++i) {
        if (watchon_count>0 && watchon_current>=0)
            snprintf(name,sizeof(name),"Music/Covers/%s_%02d.bmp",
                     watchon_tracks[watchon_current].id,i);
        else
            snprintf(name,sizeof(name),"TRAX/disc_%02d.bmp",i);
        if (!load_ui_texture(gsGlobal, &trax_v2_discs[i], name)) {
            printf("[TRAX V2] Disc %d failed/VRAM exhausted; skipping.\n", i);
            return;
        }
    }
    trax_v2_ready = 1;
    printf("[TRAX V3.5] Graphite panel + 8 compact disc frames ready; variant %d.\n", variant);
    fflush(stdout);
}

/* WATCHON_TRAX_V3_2_SHAPE_MASK:
 * Display-only scanline clipping of the known V3.1 24-bit BMP assets.
 * No transparency-capable BMP decoder or GS alpha-format dependency.
 * A 64-pixel title notification is rendered in 32 horizontal 2px bands;
 * the black-matted areas of the source images are never drawn.
 */
static void trax_v2_sprite(GSGLOBAL *gsGlobal, GSTEXTURE *tex,
                           float x, float y, float w, float h, int z)
{
    static const unsigned char panel_left[32] = {
        12, 17, 20, 22, 24, 26, 27, 29,
        30, 31, 31, 32, 32, 33, 33, 33,
        33, 33, 33, 32, 32, 31, 30, 29,
        28, 27, 25, 24, 21, 19, 15, 10
    };
    static const unsigned char panel_right[32] = {
        211, 214, 215, 215, 216, 216, 216, 216,
        216, 216, 216, 216, 216, 216, 216, 216,
        216, 216, 216, 216, 216, 216, 216, 216,
        216, 216, 216, 216, 215, 215, 214, 211
    };
    static const unsigned char disc_left[32] = {
        32, 22, 18, 14, 12, 10, 8, 6,
        5, 4, 3, 3, 2, 2, 1, 1,
        1, 1, 2, 2, 3, 4, 5, 6,
        7, 9, 10, 13, 15, 19, 25, 32
    };
    static const unsigned char disc_right[32] = {
        31, 41, 46, 49, 52, 54, 55, 57,
        58, 59, 60, 61, 61, 62, 62, 62,
        62, 62, 62, 61, 61, 60, 59, 58,
        56, 55, 53, 51, 48, 44, 39, 31
    };
    const unsigned char *left, *right;
    int i;
    float v0,v1,u0,u1;

    if (!tex || !tex->Vram) return;

    if (w == 216.0f && h == 64.0f &&
        tex->Width == 320 && tex->Height == 96) {
        left=panel_left; right=panel_right;
    } else if (w == 64.0f && h == 64.0f &&
               tex->Width == 64 && tex->Height == 64) {
        left=disc_left; right=disc_right;
    } else {
        /* Fallback for an unexpected texture version; never change others. */
        gsKit_prim_sprite_texture(gsGlobal, tex,
            x, y, 0.0f, 0.0f, x+w, y+h,
            (float)tex->Width, (float)tex->Height, z,
            GS_SETREG_RGBAQ(0x80,0x80,0x80,0x80,0));
        return;
    }

    /* Only the opaque arc interior is actually drawn. */
    for (i=0; i<32; i++) {
        if (right[i] <= left[i]) continue;
        u0=(float)left[i]*(float)tex->Width/w;
        u1=(float)right[i]*(float)tex->Width/w;
        v0=(float)(i*2)*(float)tex->Height/h;
        v1=(float)((i+1)*2)*(float)tex->Height/h;
        gsKit_prim_sprite_texture(gsGlobal, tex,
            x+(float)left[i], y+(float)(i*2), u0, v0,
            x+(float)right[i], y+(float)((i+1)*2), u1, v1,
            z, GS_SETREG_RGBAQ(0x80,0x80,0x80,0x80,0));
    }
}

static void trax_v2_draw_title(GSGLOBAL *gsGlobal, int right_side)
{
    enum { PANEL_IN=30, DISC_IN=25, IMPACT=16, HOLD=420,
           DISC_EXIT=22, PANEL_EXIT=42 };
    const int join = PANEL_IN + DISC_IN;
    const int settled = join + IMPACT;
    const int leave_disc = settled + HOLD;
    const int leave_panel = leave_disc + DISC_EXIT;
    const int end = leave_panel + PANEL_EXIT;
    /* Compact lower-left dock, leaving the centred logo/prompt clear. */
    /* WATCHON_TRAX_V3_6_1_MENU_AWARE_POSITION: preserve title values, dock right on interactive menus. */
    const float base_panel_x = right_side ? 406.0f : 40.0f;
    const float base_y = right_side ? 366.0f : 366.0f;
    const float base_disc_x = base_panel_x - 18.0f, base_disc_y = base_y;
    static const float hit_panel_x[IMPACT] = {
        4.0f, 2.8f, -2.4f, -1.9f, 1.8f, 1.1f, -1.0f, -0.7f, 0.6f,
        0.3f, -0.3f, -0.2f, 0.2f, 0.1f, 0.0f, 0.0f
    };
    static const float hit_disc_x[IMPACT] = {
        -2.8f,-1.4f,2.0f,1.3f,-1.3f,-0.6f,0.7f,0.5f,-0.4f,-0.3f,0.3f,
        0.1f,-0.1f,0,0,0
    };
    static const float hit_y[IMPACT] = {
        1.8f,-1.6f,1.2f,-0.8f,0.7f,-0.5f,0.4f,
        -0.3f,0.25f,-0.2f,0.1f,0,0,0,0,0
    };
    float x_panel = base_panel_x;
    float x_disc = base_disc_x;
    float off_y = 0.0f;
    float t;
    int f;
    int show_panel = 1, show_disc = 1;

    if (!trax_v2_ready) return;
    f = trax_v2_frame;
    if (f >= end) return;
    if (!trax_v2_started) {
        trax_v2_started = 1;
        printf("[TRAX V3] Starting at title + 90 frames (~1.5 sec).\n");
        fflush(stdout);
    }
    trax_v2_frame++;

    if (f < PANEL_IN) {
        show_disc = 0;
        t = trax_v2_cubic((float)(f+1)/(float)PANEL_IN);
        x_panel = right_side
            ? (640.0f + (base_panel_x - 640.0f)*t)
            : (-360.0f + (base_panel_x+360.0f)*t);
    } else if (f < join) {
        t = trax_v2_impact_approach((float)(f-PANEL_IN+1)/(float)DISC_IN);
        x_disc = right_side
            ? (base_disc_x - 48.0f + 48.0f*t)
            : (-112.0f + (base_disc_x+112.0f)*t);
    } else if (f < settled) {
        int k = f - join;
        x_panel += hit_panel_x[k];
        x_disc += hit_disc_x[k];
        off_y = hit_y[k];
    } else if (f < leave_disc) {
        /* Remain docked with a continuously spinning cover for 7 sec. */
    } else if (f < leave_panel) {
        t = trax_v2_clamp((float)(f-leave_disc+1)/(float)DISC_EXIT);
        x_disc = right_side
            ? (base_disc_x + (640.0f - base_disc_x + 64.0f)*t*t)
            : (base_disc_x - 238.0f*t*t);
    } else {
        show_disc = 0;
        t = trax_v2_smooth((float)(f-leave_panel+1)/(float)PANEL_EXIT);
        x_panel = right_side
            ? (base_panel_x + (640.0f - base_panel_x + 8.0f)*t)
            : (base_panel_x - (base_panel_x+355.0f)*t);
    }

    if (show_panel)
        trax_v2_sprite(gsGlobal,&trax_v2_panel,
                       x_panel,base_y+off_y,216.0f,64.0f,15);
    if (show_disc)
        trax_v2_sprite(gsGlobal,&trax_v2_discs[(f/10)%TRAX_V2_DISC_COUNT],
                       x_disc,base_disc_y+off_y,64.0f,64.0f,16);

    /* Contact flash: single brief spark, not a persistent effect. */
    if (f>=join && f<join+6) {
        float strength = (float)(join+6-f);
        float px = x_disc+63.0f;
        gsKit_prim_line(gsGlobal,px,base_y+16.0f,
                        px+strength*1.2f,base_y+12.0f-strength,
                        17,GS_SETREG_RGBAQ(0xB8,0xC1,0xCA,0x80,0));
        gsKit_prim_line(gsGlobal,px,base_y+48.0f,
                        px+strength*1.2f,base_y+52.0f+strength,
                        17,GS_SETREG_RGBAQ(0x8A,0x92,0x9B,0x80,0));
    }
}
/* WATCHON_TRAX_PRESS_START_V2_END */

static void watchon_copy(char *dst, int cap, const char *src)
{
    if (!src) src = "";
    snprintf(dst, (size_t)cap, "%s", src);
}
static void watchon_trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n-1]=='\r' || s[n-1]=='\n' || s[n-1]==' ')) s[--n]='\0';
}
static void watchon_load_library(void)
{
    char line[384];
    FILE *f;
    watchon_count=0;
    watchon_current=-1;
    f=fopen("host:Music/tracks.tsv", "rb");
    if (!f) { printf("[WATCHON] No Music/tracks.tsv yet; demo-only UI.\n"); return; }
    while (watchon_count<WATCHON_MAX_TRACKS && fgets(line,sizeof(line),f)) {
        char *fields[5];
        int n=0,i;
        WatchonTrack *p;
        if (line[0]=='#' || line[0]=='\n') continue;
        fields[n]=strtok(line,"\t\r\n");
        while (fields[n] && n<4) {
            fields[++n]=strtok(NULL,"\t\r\n");
        }
        if (!fields[0] || !fields[1] || !fields[2]) continue;
        for(i=0;fields[0][i];i++) if(fields[0][i]<'0'||fields[0][i]>'9') break;
        if(i==0||fields[0][i]!=0||i>4) continue;
        p=&watchon_tracks[watchon_count];
        watchon_copy(p->id,sizeof(p->id),fields[0]);
        watchon_copy(p->title,sizeof(p->title),fields[1]);
        watchon_copy(p->artist,sizeof(p->artist),fields[2]);
        watchon_copy(p->album,sizeof(p->album),fields[3]?fields[3]:"Unknown Album");
        p->duration_ms=fields[4]?(unsigned int)strtoul(fields[4],NULL,10):0;
        watchon_trim(p->album);
        watchon_count++;
    }
    fclose(f);
    { int on,mode; FILE *cfg=fopen("host:Music/settings.cfg","rb");
      if(cfg) { if(fscanf(cfg,"%d %d",&on,&mode)==2) {
              watchon_music_enabled=(on!=0); watchon_order_random=(mode!=0);
          } fclose(cfg); }
    }
    printf("[WATCHON] %d tracks loaded; music=%d random=%d\n",watchon_count,
           watchon_music_enabled,watchon_order_random); fflush(stdout);
}
static void watchon_save_settings(void)
{
    /* Host filesystem writes may not be supported on every PS2 launch method.
       Failure is harmless: the choice remains valid for this session. */
    FILE *f=fopen("host:Music/settings.cfg","wb");
    if(f) { fprintf(f,"%d %d\n",watchon_music_enabled,watchon_order_random); fclose(f); }
    else printf("[WATCHON] Settings kept for this session (host write unavailable).\n");
}
static void watchon_stop_music(void)
{
    if(watchon_music_file) { fclose(watchon_music_file); watchon_music_file=NULL; }
    if(watchon_music_playing && audsrv_initialized) {
        struct audsrv_fmt_t fmt;
        audsrv_stop_audio();
        fmt.bits=16; fmt.freq=22050; fmt.channels=2;
        (void)audsrv_set_format(&fmt); /* also clears queued menu PCM */
    }
    watchon_music_playing=0;
    watchon_stream_eof=0;
    watchon_eof_grace_frames=0;
    watchon_track_start_ticks=0;
}
static int watchon_start_track(int index)
{
    char filename[128];
    FILE *f;
    struct audsrv_fmt_t fmt;
    if(!watchon_music_enabled || !audsrv_initialized || index<0 || index>=watchon_count)
        return 0;
    snprintf(filename,sizeof(filename),"host:Music/Audio/%s.pcm",watchon_tracks[index].id);
    f=fopen(filename,"rb");
    if(!f) {printf("[WATCHON] Missing PCM: %s\n", filename); return 0;}
    watchon_stop_music();
    fmt.bits=16; fmt.freq=22050; fmt.channels=2;
    audsrv_stop_audio();
    if(audsrv_set_format(&fmt)<0) { fclose(f); printf("[WATCHON] PCM format unavailable.\n"); return 0; }
    audsrv_set_volume(58);
    watchon_music_file=f;
    watchon_music_playing=1;
    watchon_track_start_ticks=GetTimerSystemTime();
    watchon_current=index;
    watchon_stream_eof=0;
    watchon_eof_grace_frames=0;
    /* The V3.3 animation, hold length and exit are deliberately untouched. */
    trax_v2_frame=0;
    trax_v2_started=0;
    trax_v2_delay_remaining=0;
    watchon_assets_dirty=1;
    printf("[WATCHON] Now playing %s - %s\n", watchon_tracks[index].title,
           watchon_tracks[index].artist); fflush(stdout);
    /* Begin filling the small audsrv buffer immediately, before any
       new TRAX textures are uploaded or a menu screen is switched. */
    watchon_music_pump();
    return 1;
}
static int watchon_next_index(void)
{
    int n;
    if(!watchon_count) return -1;
    if(watchon_current<0) return 0;
    if(!watchon_order_random || watchon_count==1)
        return (watchon_current+1)%watchon_count;
    n=rand()%(watchon_count-1);
    if(n>=watchon_current) n++;
    return n;
}
static void watchon_start_different(void)
{
    int n=watchon_next_index();
    if(watchon_music_enabled && n>=0) (void)watchon_start_track(n);
}
static void watchon_music_pump(void)
{
    int i;
    char chunk[WATCHON_MUSIC_CHUNK];
    if(!watchon_music_enabled || !watchon_music_file || !watchon_music_playing) return;

    /* V4.2 clock synchronization: recover from menu transitions that take
       longer than the very small audsrv stream buffer can cover. Do not
       repeatedly seek for normal <100 ms scheduling jitter. */
    if (!watchon_stream_eof && watchon_track_start_ticks != 0) {
        u64 now_ticks=GetTimerSystemTime();
        if (now_ticks >= watchon_track_start_ticks) {
            u32 sec=0, usec=0;
            u64 desired;
            long current_pos=ftell(watchon_music_file);
            TimerBusClock2USec(now_ticks-watchon_track_start_ticks,&sec,&usec);
            desired=(u64)sec*88200ULL + ((u64)usec*88200ULL)/1000000ULL;
            desired &= ~3ULL; /* signed 16-bit stereo frame alignment */
            /* Only seek if the host reading position has fallen >130ms
               behind the actual song time. Frequent fseeks cause glitches. */
            if(current_pos>=0 && desired>(u64)current_pos+11468ULL &&
               desired <= 0x7FFFFFFCULL) {
                struct audsrv_fmt_t fmt;
                long aligned=(long)desired;
                if(fseek(watchon_music_file,aligned,SEEK_SET)==0) {
                    /* Reset only the MENU PCM queue, never episode audio. */
                    audsrv_stop_audio();
                    fmt.bits=16; fmt.freq=22050; fmt.channels=2;
                    if(audsrv_set_format(&fmt)<0) {
                        printf("[WATCHON V4.2] Audio reset failed.\n");
                        watchon_stop_music();
                        return;
                    }
                    audsrv_set_volume(58);
                    printf("[WATCHON V4.2] Menu IO stall: seek to %lu ms (was %lu ms).\n",
                           (unsigned long)(((u64)aligned*1000ULL)/88200ULL),
                           (unsigned long)(((u64)current_pos*1000ULL)/88200ULL));
                    fflush(stdout);
                }
            }
        }
    }
    if(watchon_stream_eof) {
        if(++watchon_eof_grace_frames >= 24) {
            watchon_stop_music();
            watchon_start_different();
        }
        return;
    }
    for(i=0;i<6;i++) {
        int freebytes=audsrv_available();
        int want,got,sent;
        if(freebytes<256) break;
        want=freebytes>WATCHON_MUSIC_CHUNK?WATCHON_MUSIC_CHUNK:freebytes;
        want &= ~3; /* stereo 16-bit samples */
        if(want<4) break;
        got=(int)fread(chunk,1,(size_t)want,watchon_music_file);
        if(got<=0) { watchon_stream_eof=1; break; }
        got &= ~3;
        sent=audsrv_play_audio(chunk,got);
        if(sent!=got) {
            printf("[WATCHON] Audio send incomplete: %d/%d\n",sent,got);
            watchon_stream_eof=1;
            break;
        }
    }
}
static int watchon_load_settings_textures(GSGLOBAL *gsGlobal)
{
    int ok=1;
    gsKit_vram_clear(gsGlobal);
    memset(&watchon_font_tex,0,sizeof(watchon_font_tex));
    memset(&watchon_font_gold_tex,0,sizeof(watchon_font_gold_tex));
    ok &= load_ui_texture(gsGlobal,&logo_tex,"logo.bmp");
    ok &= load_ui_texture(gsGlobal,&watchon_font_tex,"Music/UI/aileron_atlas.bmp");
    ok &= load_ui_texture(gsGlobal,&watchon_font_gold_tex,"Music/UI/aileron_selected_atlas.bmp");
    printf("[WATCHON UI] Settings screen %s.\n",ok?"ready":"missing textures");
    return ok;
}
/* WATCHON_TRAX_V4_3_VISIBLE_SELECTION: legible, full-row selection feedback.
   Uses a second pre-rendered Aileron atlas (gold), so the selected text
   changes color on PS2 regardless of texture modulate/alpha peculiarities.
   All changes are confined to WATCHON TRAX menus, not music/episode systems. */
static void watchon_draw_text_style(GSGLOBAL *gs, const char *str,
                                   float x, float y, int max_chars, int selected)
{
    int i;
    GSTEXTURE *atlas=(selected && watchon_font_gold_tex.Vram)
        ? &watchon_font_gold_tex : &watchon_font_tex;
    if(!atlas->Vram || !str) return;
    for(i=0;str[i] && i<max_chars;i++) {
        unsigned char c=(unsigned char)str[i];
        int id, column, row;
        float u,v,px;
        if(c<32 || c>126) c='?';
        id=(int)c-32; column=id%24; row=id/24;
        u=(float)(column*18); v=(float)(row*25);
        px=x+(float)i*18.0f;
        gsKit_prim_sprite_texture(gs,atlas,
            px,y,u,v,px+18.0f,y+25.0f,u+18.0f,v+25.0f,
            4,GS_SETREG_RGBAQ(0x80,0x80,0x80,0x80,0));
    }
}
static void watchon_draw_text(GSGLOBAL *gs, const char *str,
                             float x, float y, int max_chars)
{
    watchon_draw_text_style(gs,str,x,y,max_chars,0);
}
/* A full focus frame, not the old near-invisible 5-pixel marker.
   Opaque GS primitives match the proven WATCHON loading-bar border fix. */
static void watchon_draw_selection(GSGLOBAL *gs, float y, float height)
{
    int prior=gs->PrimAlphaEnable;
    gs->PrimAlphaEnable=GS_SETTING_OFF;
    /* Rich gold border, dark inner row, solid bright left focus stripe. */
    gsKit_prim_sprite(gs,34.0f,y,610.0f,y+height,2,
        GS_SETREG_RGBAQ(0xF6,0xBB,0x52,0x80,0));
    gsKit_prim_sprite(gs,36.0f,y+2.0f,608.0f,y+height-2.0f,2,
        GS_SETREG_RGBAQ(0x18,0x19,0x23,0x80,0));
    gsKit_prim_sprite(gs,36.0f,y+2.0f,44.0f,y+height-2.0f,2,
        GS_SETREG_RGBAQ(0xFF,0xCC,0x64,0x80,0));
    gs->PrimAlphaEnable=prior;
}
static void watchon_draw_settings(GSGLOBAL *gs)
{
    int i;
    int count=watchon_count;
    int focus=watchon_settings_focus;
    char title[96];
    char counter[32];

    if(logo_tex.Vram) gsKit_prim_sprite_texture(gs,&logo_tex,
        470,14,0,0,622,90,(float)logo_tex.Width,(float)logo_tex.Height,
        1,GS_SETREG_RGBAQ(0x80,0x80,0x80,0x80,0));
    watchon_draw_text_style(gs,"WATCHON TRAX",48,48,20,1);
    gsKit_prim_sprite(gs,44,85,610,87,2,
        GS_SETREG_RGBAQ(0x90,0x96,0xA5,0x80,0));

    if(focus==0) watchon_draw_selection(gs,113.0f,33.0f);
    watchon_draw_text_style(gs,watchon_music_enabled?"PLAY MUSIC: ON":"PLAY MUSIC: OFF",
                           60,117,27,focus==0);
    if(focus==1) watchon_draw_selection(gs,149.0f,33.0f);
    watchon_draw_text_style(gs,watchon_order_random?"PLAY ORDER: RANDOM":"PLAY ORDER: SEQUENTIAL",
                           60,153,29,focus==1);

    watchon_draw_text(gs,"IMPORTED SONGS",48,198,25);
    snprintf(counter,sizeof(counter),"TRACKS %d",count);
    watchon_draw_text(gs,counter,430,198,10);
    gsKit_prim_sprite(gs,44,224,610,226,2,
        GS_SETREG_RGBAQ(0x5B,0x63,0x71,0x80,0));
    if(!count) watchon_draw_text(gs,"NO SONGS - USE PYTHON IMPORTER",48,245,30);

    for(i=0;i<5;i++) {
        int idx=watchon_settings_scroll+i;
        int y=237+i*32;
        int selected=(focus==idx+2);
        if(idx>=count) break;
        if(selected) watchon_draw_selection(gs,(float)y-3.0f,30.0f);
        snprintf(title,sizeof(title),"%s",watchon_tracks[idx].title);
        watchon_draw_text_style(gs,title,60,(float)y,22,selected);
        if(idx==watchon_current)
            watchon_draw_text_style(gs,"PLAYING",473,(float)y,7,1);
    }
    /* V4.4 on-screen shortcut: 30 glyphs x 18 pixels = 540px. */
    watchon_draw_text(gs,"R3:NEXT  X:PLAY  TRIANGLE:BACK",48,414,30);
}
/* WATCHON_MUSIC_V4_END */



/* ===== MAIN MENU SUPPORT ===== */

#define SCREEN_PRESS_START    0
#define SCREEN_MAIN_MENU      1
#define SCREEN_CREDITS        2
#define SCREEN_EPISODE_SELECT 3
#define SCREEN_OPTIONS        4
#define SCREEN_TRAX_SETTINGS 5

#define MENU_PLAY_ALL      0
#define MENU_EPISODES      1
#define MENU_CREDITS       2
#define MENU_OPTIONS       3
#define MENU_ITEM_COUNT    4

#define EPISODE_COUNT       52

/* BUNNY_UI_SFX_INPUT_BEGIN: only react to accepted menu inputs */
static void ui_sfx_handle_input(u32 pressed, int screen, int lock,
                                int title_frames, int title_delay)
{
    if (lock != 0 || pressed == 0)
        return;

    if (screen == SCREEN_PRESS_START) {
        if (title_frames >= title_delay && (pressed & (PAD_START | PAD_CROSS)))
            ui_sfx_play(UI_SFX_CONFIRM);
    } else if (screen == SCREEN_MAIN_MENU ||
               screen == SCREEN_EPISODE_SELECT ||
               screen == SCREEN_OPTIONS ||
               screen == SCREEN_TRAX_SETTINGS ||
               screen == SCREEN_CREDITS) {
        /* One sound per event; cancellation wins if buttons are combined. */
        if (pressed & PAD_TRIANGLE) {
            ui_sfx_play(UI_SFX_BACK);
        } else if (pressed & PAD_CROSS) {
            if (screen != SCREEN_CREDITS)
                ui_sfx_play(UI_SFX_CONFIRM);
        } else if (screen == SCREEN_MAIN_MENU && (pressed & (PAD_UP | PAD_DOWN))) {
            ui_sfx_play(UI_SFX_MOVE);
        } else if (screen == SCREEN_EPISODE_SELECT &&
                   (pressed & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT))) {
            ui_sfx_play(UI_SFX_MOVE);
        } else if ((screen == SCREEN_OPTIONS || screen == SCREEN_TRAX_SETTINGS) &&
                   (pressed & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT))) {
            ui_sfx_play(UI_SFX_MOVE);
        }
    }
}
/* BUNNY_UI_SFX_INPUT_END */




/*
 * Draw a REAL opaque black background.
 *
 * This is intentionally more aggressive than relying only
 * on gsKit_clear().
 *
 * Every UI screen should draw this FIRST.
 */
static void draw_black_background(
    GSGLOBAL *gsGlobal)
{
    /*
     * IMPORTANT:
     *
     * gsKit_clear() is implemented using GS sprite primitives.
     *
     * That means it obeys PrimAlphaEnable.
     *
     * If alpha blending is enabled while we clear, old framebuffer
     * contents can be blended with the new black background instead
     * of being unconditionally overwritten.
     *
     * Force alpha OFF only while writing the background.
     */
    int previous_alpha =
        gsGlobal->PrimAlphaEnable;

    gsGlobal->PrimAlphaEnable =
        GS_SETTING_OFF;


    /*
     * Guaranteed opaque framebuffer clear.
     */
    gsKit_clear(
        gsGlobal,
        GS_SETREG_RGBAQ(
            0x00,
            0x00,
            0x00,
            0x00,
            0x00
        )
    );


    /*
     * Also physically cover the complete framebuffer.
     *
     * Alpha is OFF here as well.
     */
    gsKit_prim_sprite(
        gsGlobal,

        0.0f,
        0.0f,

        640.0f,
        448.0f,

        0,

        GS_SETREG_RGBAQ(
            0x00,
            0x00,
            0x00,
            0x00,
            0x00
        )
    );


    /*
     * Restore alpha for transparent/fading UI textures.
     */
    gsGlobal->PrimAlphaEnable =
        previous_alpha;
}


static void clear_screen_transition_buffers(
    GSGLOBAL *gsGlobal)
{
    int i;

    /*
     * Aggressively overwrite/flips both buffers.
     *
     * Eight passes means old:
     *
     *   Loading
     *   Disclaimer
     *   Press Start
     *   Main Menu
     *
     * pixels should have nowhere left to survive.
     */
    for (i = 0; i < 8; i++) {

        draw_black_background(
            gsGlobal
        );

        gsKit_queue_exec(
            gsGlobal
        );

        gsKit_sync_flip(
            gsGlobal
        );
        /* V4.1: prevent starvation during eight transition flips. */
        watchon_music_pump();
    }
}


static int load_main_menu_textures(
    GSGLOBAL *gsGlobal)
{
    int success = 1;

    printf("[MENU] Loading Main Menu textures...\n");
    fflush(stdout);

    /*
     * The Press Start textures are no longer needed here,
     * so reuse GS VRAM for the Main Menu.
     */
    gsKit_vram_clear(gsGlobal);

    /*
     * press_start_tex pointed into the old VRAM layout.
     *
     * It MUST NOT be considered valid while the Main Menu
     * owns that VRAM.
     */
    memset(
        &press_start_tex,
        0,
        sizeof(press_start_tex)
    );


    success &= load_ui_texture(
        gsGlobal,
        &logo_tex,
        "logo.bmp"
    );

    success &= load_ui_texture(
        gsGlobal,
        &play_norm_tex,
        "play_normal.bmp"
    );

    success &= load_ui_texture(
        gsGlobal,
        &play_sel_tex,
        "play_selected.bmp"
    );

    success &= load_ui_texture(
        gsGlobal,
        &ep_norm_tex,
        "episodes_normal.bmp"
    );

    success &= load_ui_texture(
        gsGlobal,
        &ep_sel_tex,
        "episodes_selected.bmp"
    );

    success &= load_ui_texture(
        gsGlobal,
        &cred_norm_tex,
        "credits_normal.bmp"
    );

    success &= load_ui_texture(
        gsGlobal,
        &cred_sel_tex,
        "credits_selected.bmp"
    );

    success &= load_ui_texture(
        gsGlobal,
        &opt_norm_tex,
        "options_normal.bmp"
    );

    success &= load_ui_texture(
        gsGlobal,
        &opt_sel_tex,
        "options_selected.bmp"
    );

    if (success) {
        printf("[MENU] Main Menu textures ready.\n");
    } else {
        printf("[MENU ERROR] One or more Main Menu textures failed.\n");
    }

    fflush(stdout);

    return success;
}



static int load_credits_texture(
    GSGLOBAL *gsGlobal)
{
    printf(
        "[CREDITS] Loading credits image...\n"
    );

    fflush(stdout);


    /*
     * Credits gets its own VRAM state.
     *
     * We don't need the Main Menu button textures while
     * the Credits screen is being displayed.
     */
    gsKit_vram_clear(
        gsGlobal
    );


    memset(
        &credits_bg_tex,
        0,
        sizeof(credits_bg_tex)
    );


    int success =
        load_ui_texture(
            gsGlobal,
            &credits_bg_tex,
            "credits_bg.bmp"
        );


    if (success) {

        printf(
            "[CREDITS] Credits image ready.\n"
        );

    } else {

        printf(
            "[CREDITS ERROR] Could not load credits image.\n"
        );
    }

    fflush(stdout);

    return success;
}



/* ===== OPTIONS SCREEN ===== */

static int load_options_textures(
    GSGLOBAL *gsGlobal)
{
    int success = 1;

    printf(
        "[OPTIONS] Loading textures...\n"
    );

    fflush(stdout);


    gsKit_vram_clear(
        gsGlobal
    );


    memset(
        &l3_on_norm_tex,
        0,
        sizeof(l3_on_norm_tex)
    );

    memset(
        &l3_on_sel_tex,
        0,
        sizeof(l3_on_sel_tex)
    );

    memset(
        &l3_off_norm_tex,
        0,
        sizeof(l3_off_norm_tex)
    );

    memset(
        &l3_off_sel_tex,
        0,
        sizeof(l3_off_sel_tex)
    );


    success &= load_ui_texture(
        gsGlobal,
        &logo_tex,
        "logo.bmp"
    );


    success &= load_ui_texture(
        gsGlobal,
        &l3_on_norm_tex,
        "l3_skip_on_normal.bmp"
    );

    success &= load_ui_texture(
        gsGlobal,
        &l3_on_sel_tex,
        "l3_skip_on_selected.bmp"
    );

    success &= load_ui_texture(
        gsGlobal,
        &l3_off_norm_tex,
        "l3_skip_off_normal.bmp"
    );

    success &= load_ui_texture(
        gsGlobal,
        &l3_off_sel_tex,
        "l3_skip_off_selected.bmp"
    );


    success &= load_ui_texture(gsGlobal,&watchon_option_norm_tex,
                               "Music/UI/watchon_normal.bmp");
    success &= load_ui_texture(gsGlobal,&watchon_option_sel_tex,
                               "Music/UI/watchon_selected.bmp");

    if (success) {

        printf(
            "[OPTIONS] Textures ready.\n"
        );

    } else {

        printf(
            "[OPTIONS ERROR] One or more textures failed.\n"
        );
    }


    fflush(stdout);

    return success;
}


/* ===== END OPTIONS SCREEN ===== */


/* ===== EPISODE SELECTION V1 ===== */


/* ===== EPISODE SELECTION PAGED 1-52 ===== */

static int load_episode_selection_textures(
    GSGLOBAL *gsGlobal,
    int page)
{
    int success = 1;
    int i;
    int first_episode;

    char thumb_name[128];
    char title_name[128];

    GSTEXTURE *thumb_slots[3] = {
        &thumb_ep1,
        &thumb_ep2,
        &thumb_ep3
    };

    GSTEXTURE *title_slots[3] = {
        &ep1_title_tex,
        &ep2_title_tex,
        &ep3_title_tex
    };


    if (page < 0)
        page = 0;

    if (page > 17)
        page = 17;


    first_episode =
        (page * 3) + 1;

    int last_episode =
        first_episode + 2;

    if (last_episode > EPISODE_COUNT)
        last_episode = EPISODE_COUNT;


    printf(
        "[EPISODES] Loading Page %d (Episodes %d-%d)...\n",
        page + 1,
        first_episode,
        last_episode
    );

    fflush(stdout);


    /*
     * Only ONE page lives in GS VRAM at a time.
     */
    gsKit_vram_clear(
        gsGlobal
    );


    /*
     * Clear reused descriptors before loading a new page.
     * Page 18 only uses the first slot for Episode 52.
     */
    for (i = 0; i < 3; i++) {

        memset(
            thumb_slots[i],
            0,
            sizeof(GSTEXTURE)
        );

        memset(
            title_slots[i],
            0,
            sizeof(GSTEXTURE)
        );
    }


    success &= load_ui_texture(
        gsGlobal,
        &logo_tex,
        "logo.bmp"
    );


    /*
     * Three thumbnail slots.
     */
    for (i = 0; i < 3; i++) {

        int episode_number =
            first_episode + i;

        if (episode_number > EPISODE_COUNT)
            continue;

        snprintf(
            thumb_name,
            sizeof(thumb_name),
            "Thumbnails/thumb_ep%d_ps2.bmp",
            episode_number
        );

        success &= load_ui_texture(
            gsGlobal,
            thumb_slots[i],
            thumb_name
        );
    }


    success &= load_ui_texture(
        gsGlobal,
        &button_norm_tex,
        "button_normal.bmp"
    );

    success &= load_ui_texture(
        gsGlobal,
        &button_sel_tex,
        "button_selected.bmp"
    );


    /*
     * Three matching title slots.
     */
    for (i = 0; i < 3; i++) {

        int episode_number =
            first_episode + i;

        if (episode_number > EPISODE_COUNT)
            continue;

        snprintf(
            title_name,
            sizeof(title_name),
            "episode_titles/ep%02d_title.bmp",
            episode_number
        );

        success &= load_ui_texture(
            gsGlobal,
            title_slots[i],
            title_name
        );
    }


    if (success) {

        printf(
            "[EPISODES] Page %d textures ready.\n",
            page + 1
        );

    } else {

        printf(
            "[EPISODES ERROR] Page %d texture load failed.\n",
            page + 1
        );
    }


    fflush(stdout);

    return success;
}


static void draw_episode_title(
    GSGLOBAL *gsGlobal,
    GSTEXTURE *tex,
    float center_x,
    float y,
    int selected)
{
    if (!tex || tex->Vram == 0)
        return;


    /*
     * The source title texture is 256x64.
     *
     * Display around 145 pixels wide so three titles fit
     * underneath their respective episode cards.
     */
    float scale =
        0.57f;


    /*
     * Slightly enlarge the currently selected episode title.
     *
     * This gives another little visual cue without becoming
     * distracting.
     */
    if (selected) {
        scale =
            0.62f;
    }


    float w =
        tex->Width *
        scale;

    float h =
        tex->Height *
        scale;

    float x =
        center_x -
        (w / 2.0f);


    gsKit_prim_sprite_texture(
        gsGlobal,
        tex,

        x,
        y,

        0.0f,
        0.0f,

        x + w,
        y + h,

        tex->Width,
        tex->Height,

        3,

        GS_SETREG_RGBAQ(
            0x80,
            0x80,
            0x80,
            0x80,
            0
        )
    );
}


static void draw_episode_thumbnail(
    GSGLOBAL *gsGlobal,
    GSTEXTURE *tex,
    float center_x,
    float center_y,
    int selected)
{
    if (!tex || tex->Vram == 0)
        return;


    /*
     * ============================================================
     * EPISODE BUTTON
     * ============================================================
     *
     * Keep the complete bunny-button artwork visible.
     */

    GSTEXTURE *button_tex =
        selected
        ? &button_sel_tex
        : &button_norm_tex;


    const float CARD_W =
        190.0f;

    const float CARD_H =
        125.0f;


    float card_x =
        center_x -
        (CARD_W / 2.0f);

    float card_y =
        center_y -
        (CARD_H / 2.0f);


    if (
        button_tex &&
        button_tex->Vram != 0
    ) {

        gsKit_prim_sprite_texture(
            gsGlobal,
            button_tex,

            card_x,
            card_y,

            0.0f,
            0.0f,

            card_x + CARD_W,
            card_y + CARD_H,

            button_tex->Width,
            button_tex->Height,

            1,

            GS_SETREG_RGBAQ(
                0x80,
                0x80,
                0x80,
                0x80,
                0
            )
        );
    }


    /*
     * ============================================================
     * THUMBNAIL INNER AREA
     * ============================================================
     *
     * The bunny-button has ears at the top and decorative
     * framing around the outside.
     *
     * Therefore the image must NOT use the whole card.
     *
     * Put it inside the central rectangular panel instead.
     */

    /*
     * The visible rectangular FACE of the bunny button is much
     * smaller than the complete 190x125 texture because the texture
     * also contains the ears and lower decorative point.
     *
     * Keep the actual episode image well inside that central area
     * so the NORMAL / SELECTED button artwork remains visible.
     */
    /*
     * Fit completely inside the visible rectangular face
     * of the bunny button.
     *
     * The button texture contains large transparent areas,
     * ears and the pointed decoration underneath, so the
     * usable inner box is considerably smaller than CARD_W/H.
     */
    const float INNER_MAX_W =
        78.0f;

    const float INNER_MAX_H =
        44.0f;


    float scale_x =
        INNER_MAX_W /
        (float)tex->Width;

    float scale_y =
        INNER_MAX_H /
        (float)tex->Height;

    float scale =
        scale_x;

    if (scale_y < scale)
        scale = scale_y;


    float thumb_w =
        tex->Width *
        scale;

    float thumb_h =
        tex->Height *
        scale;


    /*
     * Move the thumbnail slightly DOWN.
     *
     * This keeps it away from the bunny ears
     * and centres it inside the rectangular part
     * of the button.
     */
    /*
     * The rectangular button face sits below the bunny ears,
     * so place the screenshot slightly lower than the card centre.
     */
    /*
     * Centre the image inside the rectangular panel.
     *
     * Do NOT push it down into the sharp decorative point.
     */
    float thumb_center_y =
        center_y +
        4.0f;


    float thumb_x =
        center_x -
        (thumb_w / 2.0f);

    float thumb_y =
        thumb_center_y -
        (thumb_h / 2.0f);


    gsKit_prim_sprite_texture(
        gsGlobal,
        tex,

        thumb_x,
        thumb_y,

        0.0f,
        0.0f,

        thumb_x + thumb_w,
        thumb_y + thumb_h,

        tex->Width,
        tex->Height,

        2,

        GS_SETREG_RGBAQ(
            0x80,
            0x80,
            0x80,
            0x80,
            0
        )
    );
}


/* ===== END EPISODE SELECTION V1 ===== */


static void draw_texture_scaled(
    GSGLOBAL *gsGlobal,
    GSTEXTURE *tex,
    float x,
    float y,
    float scale,
    int z)
{
    if (!tex || tex->Vram == 0)
        return;

    float w =
        (float)tex->Width *
        scale;

    float h =
        (float)tex->Height *
        scale;

    gsKit_prim_sprite_texture(
        gsGlobal,
        tex,

        x,
        y,

        0.0f,
        0.0f,

        x + w,
        y + h,

        tex->Width,
        tex->Height,

        z,

        GS_SETREG_RGBAQ(
            0x80,
            0x80,
            0x80,
            0x80,
            0x00
        )
    );
}


static void draw_menu_button(
    GSGLOBAL *gsGlobal,
    GSTEXTURE *tex,
    float center_x,
    float y)
{
    if (!tex || tex->Vram == 0)
        return;

    /*
     * Keep the user's original button artwork proportions.
     * Only shrink oversized artwork if necessary.
     */
    /*
     * Menu buttons are 256x128.
     *
     * 75% gives us:
     *   192 x 96
     *
     * This keeps the full artwork visible while giving enough
     * vertical room for all three buttons without overlap.
     */
    float scale = 0.75f;

    float w =
        (float)tex->Width *
        scale;

    float x =
        center_x -
        (w / 2.0f);

    draw_texture_scaled(
        gsGlobal,
        tex,
        x,
        y,
        scale,
        1
    );
}


/* ===== END MAIN MENU SUPPORT ===== */


int main(int argc, char *argv[]) {
    printf("=== Starting PS2 Application Debug Session ===\n");
    fflush(stdout);

    printf("[STEP 1] SifInitRpc(0)...\n");
    fflush(stdout);
    SifInitRpc(0);

    printf("[STEP 2] gsKit_init_global()...\n");
    fflush(stdout);
    GSGLOBAL *gsGlobal = gsKit_init_global();
    if (!gsGlobal) {
        printf("[FATAL ERROR] gsKit_init_global failed!\n");
        fflush(stdout);
        return -1;
    }

    printf("[STEP 3] Setting GS mode parameters...\n");
    fflush(stdout);
    gsGlobal->Mode = GS_MODE_NTSC;
    gsGlobal->Width = 640;
    gsGlobal->Height = 448;
    gsGlobal->Interlace = GS_INTERLACED;
    gsGlobal->Field = GS_FIELD;
    gsGlobal->DoubleBuffering = GS_SETTING_ON;
    gsGlobal->ZBuffering = GS_SETTING_OFF;

    printf("[STEP 4] dmaKit_init() with DMA_CHANNEL_GIF bitmask...\n");
    fflush(stdout);
    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, (1 << DMA_CHANNEL_GIF));

    printf("[STEP 5] dmaKit_chan_init(DMA_CHANNEL_GIF)...\n");
    fflush(stdout);
    dmaKit_chan_init(DMA_CHANNEL_GIF);

    printf("[STEP 6] gsKit_init_screen()...\n");
    fflush(stdout);
    gsKit_init_screen(gsGlobal);

    printf("[STEP 7] gsKit_mode_switch()...\n");
    fflush(stdout);
    gsKit_mode_switch(gsGlobal, GS_ONESHOT);

    show_debug_screen(gsGlobal, 0x00, 0x00, 0x00);

    printf("[STEP 9] Initializing Audio...\n");
    fflush(stdout);
    init_audio_subsystem();
    ui_sfx_init();

    printf("[STEP 10] Initializing Pad...\n");
    fflush(stdout);
    init_pad_subsystem();


    /*
     * Bunny Maloney boot presentation:
     * Loading -> Memory Card check/load -> Disclaimer
     * -> existing Press Start screen.
     *
     * IMPORTANT:
     * This happens before the persistent menu textures are loaded.
     */
    show_boot_screens(gsGlobal);

    /* Library only: decoded PCM is streamed; never loaded into GS VRAM. */
    srand((unsigned int)GetTimerSystemTime());
    watchon_load_library();
    if (watchon_music_enabled && watchon_count>0)
        (void)watchon_start_track(watchon_order_random ? rand()%watchon_count : 0);
    /* Preserve the title notification's original 1.5-second delay. */
    trax_v2_delay_remaining=90;

    printf("[STEP 11] Loading Textures...\n");
    fflush(stdout);
    load_all_menu_textures(gsGlobal);
    watchon_assets_dirty=0;

    int state_transition_lock = 0;
    u32 old_pad = 0;
    int pad_debug_counter = 0;

    /*
     * Application screens.
     */
    int screen_state =
        SCREEN_PRESS_START;

    int menu_selection =
        MENU_PLAY_ALL;

    /*
     * Episode Selection:
     *
     * 0-2   = Page 1
     * 3-5   = Page 2
     * 6-8   = Page 3
     * 9-11  = Page 4
     * 12-14 = Page 5
     * 15-50 = Pages 6-17
     * 51    = Page 18 (Episode 52 only)
     */
    int selected_episode =
        0;

    int episode_page =
        0;

    int pending_episode_number =
        1;


    /*
     * Loading a new screen clears/reuses GS VRAM.
     *
     * We defer that until the NEXT frame so the previous
     * screen can finish rendering safely.
     */
    int screen_resources_dirty = 0;

    /*
     * Actions are also deferred until the next loop.
     *
     * This prevents playback from being launched while
     * menu primitives are still queued for rendering.
     */
    int pending_action = 0;


    /*
     * Press Start presentation timing.
     *
     * 180 frames = ~3 seconds at 60 FPS; notification starts at 90.
     * After that, the prompt continuously fades in and out.
     */
    const int PRESS_START_DELAY_FRAMES = 180;
    const int PRESS_START_FADE_FRAMES  = 45;

    int press_start_timer = 0;

    gsGlobal->PrimAlphaEnable = GS_SETTING_ON;

    printf("[STEP 12] Entering Main Loop...\n");
    fflush(stdout);

    while (1) {
        if (state_transition_lock > 0) {
            state_transition_lock--;
        }

        /*
         * ------------------------------------------------------------
         * DEFERRED ACTIONS
         * ------------------------------------------------------------
         */

        /*
         * ------------------------------------------------------------
         * DEFERRED PLAYBACK
         * ------------------------------------------------------------
         *
         * pending_action:
         *
         *   0 = nothing
         *   1 = Play All (Episodes 1-52)
         *   2 = selected episode
         */

        if (
            pending_action == 1 ||
            pending_action == 2
        ) {

            /* Exclusive audsrv ownership transfers to the episode player. */
            watchon_stop_music();

            int action =
                pending_action;

            pending_action =
                0;


            char episode_folder[32];
            char episode_audio[32];


            /*
             * =========================================================
             * PLAY ALL
             * =========================================================
             *
             * Play Episodes 1 through 52 sequentially.
             *
             * The next episode starts only when the previous episode
             * genuinely reaches the end of both its video and audio.
             *
             * Triangle / START cancels the current episode and therefore
             * also stops the complete Play All sequence.
             */
            if (action == 1) {

                int play_all_episode;

                printf(
                    "[PLAY ALL] Starting Episodes 1-%d...\n",
                    EPISODE_COUNT
                );

                fflush(stdout);


                for (
                    play_all_episode = 1;
                    play_all_episode <= EPISODE_COUNT;
                    play_all_episode++
                ) {

                    snprintf(
                        episode_folder,
                        sizeof(episode_folder),
                        "episode%d",
                        play_all_episode
                    );

                    snprintf(
                        episode_audio,
                        sizeof(episode_audio),
                        "episode%d.wav",
                        play_all_episode
                    );


                    printf(
                        "[PLAY ALL] Episode %d / %d\n",
                        play_all_episode,
                        EPISODE_COUNT
                    );

                    fflush(stdout);


                    play_video_file(
                        gsGlobal,
                        episode_audio,
                        episode_folder
                    );


                    /*
                     * Natural ending:
                     * continue to the next episode.
                     *
                     * Triangle / START / playback failure:
                     * stop the whole sequence.
                     */
                    if (!playback_completed_naturally) {

                        printf(
                            "[PLAY ALL] Stopped during Episode %d.\n",
                            play_all_episode
                        );

                        fflush(stdout);

                        break;
                    }


                    printf(
                        "[PLAY ALL] Episode %d finished normally.\n",
                        play_all_episode
                    );

                    fflush(stdout);


                    /*
                     * Short clean transition between episodes.
                     *
                     * Do not add it after Episode 52 because Play All
                     * is about to return to the Main Menu anyway.
                     */
                    if (
                        play_all_episode <
                        EPISODE_COUNT
                    ) {

                        int transition_frame;

                        printf(
                            "[PLAY ALL] Preparing Episode %d...\n",
                            play_all_episode + 1
                        );

                        fflush(stdout);


                        /*
                         * About half a second at NTSC refresh.
                         */
                        for (
                            transition_frame = 0;
                            transition_frame < 30;
                            transition_frame++
                        ) {

                            draw_black_background(
                                gsGlobal
                            );

                            gsKit_queue_exec(
                                gsGlobal
                            );

                            gsKit_sync_flip(
                                gsGlobal
                            );
                        }
                    }
                }


                if (
                    play_all_episode >
                    EPISODE_COUNT
                ) {

                    printf(
                        "[PLAY ALL] All %d episodes completed.\n",
                        EPISODE_COUNT
                    );

                    fflush(stdout);
                }
            }


            /*
             * Individual Episode Selection playback.
             */
            else {

                int episode_number =
                    pending_episode_number;


                if (
                    episode_number < 1 ||
                    episode_number > EPISODE_COUNT
                ) {

                    printf(
                        "[EPISODES WARNING] Invalid Episode %d; "
                        "using Episode 1.\n",
                        episode_number
                    );

                    episode_number =
                        1;
                }


                snprintf(
                    episode_folder,
                    sizeof(episode_folder),
                    "episode%d",
                    episode_number
                );

                snprintf(
                    episode_audio,
                    sizeof(episode_audio),
                    "episode%d.wav",
                    episode_number
                );


                printf(
                    "[EPISODES] Launching Episode %d...\n",
                    episode_number
                );
            }


            fflush(stdout);


            /*
             * Individual Episode Selection still launches exactly
             * one episode here.
             *
             * Play All already launches its episodes inside the
             * sequential loop above.
             */
            if (action == 2) {

                play_video_file(
                    gsGlobal,
                    episode_audio,
                    episode_folder
                );


                printf(
                    "[VIDEO] Returned from playback.\n"
                );

                fflush(stdout);
            }


            /*
             * The player clears GS texture VRAM.
             *
             * Play All returns to Main Menu.
             *
             * Individual episode playback returns to the
             * Episode Selection screen.
             */
            if (action == 2) {

                screen_state =
                    SCREEN_EPISODE_SELECT;

            } else {

                screen_state =
                    SCREEN_MAIN_MENU;
            }


            /* V3.4: episode playback has reset GS VRAM. When we return to
             * Episode Select / Main Menu, re-show the DEMO notification.
             * This does not yet select a song or play audio.
             */
            trax_v2_frame = 0;
            trax_v2_started = 0;
            trax_v2_delay_remaining = 0;
            trax_v2_ready = 0;
            printf("[WATCHON] Episode returned; choosing another song.\n");
            if (watchon_music_enabled && watchon_count>0)
                watchon_start_different();

            screen_resources_dirty =
                1;

            old_pad =
                0;

            state_transition_lock =
                20;
        }


        /*
         * ------------------------------------------------------------
         * SCREEN RESOURCE CHANGES
         * ------------------------------------------------------------
         */

        /* WATCHON_TRAX_V4_5_NO_BLACK_SONG_SWITCH:
         * Only track artwork changed, not the destination screen.
         * Keep the previous menu displayed during the texture refresh.
         * Real screen changes retain the original double-buffer clearing.
         */
        int watchon_same_screen_refresh = 0;
        if (watchon_assets_dirty) {
            if (!screen_resources_dirty && screen_state != SCREEN_CREDITS)
                watchon_same_screen_refresh = 1;
            screen_resources_dirty = 1;
            watchon_assets_dirty = 0;
        }
        if (screen_resources_dirty) {

            screen_resources_dirty =
                0;

            /*
             * Remove anything left in either framebuffer from
             * the previous screen BEFORE loading/drawing the
             * next screen.
             */
            if (!watchon_same_screen_refresh) {
                clear_screen_transition_buffers(
                    gsGlobal
                );
            }


            if (screen_state ==
                SCREEN_PRESS_START) {

                load_all_menu_textures(
                    gsGlobal
                );

            } else if (
                screen_state ==
                SCREEN_MAIN_MENU) {

                load_main_menu_textures(
                    gsGlobal
                );

            } else if (
                screen_state ==
                SCREEN_CREDITS) {

                load_credits_texture(
                    gsGlobal
                );

            } else if (
                screen_state ==
                SCREEN_EPISODE_SELECT) {

                load_episode_selection_textures(
                    gsGlobal,
                    episode_page
                );

            } else if (
                screen_state ==
                SCREEN_OPTIONS) {

                load_options_textures(
                    gsGlobal
                );
            } else if (screen_state == SCREEN_TRAX_SETTINGS) {
                watchon_load_settings_textures(gsGlobal);
            }


            /* V3.4: every destination screen has a fresh VRAM layout.
             * The Press Start loader already loads TRAX itself.
             * Load it LAST elsewhere so it stays in front of each menu,
             * but never restart its animation clock here.
             */
            /* V3.6.1: Credits consumes its own VRAM layout and must
             * never display TRAX or allocate its 8 disc textures.
             * The animation clock is preserved for returning to a menu.
             */
            if (screen_state == SCREEN_CREDITS) {
                trax_v2_ready = 0;
            } else if (screen_state != SCREEN_PRESS_START) {
                if (trax_v2_frame < TRAX_V2_TOTAL_FRAMES) {
                    trax_v2_load_title(gsGlobal);
                } else {
                    trax_v2_ready = 0;
                }
            }

            /*
             * gsKit_vram_clear() resets texture allocation but
             * does not itself erase old pixels already displayed.
             *
             * Make sure both visible buffers are clean after
             * loading the new screen's resources as well.
             */
            if (!watchon_same_screen_refresh) {
                clear_screen_transition_buffers(
                    gsGlobal
                );
            }
        }

        /*
         * Always paint a complete opaque black background
         * before drawing ANY screen-specific graphics.
         */
        draw_black_background(
            gsGlobal
        );


        /*
         * ============================================================
         * PRESS START SCREEN
         * ============================================================
         */

        if (screen_state ==
            SCREEN_PRESS_START) {

            /*
             * Smaller centred title logo.
             */

            if (logo_tex.Vram != 0) {

                float logo_scale =
                    0.60f;

                float logo_w =
                    (float)logo_tex.Width *
                    logo_scale;

                float logo_h =
                    (float)logo_tex.Height *
                    logo_scale;

                float logo_x =
                    (640.0f - logo_w) /
                    2.0f;

                float logo_y =
                    (448.0f - logo_h) /
                    2.0f;


                gsKit_prim_sprite_texture(
                    gsGlobal,
                    &logo_tex,

                    logo_x,
                    logo_y,

                    0.0f,
                    0.0f,

                    logo_x + logo_w,
                    logo_y + logo_h,

                    logo_tex.Width,
                    logo_tex.Height,

                    1,

                    GS_SETREG_RGBAQ(
                        0x80,
                        0x80,
                        0x80,
                        0x80,
                        0x00
                    )
                );
            }


            /*
             * Press Start presentation.
             *
             * Wait about three seconds before showing it,
             * then continuously fade in and out.
             */

            press_start_timer++;

            int press_start_ready =
                press_start_timer >=
                PRESS_START_DELAY_FRAMES;


            if (press_start_tex.Vram != 0 &&
                press_start_ready) {

                int animation_frame =
                    press_start_timer -
                    PRESS_START_DELAY_FRAMES;

                int cycle_length =
                    PRESS_START_FADE_FRAMES *
                    2;

                int phase =
                    animation_frame %
                    cycle_length;

                int press_shade;


                if (phase <
                    PRESS_START_FADE_FRAMES) {

                    press_shade =
                        (phase * 0x80) /
                        (
                            PRESS_START_FADE_FRAMES -
                            1
                        );

                } else {

                    int fade_out_frame =
                        phase -
                        PRESS_START_FADE_FRAMES;

                    press_shade =
                        0x80 -
                        (
                            (
                                fade_out_frame *
                                0x80
                            )
                            /
                            (
                                PRESS_START_FADE_FRAMES -
                                1
                            )
                        );
                }


                if (press_shade < 0)
                    press_shade = 0;

                if (press_shade > 0x80)
                    press_shade = 0x80;


                float press_w =
                    (float)press_start_tex.Width;

                float press_h =
                    (float)press_start_tex.Height;

                float press_x =
                    (640.0f - press_w) /
                    2.0f;

                float press_y =
                    320.0f;


                gsKit_prim_sprite_texture(
                    gsGlobal,
                    &press_start_tex,

                    press_x,
                    press_y,

                    0.0f,
                    0.0f,

                    press_x + press_w,
                    press_y + press_h,

                    press_start_tex.Width,
                    press_start_tex.Height,

                    1,

                    GS_SETREG_RGBAQ(
                        press_shade,
                        press_shade,
                        press_shade,
                        0x80,
                        0x00
                    )
                );
            }
        }


        /*
         * ============================================================
         * MAIN MENU
         * ============================================================
         */

        else if (
            screen_state ==
            SCREEN_MAIN_MENU) {

            /*
             * Smaller persistent logo in the top-right.
             */

            if (logo_tex.Vram != 0) {

                float logo_scale =
                    0.30f;

                float logo_w =
                    (float)logo_tex.Width *
                    logo_scale;

                float logo_h =
                    (float)logo_tex.Height *
                    logo_scale;

                /*
                 * Never allow an oversized menu logo.
                 */
                if (logo_w > 245.0f) {

                    logo_scale *=
                        245.0f /
                        logo_w;

                    logo_w =
                        (float)logo_tex.Width *
                        logo_scale;

                    logo_h =
                        (float)logo_tex.Height *
                        logo_scale;
                }

                float logo_x =
                    640.0f -
                    logo_w -
                    20.0f;

                float logo_y =
                    20.0f;


                draw_texture_scaled(
                    gsGlobal,
                    &logo_tex,
                    logo_x,
                    logo_y,
                    logo_scale,
                    1
                );
            }


            /*
             * Pick the correct NORMAL / SELECTED texture
             * for each option.
             */

            GSTEXTURE *play_tex =
                (
                    menu_selection ==
                    MENU_PLAY_ALL
                )
                ? &play_sel_tex
                : &play_norm_tex;


            GSTEXTURE *episodes_tex =
                (
                    menu_selection ==
                    MENU_EPISODES
                )
                ? &ep_sel_tex
                : &ep_norm_tex;


            GSTEXTURE *credits_tex =
                (
                    menu_selection ==
                    MENU_CREDITS
                )
                ? &cred_sel_tex
                : &cred_norm_tex;


            GSTEXTURE *options_tex =
                (
                    menu_selection ==
                    MENU_OPTIONS
                )
                ? &opt_sel_tex
                : &opt_norm_tex;


            /*
             * Four Main Menu buttons.
             *
             * Keep them down the left side while the logo remains
             * in the top-right.
             */

            const float MENU_CENTER_X =
                205.0f;


            draw_menu_button(
                gsGlobal,
                play_tex,
                MENU_CENTER_X,
                25.0f
            );

            draw_menu_button(
                gsGlobal,
                episodes_tex,
                MENU_CENTER_X,
                130.0f
            );

            draw_menu_button(
                gsGlobal,
                credits_tex,
                MENU_CENTER_X,
                235.0f
            );

            draw_menu_button(
                gsGlobal,
                options_tex,
                MENU_CENTER_X,
                340.0f
            );
        }



        /*
         * ============================================================
         * OPTIONS
         * ============================================================
         */

        else if (
            screen_state ==
            SCREEN_OPTIONS) {

            /*
             * Same small top-right logo used elsewhere.
             */
            if (logo_tex.Vram != 0) {

                float logo_scale =
                    0.30f;

                float logo_w =
                    logo_tex.Width *
                    logo_scale;

                float logo_h =
                    logo_tex.Height *
                    logo_scale;


                if (logo_w > 245.0f) {

                    logo_scale *=
                        245.0f /
                        logo_w;

                    logo_w =
                        logo_tex.Width *
                        logo_scale;

                    logo_h =
                        logo_tex.Height *
                        logo_scale;
                }


                float logo_x =
                    640.0f -
                    logo_w -
                    20.0f;

                float logo_y =
                    20.0f;


                draw_texture_scaled(
                    gsGlobal,
                    &logo_tex,
                    logo_x,
                    logo_y,
                    logo_scale,
                    1
                );
            }


            /* Two-button Options menu. Existing L3 settings stay intact. */
            GSTEXTURE *l3_setting_tex = option_l3_skip_enabled
                ? (watchon_options_focus==0 ? &l3_on_sel_tex : &l3_on_norm_tex)
                : (watchon_options_focus==0 ? &l3_off_sel_tex : &l3_off_norm_tex);
            draw_menu_button(gsGlobal,l3_setting_tex,320.0f,157.0f);
            draw_menu_button(gsGlobal,
                watchon_options_focus==1 ? &watchon_option_sel_tex : &watchon_option_norm_tex,
                320.0f,266.0f);
        }


        /* WATCHON TRAX settings + imported track browser. */
        else if (screen_state == SCREEN_TRAX_SETTINGS) {
            watchon_draw_settings(gsGlobal);
        }

        /*
         * ============================================================
         * EPISODE SELECTION
         * ============================================================
         */

        else if (
            screen_state ==
            SCREEN_EPISODE_SELECT) {

            /*
             * Keep the Bunny Maloney logo in the top-right,
             * matching the Main Menu style.
             */
            if (logo_tex.Vram != 0) {

                float logo_scale =
                    0.30f;

                float logo_w =
                    logo_tex.Width *
                    logo_scale;

                float logo_h =
                    logo_tex.Height *
                    logo_scale;


                if (logo_w > 245.0f) {

                    logo_scale *=
                        245.0f /
                        logo_w;

                    logo_w =
                        logo_tex.Width *
                        logo_scale;

                    logo_h =
                        logo_tex.Height *
                        logo_scale;
                }


                float logo_x =
                    640.0f -
                    logo_w -
                    20.0f;

                float logo_y =
                    20.0f;


                draw_texture_scaled(
                    gsGlobal,
                    &logo_tex,
                    logo_x,
                    logo_y,
                    logo_scale,
                    1
                );
            }


            /*
             * The same three UI slots are reused on every page.
             */
            int page_first_selection =
                episode_page * 3;


            /*
             * Pages 1-17 contain three episodes.
             * Page 18 contains Episode 52 only.
             */
            if (episode_page == 17) {

                draw_episode_thumbnail(
                    gsGlobal,
                    &thumb_ep1,
                    320.0f,
                    245.0f,
                    selected_episode ==
                    page_first_selection
                );

                draw_episode_title(
                    gsGlobal,
                    &ep1_title_tex,
                    320.0f,
                    320.0f,
                    selected_episode ==
                    page_first_selection
                );

            } else {

                draw_episode_thumbnail(
                    gsGlobal,
                    &thumb_ep1,
                    110.0f,
                    245.0f,
                    selected_episode ==
                    page_first_selection
                );


                draw_episode_thumbnail(
                    gsGlobal,
                    &thumb_ep2,
                    320.0f,
                    245.0f,
                    selected_episode ==
                    page_first_selection + 1
                );


                draw_episode_thumbnail(
                    gsGlobal,
                    &thumb_ep3,
                    530.0f,
                    245.0f,
                    selected_episode ==
                    page_first_selection + 2
                );


                draw_episode_title(
                    gsGlobal,
                    &ep1_title_tex,
                    110.0f,
                    320.0f,
                    selected_episode ==
                    page_first_selection
                );


                draw_episode_title(
                    gsGlobal,
                    &ep2_title_tex,
                    320.0f,
                    320.0f,
                    selected_episode ==
                    page_first_selection + 1
                );


                draw_episode_title(
                    gsGlobal,
                    &ep3_title_tex,
                    530.0f,
                    320.0f,
                    selected_episode ==
                    page_first_selection + 2
                );
            }
        }


        /*
         * ============================================================
         * CREDITS
         * ============================================================
         */

        else if (
            screen_state ==
            SCREEN_CREDITS) {

            if (credits_bg_tex.Vram != 0) {

                /*
                 * Preserve the original image aspect ratio.
                 *
                 * Maximum display area:
                 *     620 x 428
                 *
                 * giving us a small black border around the artwork.
                 */

                float credits_scale =
                    1.0f;

                float credits_w =
                    (float)credits_bg_tex.Width;

                float credits_h =
                    (float)credits_bg_tex.Height;


                if (credits_w > 620.0f) {

                    credits_scale =
                        620.0f /
                        credits_w;
                }


                if (
                    credits_h *
                    credits_scale >
                    428.0f
                ) {

                    credits_scale =
                        428.0f /
                        credits_h;
                }


                credits_w =
                    credits_bg_tex.Width *
                    credits_scale;

                credits_h =
                    credits_bg_tex.Height *
                    credits_scale;


                float credits_x =
                    (640.0f - credits_w) /
                    2.0f;

                float credits_y =
                    (448.0f - credits_h) /
                    2.0f;


                draw_texture_scaled(
                    gsGlobal,
                    &credits_bg_tex,
                    credits_x,
                    credits_y,
                    credits_scale,
                    1
                );
            }
        }

        if (pad_initialized) {

            int pad_state =
                padGetState(0, 0);


            /*
             * Keep the existing occasional controller debug.
             */
            if ((pad_debug_counter++ % 120) == 0) {

                char state_name[16] =
                    {0};

                padStateInt2String(
                    pad_state,
                    state_name
                );

                printf(
                    "[PAD] State = %d (%s)\n",
                    pad_state,
                    state_name
                );

                fflush(stdout);
            }


            if (pad_state ==
                PAD_STATE_STABLE) {

                struct padButtonStatus buttons;

                memset(
                    &buttons,
                    0,
                    sizeof(buttons)
                );


                if (padRead(
                        0,
                        0,
                        &buttons) != 0) {

                    u32 paddata =
                        0xffff ^
                        buttons.btns;

                    u32 new_pad =
                        paddata &
                        ~old_pad;

                    old_pad =
                        paddata;

                    ui_sfx_handle_input(new_pad, screen_state,
                                        state_transition_lock,
                                        press_start_timer,
                                        PRESS_START_DELAY_FRAMES);

                    /* WATCHON_TRAX_V4_4_R3_SKIP: one R3 click = one next track.
                       Reuse the proven sequential/random picker and preserve
                       all episode-player and animation code unchanged. */
                    if ((new_pad & PAD_R3) &&
                        state_transition_lock == 0 &&
                        watchon_music_enabled &&
                        watchon_music_playing &&
                        watchon_count > 0 &&
                        (screen_state == SCREEN_PRESS_START ||
                         screen_state == SCREEN_MAIN_MENU ||
                         screen_state == SCREEN_EPISODE_SELECT ||
                         screen_state == SCREEN_OPTIONS ||
                         screen_state == SCREEN_TRAX_SETTINGS)) {
                        printf("[WATCHON] R3: next track (%s).\n",
                               watchon_order_random ? "random" : "sequential");
                        fflush(stdout);
                        watchon_start_different();
                    }

                    if (new_pad != 0) {

                        printf(
                            "[PAD] Button mask = 0x%04X\n",
                            (unsigned int)new_pad
                        );

                        fflush(stdout);
                    }


                    /*
                     * ==================================================
                     * PRESS START CONTROLS
                     * ==================================================
                     */

                    if (screen_state ==
                        SCREEN_CREDITS) {

                        if (
                            state_transition_lock ==
                            0 &&
                            (
                                new_pad &
                                PAD_TRIANGLE
                            )
                        ) {

                            printf(
                                "[CREDITS] Returning to Main Menu.\n"
                            );

                            fflush(stdout);


                            screen_state =
                                SCREEN_MAIN_MENU;

                            screen_resources_dirty =
                                1;

                            menu_selection =
                                MENU_CREDITS;

                            state_transition_lock =
                                15;
                        }
                    }


                    else if (screen_state == SCREEN_OPTIONS) {
                        if (state_transition_lock == 0) {
                            if (new_pad & (PAD_UP | PAD_DOWN))
                                watchon_options_focus = 1-watchon_options_focus;
                            if (watchon_options_focus == 0 &&
                                (new_pad & (PAD_LEFT | PAD_RIGHT | PAD_CROSS))) {
                                option_l3_skip_enabled = !option_l3_skip_enabled;
                                printf("[OPTIONS] L3 Skip: %s\n",
                                    option_l3_skip_enabled?"ON":"OFF");
#if ENABLE_MEMORY_CARD_IO
                                memory_card_save_settings();
#endif
                            } else if (watchon_options_focus == 1 &&
                                       (new_pad & PAD_CROSS)) {
                                screen_state=SCREEN_TRAX_SETTINGS;
                                screen_resources_dirty=1;
                                watchon_settings_focus=0;
                                watchon_settings_scroll=0;
                                state_transition_lock=15;
                            }
                            if (new_pad & PAD_TRIANGLE) {
                                screen_state=SCREEN_MAIN_MENU;
                                screen_resources_dirty=1;
                                menu_selection=MENU_OPTIONS;
                                state_transition_lock=15;
                            }
                        }
                    }

                    else if (screen_state == SCREEN_TRAX_SETTINGS) {
                        if (state_transition_lock == 0) {
                            int max_focus=watchon_count+1;
                            if (new_pad & PAD_UP) {
                                watchon_settings_focus--;
                                if(watchon_settings_focus<0) watchon_settings_focus=max_focus;
                            } else if (new_pad & PAD_DOWN) {
                                watchon_settings_focus++;
                                if(watchon_settings_focus>max_focus) watchon_settings_focus=0;
                            }
                            if(watchon_settings_focus>=2) {
                                int row=watchon_settings_focus-2;
                                if(row<watchon_settings_scroll) watchon_settings_scroll=row;
                                if(row>=watchon_settings_scroll+5)
                                    watchon_settings_scroll=row-4;
                            }
                            if (new_pad & (PAD_CROSS|PAD_LEFT|PAD_RIGHT)) {
                                if(watchon_settings_focus==0) {
                                    watchon_music_enabled=!watchon_music_enabled;
                                    if(!watchon_music_enabled) watchon_stop_music();
                                    else watchon_start_different();
                                    watchon_save_settings();
                                } else if(watchon_settings_focus==1) {
                                    watchon_order_random=!watchon_order_random;
                                    watchon_save_settings();
                                } else if((new_pad & PAD_CROSS) &&
                                          watchon_settings_focus-2<watchon_count) {
                                    if(!watchon_music_enabled) {
                                        watchon_music_enabled=1;
                                        watchon_save_settings();
                                    }
                                    (void)watchon_start_track(watchon_settings_focus-2);
                                }
                            }
                            if(new_pad & PAD_TRIANGLE) {
                                screen_state=SCREEN_OPTIONS;
                                screen_resources_dirty=1;
                                watchon_options_focus=1;
                                state_transition_lock=15;
                            }
                        }
                    }

                    else if (screen_state ==
                        SCREEN_PRESS_START) {

                        int press_start_ready =
                            press_start_timer >=
                            PRESS_START_DELAY_FRAMES;


                        if (
                            press_start_ready &&
                            (
                                (new_pad &
                                 PAD_START) ||

                                (new_pad &
                                 PAD_CROSS)
                            ) &&
                            state_transition_lock == 0
                        ) {

                            printf(
                                "[UI] Entering Main Menu.\n"
                            );

                            fflush(stdout);

                            screen_state =
                                SCREEN_MAIN_MENU;

                            menu_selection =
                                MENU_PLAY_ALL;

                            screen_resources_dirty =
                                1;

                            state_transition_lock =
                                15;
                        }
                    }


                    /*
                     * ==================================================
                     * MAIN MENU CONTROLS
                     * ==================================================
                     */

                    /*
                     * ==================================================
                     * EPISODE SELECTION CONTROLS
                     * ==================================================
                     */

                    else if (
                        screen_state ==
                        SCREEN_EPISODE_SELECT) {

                        if (
                            state_transition_lock ==
                            0
                        ) {

                            /*
                             * Previous Episode.
                             */
                            if (
                                (new_pad & PAD_LEFT) ||
                                (new_pad & PAD_UP)
                            ) {

                                int old_page =
                                    episode_page;

                                selected_episode--;


                                if (
                                    selected_episode <
                                    0
                                ) {

                                    selected_episode =
                                        EPISODE_COUNT - 1;
                                }


                                episode_page =
                                    selected_episode /
                                    3;


                                if (
                                    episode_page !=
                                    old_page
                                ) {

                                    screen_resources_dirty =
                                        1;

                                    state_transition_lock =
                                        8;
                                }


                                printf(
                                    "[EPISODES] Page %d | Episode %d\n",
                                    episode_page + 1,
                                    selected_episode + 1
                                );

                                fflush(stdout);
                            }


                            /*
                             * Next Episode.
                             */
                            if (
                                (new_pad & PAD_RIGHT) ||
                                (new_pad & PAD_DOWN)
                            ) {

                                int old_page =
                                    episode_page;

                                selected_episode++;


                                if (
                                    selected_episode >=
                                    EPISODE_COUNT
                                ) {

                                    selected_episode =
                                        0;
                                }


                                episode_page =
                                    selected_episode /
                                    3;


                                if (
                                    episode_page !=
                                    old_page
                                ) {

                                    screen_resources_dirty =
                                        1;

                                    state_transition_lock =
                                        8;
                                }


                                printf(
                                    "[EPISODES] Page %d | Episode %d\n",
                                    episode_page + 1,
                                    selected_episode + 1
                                );

                                fflush(stdout);
                            }


                            /*
                             * Play.
                             */
                            if (
                                new_pad &
                                PAD_CROSS
                            ) {

                                pending_episode_number =
                                    selected_episode +
                                    1;

                                pending_action =
                                    2;

                                state_transition_lock =
                                    20;
                            }


                            /*
                             * Back.
                             */
                            if (
                                new_pad &
                                PAD_TRIANGLE
                            ) {

                                printf(
                                    "[EPISODES] Returning to Main Menu.\n"
                                );

                                fflush(stdout);


                                screen_state =
                                    SCREEN_MAIN_MENU;

                                screen_resources_dirty =
                                    1;

                                menu_selection =
                                    MENU_EPISODES;

                                state_transition_lock =
                                    15;
                            }
                        }
                    }


                    else if (
                        screen_state ==
                        SCREEN_MAIN_MENU) {

                        if (
                            state_transition_lock ==
                            0
                        ) {

                            /*
                             * Move selection UP.
                             */
                            if (new_pad &
                                PAD_UP) {

                                menu_selection--;

                                if (
                                    menu_selection <
                                    0
                                ) {
                                    menu_selection =
                                        MENU_ITEM_COUNT -
                                        1;
                                }

                                printf(
                                    "[MENU] Selection: %d\n",
                                    menu_selection
                                );

                                fflush(stdout);
                            }


                            /*
                             * Move selection DOWN.
                             */
                            if (new_pad &
                                PAD_DOWN) {

                                menu_selection++;

                                if (
                                    menu_selection >=
                                    MENU_ITEM_COUNT
                                ) {
                                    menu_selection =
                                        0;
                                }

                                printf(
                                    "[MENU] Selection: %d\n",
                                    menu_selection
                                );

                                fflush(stdout);
                            }


                            /*
                             * Select option.
                             */
                            if (new_pad &
                                PAD_CROSS) {

                                if (
                                    menu_selection ==
                                    MENU_PLAY_ALL
                                ) {

                                    /*
                                     * Defer playback until next loop.
                                     */
                                    pending_action =
                                        1;

                                } else if (
                                    menu_selection ==
                                    MENU_EPISODES
                                ) {

                                    printf(
                                        "[MENU] Entering Episode Selection.\n"
                                    );

                                    fflush(stdout);

                                    screen_state =
                                        SCREEN_EPISODE_SELECT;

                                    screen_resources_dirty =
                                        1;

                                    selected_episode =
                                        0;

                                    episode_page =
                                        0;

                                    state_transition_lock =
                                        15;

                                } else if (
                                    menu_selection ==
                                    MENU_CREDITS
                                ) {

                                    printf(
                                        "[MENU] Entering Credits.\n"
                                    );

                                    fflush(stdout);

                                    screen_state =
                                        SCREEN_CREDITS;

                                    screen_resources_dirty =
                                        1;

                                    state_transition_lock =
                                        15;

                                } else if (
                                    menu_selection ==
                                    MENU_OPTIONS
                                ) {

                                    printf(
                                        "[MENU] Entering Options.\n"
                                    );

                                    fflush(stdout);

                                    screen_state =
                                        SCREEN_OPTIONS;

                                    screen_resources_dirty =
                                        1;

                                    state_transition_lock =
                                        15;
                                }
                            }


                            /*
                             * Triangle returns to title screen.
                             */
                            if (new_pad &
                                PAD_TRIANGLE) {

                                printf(
                                    "[UI] Returning to Press Start.\n"
                                );

                                fflush(stdout);

                                screen_state =
                                    SCREEN_PRESS_START;

                                screen_resources_dirty =
                                    1;

                                press_start_timer =
                                    0;

                                state_transition_lock =
                                    15;
                            }
                        }
                    }
                }

            } else {

                /*
                 * Prevent a stale held-button state.
                 */
                old_pad = 0;
            }
        }

        /* Music pumping happens only in menu frames, never inside the episode
         * playback loop. It is bounded and non-blocking. */
        watchon_music_pump();

        /* V3.4: final foreground overlay on EVERY ordinary menu screen.
         * The timebase is independent of Press Start and survives navigation.
         * The separate episode renderer is intentionally never modified.
         */
        /* V3.6.1: draw TRAX last only on ordinary UI screens.
         * Credits freezes the demo animation; episode playback uses
         * its existing separate renderer and remains unchanged.
         */
        if (screen_state != SCREEN_CREDITS && watchon_music_enabled) {
            if (trax_v2_delay_remaining > 0)
                trax_v2_delay_remaining--;
            else
                trax_v2_draw_title(gsGlobal,
                    screen_state != SCREEN_PRESS_START);
        }

        gsKit_queue_exec(gsGlobal);
        gsKit_sync_flip(gsGlobal);
    }

    return 0;
}

void play_video_file(
    GSGLOBAL *gsGlobal,
    const char *audio_filename,
    const char *folder_name)
{
    (void)audio_filename;

    /*
     * Assume playback did NOT complete unless the normal
     * end-of-video + end-of-audio condition is reached.
     */
    playback_completed_naturally = 0;

    /*
     * Bunny Maloney PS2 streaming format
     *
     * VIDEO:
     *   256 x 180
     *   16-bit BGR555
     *   24 FPS
     *
     * AUDIO:
     *   signed 16-bit little-endian PCM
     *   22050 Hz
     *   stereo
     */

    const int VIDEO_WIDTH  = 256;
    const int VIDEO_HEIGHT = 180;
    const int VIDEO_FPS    = 24;

    /*
     * ---------------------------------------------------------
     * AUDSRV AUDIO/VIDEO SYNC
     * ---------------------------------------------------------
     *
     * audsrv introduces a short delay between PCM submission
     * and the sound actually becoming audible.
     *
     * Let audio begin first, hold video frame 1, then begin
     * advancing the video clock after this amount of time.
     *
     * TEST VALUE:
     *     200 ms = 0.200 seconds
     */
    const int AUDIO_PREROLL_MS = 200;

    /*
     * TEMPORARY PLAY ALL DEBUG:
     *
     * Press L3 during an episode to jump close to the end.
     */
    const int DEBUG_SKIP_SECONDS = 10;

    const int FRAME_BYTES =
        VIDEO_WIDTH * VIDEO_HEIGHT * 2;

    char video_path[512];
    char audio_path[512];

    snprintf(
        video_path,
        sizeof(video_path),
        "host:Video/%s.raw",
        folder_name
    );

    snprintf(
        audio_path,
        sizeof(audio_path),
        "host:Audio/%s.pcm",
        folder_name
    );

    printf("[STREAM] Video: %s\n", video_path);
    printf("[STREAM] Audio: %s\n", audio_path);
    fflush(stdout);

    FILE *video = fopen(video_path, "rb");

    if (!video) {
        printf("[STREAM ERROR] Could not open video.\n");
        fflush(stdout);
        return;
    }

    FILE *audio = fopen(audio_path, "rb");

    if (!audio) {
        printf("[STREAM ERROR] Could not open audio.\n");
        fclose(video);
        fflush(stdout);
        return;
    }

    /*
     * Determine how many complete raw frames exist.
     *
     * 256x180x2 gives 92,160 bytes per frame.
     */
    fseek(video, 0, SEEK_END);

    long video_size = ftell(video);

    fseek(video, 0, SEEK_SET);

    if (video_size <= 0) {
        printf("[STREAM ERROR] Invalid video size.\n");

        fclose(video);
        fclose(audio);

        return;
    }

    int total_frames =
        (int)(video_size / FRAME_BYTES);

    printf(
        "[STREAM] Video size: %ld bytes\n"
        "[STREAM] Frames: %d\n"
        "[STREAM] Expected duration: %.2f seconds\n",
        video_size,
        total_frames,
        (float)total_frames / (float)VIDEO_FPS
    );

    fflush(stdout);

    /*
     * The frame buffer exists ONCE.
     *
     * No per-frame malloc.
     * No BMP decoder.
     * No thousands of individual files.
     */
    unsigned char *frame_buffer =
        memalign(128, FRAME_BYTES);

    if (!frame_buffer) {
        printf("[STREAM ERROR] Frame RAM allocation failed.\n");

        fclose(video);
        fclose(audio);

        return;
    }

    /*
     * Clear menu VRAM and create one reusable video texture.
     */
    gsKit_vram_clear(gsGlobal);

    GSTEXTURE frame_tex;

    memset(&frame_tex, 0, sizeof(frame_tex));

    frame_tex.Width   = VIDEO_WIDTH;
    frame_tex.Height  = VIDEO_HEIGHT;
    frame_tex.PSM     = GS_PSM_CT16;
    frame_tex.Mem     = (u32 *)frame_buffer;
    frame_tex.Delayed = 1;

    frame_tex.Vram =
        gsKit_vram_alloc(
            gsGlobal,
            gsKit_texture_size(
                VIDEO_WIDTH,
                VIDEO_HEIGHT,
                GS_PSM_CT16
            ),
            GSKIT_ALLOC_USERBUFFER
        );

    if (frame_tex.Vram == GSKIT_ALLOC_ERROR) {
        printf("[STREAM ERROR] Video VRAM allocation failed.\n");

        free(frame_buffer);
        fclose(video);
        fclose(audio);

        return;
    }

    /*
     * Load frame 1 BEFORE starting the audio clock.
     *
     * This avoids having audio already running while the first image
     * is still being fetched.
     */
    size_t first_read =
        fread(
            frame_buffer,
            1,
            FRAME_BYTES,
            video
        );

    if (first_read != FRAME_BYTES) {
        printf("[STREAM ERROR] Could not read first frame.\n");

        free(frame_buffer);
        fclose(video);
        fclose(audio);

        return;
    }

    gsKit_texture_upload(
        gsGlobal,
        &frame_tex
    );

    int last_frame_num = 1;

    /*
     * The video file is now positioned at frame 2.
     */
    int next_file_frame = 2;

    /*
     * Normally playback begins from frame 1.
     *
     * L3 debugging changes this so the normal timer can restart
     * from a frame close to the end of the episode.
     */
    int playback_base_frame = 0;

    /*
     * TEMP DEBUG:
     * L3 may only seek once during each episode.
     */
    int debug_skip_used = 0;

    /*
     * Audio setup.
     */
    if (!audsrv_initialized)
        init_audio_subsystem();

    if (audsrv_initialized) {

        struct audsrv_fmt_t format;

        format.bits     = 16;
        format.freq     = 22050;
        format.channels = 2;

        audsrv_set_format(&format);
        audsrv_set_volume(MAX_VOLUME);
    }

    char audio_chunk[2048];

    int audio_eof = 0;
    int video_eof = 0;
    int video_playing = 1;

    /*
     * Once BOTH files reach EOF, hold the final frame briefly
     * so the last submitted PCM has time to finish.
     *
     * We deliberately do NOT depend on audsrv_queued() reaching
     * zero because its circular-buffer accounting does not behave
     * like a simple empty/not-empty queue.
     */
    const int NATURAL_END_GRACE_MS = 150;

    int natural_end_wait_started = 0;
    u64 natural_end_wait_start = 0;

    /*
     * Prevent the START press which launched the episode from
     * immediately cancelling it.
     */
    u32 old_video_pad = 0;

    if (pad_initialized &&
        padGetState(0, 0) == PAD_STATE_STABLE) {

        struct padButtonStatus initial_buttons;

        memset(
            &initial_buttons,
            0,
            sizeof(initial_buttons)
        );

        if (padRead(
                0,
                0,
                &initial_buttons) != 0) {

            old_video_pad =
                0xffff ^ initial_buttons.btns;
        }
    }

    gsGlobal->PrimAlphaEnable =
        GS_SETTING_OFF;

    /*
     * Display frame 1 immediately.
     */
    gsKit_clear(
        gsGlobal,
        GS_SETREG_RGBAQ(
            0x00,
            0x00,
            0x00,
            0x80,
            0x00
        )
    );

    gsKit_prim_sprite_texture(
        gsGlobal,
        &frame_tex,

        0.0f,
        0.0f,

        0.0f,
        0.0f,

        640.0f,
        448.0f,

        VIDEO_WIDTH,
        VIDEO_HEIGHT,

        1,

        GS_SETREG_RGBAQ(
            0x80,
            0x80,
            0x80,
            0x80,
            0x00
        )
    );

    gsKit_queue_exec(gsGlobal);
    gsKit_sync_flip(gsGlobal);

    /*
     * Audio begins first.
     *
     * The video clock deliberately starts AUDIO_PREROLL_MS later
     * to compensate for audsrv output latency.
     */
    u64 audio_preroll_start_time = 0;
    u64 video_start_time = 0;

    int audio_preroll_started = 0;
    int clock_started = 0;

    printf(
        "[STREAM] Starting raw 24 FPS playback...\n"
    );

    fflush(stdout);

    while (video_playing) {

        /*
         * ---------------------------------------------------------
         * AUDIO
         * ---------------------------------------------------------
         *
         * Keep topping up audsrv WITHOUT blocking.
         *
         * We use whatever room is currently available.
         */
        if (audsrv_initialized &&
            !audio_eof) {

            int pump;

            for (pump = 0; pump < 8; pump++) {

                int available =
                    audsrv_available();

                if (available < 256)
                    break;

                int wanted = available;

                if (wanted >
                    (int)sizeof(audio_chunk)) {

                    wanted =
                        sizeof(audio_chunk);
                }

                /*
                 * 16-bit stereo = four-byte sample frames.
                 */
                wanted &= ~3;

                if (wanted <= 0)
                    break;

                int got =
                    fread(
                        audio_chunk,
                        1,
                        wanted,
                        audio
                    );

                if (got <= 0) {
                    audio_eof = 1;

                    printf(
                        "[AUDIO] PCM file finished.\n"
                    );

                    fflush(stdout);

                    break;
                }

                /*
                 * Submit PCM first.
                 *
                 * The VIDEO clock is intentionally NOT started here.
                 */
                int sent =
                    audsrv_play_audio(
                        audio_chunk,
                        got
                    );

                if (sent < 0) {
                    printf(
                        "[AUDIO ERROR] audsrv: %d\n",
                        sent
                    );

                    audio_eof = 1;
                    break;
                }


                /*
                 * The first successfully accepted PCM block marks
                 * the beginning of the audsrv preroll.
                 */
                if (
                    sent > 0 &&
                    !audio_preroll_started
                ) {

                    audio_preroll_start_time =
                        GetTimerSystemTime();

                    audio_preroll_started =
                        1;

                    printf(
                        "[AUDIO SYNC] audsrv started. "
                        "Holding video for %d ms.\n",
                        AUDIO_PREROLL_MS
                    );

                    fflush(stdout);
                }


                /*
                 * Normally sent == got because we checked
                 * audsrv_available() first.
                 */
                if (sent < got) {

                    long rewind_bytes =
                        (long)(sent - got);

                    fseek(
                        audio,
                        rewind_bytes,
                        SEEK_CUR
                    );

                    break;
                }
            }
        }

        /*
         * ---------------------------------------------------------
         * AUDSRV PREROLL / VIDEO CLOCK START
         * ---------------------------------------------------------
         */

        u64 now =
            GetTimerSystemTime();


        if (!clock_started) {

            /*
             * Normal case:
             *
             * PCM has been accepted by audsrv. Keep feeding audsrv
             * normally, but hold the video on frame 1 until the
             * compensation time has elapsed.
             */
            if (audio_preroll_started) {

                u64 preroll_elapsed =
                    now -
                    audio_preroll_start_time;

                u64 preroll_target =
                    (
                        (u64)kBUSCLK *
                        (u64)AUDIO_PREROLL_MS
                    ) /
                    1000ULL;


                if (
                    preroll_elapsed >=
                    preroll_target
                ) {

                    video_start_time =
                        now;

                    clock_started =
                        1;

                    printf(
                        "[AUDIO SYNC] Preroll complete. "
                        "Video clock started.\n"
                    );

                    fflush(stdout);
                }

            } else if (
                !audsrv_initialized ||
                audio_eof
            ) {

                /*
                 * Safety fallback:
                 *
                 * If audio is unavailable, don't leave the video
                 * permanently frozen on frame 1.
                 */
                video_start_time =
                    now;

                clock_started =
                    1;
            }
        }


        /*
         * ---------------------------------------------------------
         * VIDEO CLOCK
         * ---------------------------------------------------------
         *
         * While preroll is active elapsed stays zero, therefore
         * target_frame stays at frame 1.
         */

        u64 elapsed =
            0;

        if (clock_started) {

            elapsed =
                now -
                video_start_time;
        }

        int target_frame =
            playback_base_frame +
            (int)(
                (elapsed *
                 (u64)VIDEO_FPS)
                /
                kBUSCLK
            ) + 1;

        if (target_frame >
            total_frames) {

            target_frame =
                total_frames;

            if (!video_eof) {

                video_eof = 1;

                printf(
                    "[VIDEO] RAW stream finished.\n"
                );

                fflush(stdout);
            }
        }

        /*
         * Only move FORWARD.
         *
         * If we're behind, skip stale frames rather than slowing
         * the whole episode down.
         */
        if (!video_eof &&
            target_frame >
            last_frame_num) {

            /*
             * Raw frames are fixed-size, so skipping a late frame
             * is simply one seek operation.
             */
            if (target_frame >
                next_file_frame) {

                int frames_to_skip =
                    target_frame -
                    next_file_frame;

                long bytes_to_skip =
                    (long)frames_to_skip *
                    (long)FRAME_BYTES;

                if (fseek(
                        video,
                        bytes_to_skip,
                        SEEK_CUR) != 0) {

                    printf(
                        "[VIDEO ERROR] Seek failed.\n"
                    );

                    video_eof = 1;
                }

                next_file_frame =
                    target_frame;
            }

            if (!video_eof) {

                size_t got_frame =
                    fread(
                        frame_buffer,
                        1,
                        FRAME_BYTES,
                        video
                    );

                if (got_frame ==
                    FRAME_BYTES) {

                    /*
                     * Upload raw pixels directly.
                     *
                     * NO:
                     *  - BMP header
                     *  - BMP decoder
                     *  - malloc
                     *  - filename construction
                     *  - file reopen
                     */
                    gsKit_texture_upload(
                        gsGlobal,
                        &frame_tex
                    );

                    last_frame_num =
                        target_frame;

                    next_file_frame =
                        target_frame + 1;

                } else {

                    video_eof = 1;

                    printf(
                        "[VIDEO] End of raw stream.\n"
                    );

                    fflush(stdout);
                }
            }
        }

        /*
         * Draw the most recently uploaded texture.
         */
        gsKit_clear(
            gsGlobal,
            GS_SETREG_RGBAQ(
                0x00,
                0x00,
                0x00,
                0x80,
                0x00
            )
        );

        gsKit_prim_sprite_texture(
            gsGlobal,
            &frame_tex,

            0.0f,
            0.0f,

            0.0f,
            0.0f,

            640.0f,
            448.0f,

            VIDEO_WIDTH,
            VIDEO_HEIGHT,

            1,

            GS_SETREG_RGBAQ(
                0x80,
                0x80,
                0x80,
                0x80,
                0x00
            )
        );

        /*
         * Controller.
         */
        if (pad_initialized &&
            padGetState(0, 0) ==
                PAD_STATE_STABLE) {

            struct padButtonStatus buttons;

            if (padRead(
                    0,
                    0,
                    &buttons) != 0) {

                u32 paddata =
                    0xffff ^ buttons.btns;

                u32 new_pad =
                    paddata &
                    ~old_video_pad;

                old_video_pad =
                    paddata;

                /*
                 * =====================================================
                 * TEMPORARY L3 DEBUG SKIP
                 * =====================================================
                 *
                 * Jump to DEBUG_SKIP_SECONDS before the end.
                 *
                 * Video and audio are both repositioned and the
                 * normal 200 ms audio preroll is restarted.
                 */
                if (
                    option_l3_skip_enabled &&
                    (new_pad & PAD_L3) &&
                    !debug_skip_used
                ) {

                    debug_skip_used = 1;

                    int debug_skip_frames =
                        DEBUG_SKIP_SECONDS *
                        VIDEO_FPS;

                    int debug_target_frame =
                        total_frames -
                        debug_skip_frames;


                    if (debug_target_frame < 1)
                        debug_target_frame = 1;


                    long debug_video_offset =
                        (long)(
                            debug_target_frame - 1
                        ) *
                        (long)FRAME_BYTES;


                    /*
                     * PCM:
                     *
                     * 22050 Hz
                     * 16-bit
                     * stereo
                     *
                     * = 88200 bytes/sec
                     */
                    u64 debug_audio_offset64 =
                        (
                            (u64)(
                                debug_target_frame - 1
                            ) *
                            22050ULL *
                            2ULL *
                            2ULL
                        ) /
                        (u64)VIDEO_FPS;


                    /*
                     * Stereo 16-bit PCM must remain aligned to
                     * four-byte sample frames.
                     */
                    debug_audio_offset64 &=
                        ~3ULL;


                    long debug_audio_offset =
                        (long)debug_audio_offset64;


                    printf(
                        "[DEBUG] L3 pressed. "
                        "Jumping to %.2f seconds before end.\n",
                        (float)DEBUG_SKIP_SECONDS
                    );

                    printf(
                        "[DEBUG] Target frame: %d / %d\n",
                        debug_target_frame,
                        total_frames
                    );

                    fflush(stdout);


                    /*
                     * Remove the old audio stream.
                     *
                     * audsrv_stop_audio() alone does NOT reset the
                     * ring-buffer read/write positions.
                     *
                     * Re-applying the same format resets that buffer
                     * so the seeked PCM can begin cleanly.
                     */
                    if (audsrv_initialized) {

                        audsrv_stop_audio();

                        struct audsrv_fmt_t debug_format;

                        debug_format.bits     = 16;
                        debug_format.freq     = 22050;
                        debug_format.channels = 2;

                        int debug_format_result =
                            audsrv_set_format(
                                &debug_format
                            );

                        if (debug_format_result < 0) {

                            printf(
                                "[DEBUG ERROR] "
                                "audsrv format reset failed: %d\n",
                                debug_format_result
                            );

                            fflush(stdout);

                        } else {

                            audsrv_set_volume(
                                MAX_VOLUME
                            );

                            printf(
                                "[DEBUG] audsrv ring buffer reset. "
                                "Available=%d Queued=%d\n",
                                audsrv_available(),
                                audsrv_queued()
                            );

                            fflush(stdout);
                        }
                    }


                    /*
                     * Reposition video.
                     */
                    if (
                        fseek(
                            video,
                            debug_video_offset,
                            SEEK_SET
                        ) != 0
                    ) {

                        printf(
                            "[DEBUG ERROR] Video seek failed.\n"
                        );

                        fflush(stdout);

                    } else if (
                        fseek(
                            audio,
                            debug_audio_offset,
                            SEEK_SET
                        ) != 0
                    ) {

                        printf(
                            "[DEBUG ERROR] Audio seek failed.\n"
                        );

                        fflush(stdout);

                    } else {

                        /*
                         * Load the target video frame immediately.
                         */
                        size_t debug_read =
                            fread(
                                frame_buffer,
                                1,
                                FRAME_BYTES,
                                video
                            );


                        if (
                            debug_read ==
                            FRAME_BYTES
                        ) {

                            gsKit_texture_upload(
                                gsGlobal,
                                &frame_tex
                            );


                            last_frame_num =
                                debug_target_frame;

                            next_file_frame =
                                debug_target_frame + 1;


                            /*
                             * target_frame calculation now starts
                             * from this new position.
                             */
                            playback_base_frame =
                                debug_target_frame - 1;


                            /*
                             * The files are no longer at EOF.
                             */
                            video_eof = 0;
                            audio_eof = 0;

                            natural_end_wait_started = 0;
                            natural_end_wait_start = 0;


                            /*
                             * Restart the normal audio/video sync
                             * sequence from this point.
                             */
                            audio_preroll_start_time = 0;
                            video_start_time = 0;

                            audio_preroll_started = 0;
                            clock_started = 0;


                            /*
                             * Play All must still wait for the real
                             * natural ending.
                             */
                            playback_completed_naturally = 0;


                            printf(
                                "[DEBUG] Seek complete. "
                                "Restarting with normal %d ms preroll.\n",
                                AUDIO_PREROLL_MS
                            );

                            if (audsrv_initialized) {

                                printf(
                                    "[DEBUG] After seek: "
                                    "Available=%d Queued=%d\n",
                                    audsrv_available(),
                                    audsrv_queued()
                                );
                            }

                            fflush(stdout);

                        } else {

                            printf(
                                "[DEBUG ERROR] "
                                "Could not read target frame.\n"
                            );

                            fflush(stdout);
                        }
                    }
                }


                if ((new_pad &
                     PAD_TRIANGLE) ||
                    (new_pad &
                     PAD_START)) {

                    printf(
                        "[STREAM] Playback cancelled.\n"
                    );

                    fflush(stdout);

                    video_playing = 0;
                }
            }
        }

        gsKit_queue_exec(gsGlobal);
        gsKit_sync_flip(gsGlobal);

        /*
         * =========================================================
         * NATURAL EPISODE END
         * =========================================================
         *
         * Both source files reaching EOF is the authoritative end
         * condition.
         *
         * Give the last submitted PCM a short grace period before
         * returning to Play All.
         */
        if (
            video_playing &&
            video_eof &&
            audio_eof
        ) {

            if (!natural_end_wait_started) {

                natural_end_wait_started = 1;

                natural_end_wait_start =
                    GetTimerSystemTime();

                printf(
                    "[STREAM] Both streams reached EOF. "
                    "Finishing final audio...\n"
                );

                fflush(stdout);
            }


            u64 natural_end_elapsed =
                GetTimerSystemTime() -
                natural_end_wait_start;


            u64 natural_end_target =
                (
                    (u64)kBUSCLK *
                    (u64)NATURAL_END_GRACE_MS
                ) /
                1000ULL;


            if (
                natural_end_elapsed >=
                natural_end_target
            ) {

                playback_completed_naturally = 1;

                printf(
                    "[STREAM] NATURAL END CONFIRMED.\n"
                );

                fflush(stdout);

                video_playing = 0;
            }
        }
    }

    if (audsrv_initialized)
        audsrv_stop_audio();

    fclose(video);
    fclose(audio);

    free(frame_buffer);

    gsGlobal->PrimAlphaEnable =
        GS_SETTING_ON;

    gsKit_vram_clear(gsGlobal);

    printf(
        "[STREAM] Playback finished.\n"
    );

    fflush(stdout);
}

