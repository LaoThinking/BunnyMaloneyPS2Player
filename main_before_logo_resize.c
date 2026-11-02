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
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <libpad.h>
#include <audsrv.h>
#include <timer.h>

#define MAX_VOLUME 100

#ifndef PAD_TRIANGLE
#define PAD_TRIANGLE 0x1000
#endif
#ifndef PAD_START
#define PAD_START    0x0008
#endif
#ifndef PAD_STATE_READY
#define PAD_STATE_READY 0x06
#endif

void get_filepath(char *dest, const char *filename) {
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

    return 1;
}

void play_video_file(GSGLOBAL *gsGlobal, const char *audio_filename, const char *folder_name);

static int audsrv_initialized = 0;

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

static GSTEXTURE test_bar_tex, loading_text_tex, disclaimer_text_tex, logo_tex, press_start_tex;
static GSTEXTURE play_norm_tex, play_sel_tex, ep_norm_tex, ep_sel_tex, cred_norm_tex, cred_sel_tex;
static GSTEXTURE credits_bg_tex, thumb_ep1, thumb_ep2, thumb_ep3, button_norm_tex, button_sel_tex;

int load_all_menu_textures(GSGLOBAL *gsGlobal) {
    printf("[STEP] Clearing menu texture VRAM...\n");
    fflush(stdout);

    gsKit_vram_clear(gsGlobal);

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


static void show_boot_screens(
    GSGLOBAL *gsGlobal)
{
    GSTEXTURE loading_text;
    GSTEXTURE test_bar;
    GSTEXTURE disclaimer;

    int loading_ok;
    int bar_ok;
    int disclaimer_ok;

    int i;

    const int BLACK_DELAY_FRAMES = 60;

    /*
     * 11 seconds of actual loading animation.
     *
     * 1 second black +
     * 11 second loading =
     * bar reaches 100% at about 12 seconds.
     */
    const int LOADING_FRAMES = 660;

    const int LOADING_FADE_FRAMES = 45;

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
    const float BAR_SCALE_Y = 0.45f;
    const float BAR_Y       = 380.0f;
    const float BAR_BORDER  = 2.0f;


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
     * Completely black transition before disclaimer.
     */

    for (i = 0; i < 4; i++) {

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
     * DISCLAIMER
     * ============================================================
     */

    gsKit_vram_clear(gsGlobal);


    disclaimer_ok =
        boot_load_texture(
            gsGlobal,
            &disclaimer,
            "disclaimer_text.bmp"
        );


    float disclaimer_scale =
        0.50f;


    if (disclaimer_ok) {

        float w =
            disclaimer.Width *
            disclaimer_scale;

        float h =
            disclaimer.Height *
            disclaimer_scale;


        if (w > 430.0f) {

            disclaimer_scale *=
                430.0f / w;
        }


        w =
            disclaimer.Width *
            disclaimer_scale;

        h =
            disclaimer.Height *
            disclaimer_scale;


        if (h > 180.0f) {

            disclaimer_scale *=
                180.0f / h;
        }
    }


    /*
     * Fade disclaimer in.
     */

    for (i = 0;
         i < DISCLAIMER_FADE_IN;
         i++) {

        float fade =
            (float)i /
            (float)(
                DISCLAIMER_FADE_IN - 1
            );

        int shade =
            (int)(
                128.0f *
                fade
            );


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


        if (disclaimer_ok) {

            float w =
                disclaimer.Width *
                disclaimer_scale;

            float h =
                disclaimer.Height *
                disclaimer_scale;

            float x =
                (640.0f - w) /
                2.0f;

            float y =
                (448.0f - h) /
                2.0f;


            gsKit_prim_sprite_texture(
                gsGlobal,
                &disclaimer,

                x,
                y,

                0.0f,
                0.0f,

                x + w,
                y + h,

                disclaimer.Width,
                disclaimer.Height,

                1,

                GS_SETREG_RGBAQ(
                    shade,
                    shade,
                    shade,
                    0x80,
                    0
                )
            );
        }


        gsKit_queue_exec(gsGlobal);
        gsKit_sync_flip(gsGlobal);
    }


    /*
     * Hold disclaimer.
     */

    for (i = 0;
         i < DISCLAIMER_HOLD;
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


        if (disclaimer_ok) {

            float w =
                disclaimer.Width *
                disclaimer_scale;

            float h =
                disclaimer.Height *
                disclaimer_scale;

            float x =
                (640.0f - w) /
                2.0f;

            float y =
                (448.0f - h) /
                2.0f;


            gsKit_prim_sprite_texture(
                gsGlobal,
                &disclaimer,

                x,
                y,

                0.0f,
                0.0f,

                x + w,
                y + h,

                disclaimer.Width,
                disclaimer.Height,

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


        gsKit_queue_exec(gsGlobal);
        gsKit_sync_flip(gsGlobal);
    }


    /*
     * Fade disclaimer OUT completely.
     */

    for (i = 0;
         i < DISCLAIMER_FADE_OUT;
         i++) {

        float fade =
            1.0f -
            (
                (float)i /
                (float)(
                    DISCLAIMER_FADE_OUT - 1
                )
            );

        int shade =
            (int)(
                128.0f *
                fade
            );


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


        if (disclaimer_ok) {

            float w =
                disclaimer.Width *
                disclaimer_scale;

            float h =
                disclaimer.Height *
                disclaimer_scale;

            float x =
                (640.0f - w) /
                2.0f;

            float y =
                (448.0f - h) /
                2.0f;


            gsKit_prim_sprite_texture(
                gsGlobal,
                &disclaimer,

                x,
                y,

                0.0f,
                0.0f,

                x + w,
                y + h,

                disclaimer.Width,
                disclaimer.Height,

                1,

                GS_SETREG_RGBAQ(
                    shade,
                    shade,
                    shade,
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

    for (i = 0;
         i < 8;
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


    printf(
        "[BOOT] Boot presentation finished.\n"
    );

    fflush(stdout);
}




/* ===== END BUNNY MALONEY BOOT UI ===== */

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
    gsGlobal->Field = GS_FIELD_ODD;
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

    printf("[STEP 10] Initializing Pad...\n");
    fflush(stdout);
    init_pad_subsystem();


    /*
     * Bunny Maloney boot presentation:
     * Loading -> Disclaimer -> existing Press Start screen.
     *
     * IMPORTANT:
     * This happens before the persistent menu textures are loaded.
     */
    show_boot_screens(gsGlobal);

    printf("[STEP 11] Loading Textures...\n");
    fflush(stdout);
    load_all_menu_textures(gsGlobal);

    int state_transition_lock = 0;
    u32 old_pad = 0;
    int pad_debug_counter = 0;

    gsGlobal->PrimAlphaEnable = GS_SETTING_ON;

    printf("[STEP 12] Entering Main Loop...\n");
    fflush(stdout);

    while (1) {
        if (state_transition_lock > 0) {
            state_transition_lock--;
        }

        gsKit_clear(gsGlobal, GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x80, 0x00));

        if (logo_tex.Vram != 0) {
            gsKit_prim_sprite_texture(gsGlobal, &logo_tex,
                50.0f, 30.0f,
                0.0f, 0.0f,
                590.0f, 180.0f,
                logo_tex.Width, logo_tex.Height,
                1, GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0x00));
        }

        if (press_start_tex.Vram != 0) {

            /*
             * Native 256x64 texture size,
             * horizontally centred.
             */

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
                    0x80,
                    0x80,
                    0x80,
                    0x80,
                    0x00
                )
            );
        }

        if (pad_initialized) {
            int pad_state = padGetState(0, 0);

            /*
             * Print the controller state about once every two seconds.
             * NTSC is approximately 60 fields/frames per second here.
             */
            if ((pad_debug_counter++ % 120) == 0) {
                char state_name[16] = {0};

                padStateInt2String(pad_state, state_name);

                printf("[PAD] State = %d (%s)\n",
                       pad_state,
                       state_name);
                fflush(stdout);
            }

            if (pad_state == PAD_STATE_STABLE) {
                struct padButtonStatus buttons;
                memset(&buttons, 0, sizeof(buttons));

                int pad_result = padRead(0, 0, &buttons);

                if (pad_result != 0) {
                    u32 paddata = 0xffff ^ buttons.btns;
                    u32 new_pad = paddata & ~old_pad;

                    old_pad = paddata;

                    if (new_pad != 0) {
                        printf("[PAD] Button mask = 0x%04X\n",
                               (unsigned int)new_pad);
                        fflush(stdout);
                    }

                    /*
                     * START normally launches the video.
                     * CROSS is also accepted for testing.
                     */
                    if (((new_pad & PAD_START) ||
                         (new_pad & PAD_CROSS)) &&
                        state_transition_lock == 0) {

                        printf("[DEBUG] START/X detected. Launching Episode 1...\n");
                        fflush(stdout);

                        play_video_file(gsGlobal,
                                        "episode1.wav",
                                        "episode1");

                        printf("[DEBUG] Returned from Episode 1 playback.\n");
                        fflush(stdout);

                        load_all_menu_textures(gsGlobal);

                        old_pad = 0;
                        state_transition_lock = 20;
                    }
                }
            } else {
                /*
                 * Reset this whenever the controller temporarily leaves
                 * STABLE state, otherwise an old button state can stick.
                 */
                old_pad = 0;
            }
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
     * The video clock begins when the first actual PCM is submitted.
     */
    u64 video_start_time = 0;
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
                 * Start the master clock immediately before the first
                 * audio block is submitted.
                 */
                if (!clock_started) {
                    video_start_time =
                        GetTimerSystemTime();

                    clock_started = 1;
                }

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
         * No audio means no master clock yet.
         */
        if (!clock_started) {

            video_start_time =
                GetTimerSystemTime();

            clock_started = 1;
        }

        /*
         * ---------------------------------------------------------
         * VIDEO CLOCK
         * ---------------------------------------------------------
         */

        u64 now =
            GetTimerSystemTime();

        u64 elapsed =
            now - video_start_time;

        int target_frame =
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

            video_eof = 1;
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
         * Exit only after both streams are finished.
         */
        if (video_eof &&
            audio_eof) {

            if (!audsrv_initialized ||
                audsrv_queued() <= 0) {

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

