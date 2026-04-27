/**
 * pixshift.c — Pixel Shift Resolution module for Magic Lantern
 * v1.3: API исправлен по реальным хедерам репозитория
 *
 * Исправлено относительно v1.2:
 *   - lens_info.lens_present → lens_info.lens_exists  (реальное поле)
 *   - FONT_MEDIUM            → FONT_MED               (реальная константа)
 *   - lens_focus_move()      → lens_focus()           (реальная функция)
 *   - take_a_pic(ML_SHOOT_WAIT_CARD) → take_a_pic(0)  (правильная сигнатура)
 *   - UNIT_MS                → UNIT_TIME_MS           (реальная константа)
 *   - console_printf()       → printf()               (нет такой функции)
 *   - void* task             → uint32_t task          (реальный тип возврата)
 *
 * Сигнатуры из хедеров:
 *   lens_focus(int num_steps, int stepsize, int wait, int extra_delay) → int
 *   take_a_pic(int should_af) → int
 *   lens_info.lens_exists
 */

#include <module.h>
#include <dryos.h>
#include <property.h>
#include <bmp.h>
#include <menu.h>
#include <shoot.h>
#include <lens.h>
#include <config.h>

/* =========================================================
 * Параметры модуля
 * ========================================================= */

static CONFIG_INT("pixshift.frames",  pixshift_frames,  1);
static CONFIG_INT("pixshift.delay",   pixshift_delay,   500);
static CONFIG_INT("pixshift.step",    pixshift_step,    2);
static CONFIG_INT("pixshift.mode",    pixshift_mode,    0);
static CONFIG_INT("pixshift.enabled", pixshift_enabled, 0);

static volatile int pixshift_running    = 0;
static volatile int pixshift_frame_num  = 0;
static volatile int pixshift_task_alive = 0;

static inline int pixshift_frames_count(void)
{
    return (pixshift_frames == 0) ? 2 : 4;
}

/* =========================================================
 * Фокус-мотор
 * lens_focus(num_steps, stepsize, wait, extra_delay)
 * ========================================================= */

static int pixshift_do_focus_step(int steps)
{
    if (!lens_info.lens_exists)
    {
        bmp_printf(FONT_MED, 50, 50, "No lens detected!");
        return 0;
    }
    return lens_focus(steps, 1, 1, 0);
}

static void pixshift_return_focus(int total_steps)
{
    if (total_steps != 0)
        lens_focus(-total_steps, 1, 1, 0);
}

static int pixshift_has_af_motor(void)
{
    if (!lens_info.lens_exists) return 0;
    int ok = lens_focus(1, 1, 1, 0);
    if (ok) lens_focus(-1, 1, 1, 0);
    return ok;
}

/* =========================================================
 * Съёмочный таск
 * ========================================================= */

static void pixshift_shoot_task(void* arg)
{
    pixshift_task_alive = 1;

    int n           = pixshift_frames_count();
    int step        = pixshift_step;
    int delay_ms    = pixshift_delay;
    int total_steps = 0;

    printf("PixShift: starting %d-frame sequence\n", n);

    int i;
    for (i = 0; i < n; i++)
    {
        if (!pixshift_running)
        {
            printf("PixShift: aborted at frame %d\n", i + 1);
            break;
        }

        pixshift_frame_num = i + 1;
        bmp_printf(FONT_MED | FONT_ALIGN_CENTER, 360, 400,
                   "PixShift %d/%d", i + 1, n);

        if (i > 0)
        {
            if (pixshift_mode == 0)
            {
                int ok = pixshift_do_focus_step(step);
                if (!ok)
                {
                    printf("PixShift: WARNING frame %d motor stalled\n", i + 1);
                    bmp_printf(FONT_MED | FONT_ALIGN_CENTER, 360, 420,
                               "WARNING: motor stalled!");
                }
                else
                {
                    total_steps += step;
                }
            }
            else
            {
                bmp_printf(FONT_MED | FONT_ALIGN_CENTER, 360, 420,
                           "Move camera, then HALF-SHUTTER");
                int waited = 0;
                while (!get_halfshutter_pressed() && waited < 30000 && pixshift_running)
                {
                    msleep(100);
                    waited += 100;
                }
                if (!pixshift_running) break;
            }

            msleep(delay_ms);
        }

        /* take_a_pic(should_af): 0 = без AF */
        int ret = take_a_pic(0);
        if (ret != 0)
        {
            printf("PixShift: take_a_pic failed (ret=%d)\n", ret);
            pixshift_running = 0;
            break;
        }

        /* Ждём окончания записи */
        while (lens_info.job_state) msleep(100);

        printf("PixShift: frame %d done\n", i + 1);
    }

    if (pixshift_mode == 0 && total_steps != 0)
        pixshift_return_focus(total_steps);

    bmp_printf(FONT_MED | FONT_ALIGN_CENTER, 360, 400,
               "PixShift done! (%d frames)", n);
    msleep(2000);

    pixshift_running    = 0;
    pixshift_frame_num  = 0;
    pixshift_task_alive = 0;
    printf("PixShift: complete\n");
}

/* =========================================================
 * Запуск
 * ========================================================= */

static void pixshift_start(void)
{
    if (pixshift_running) return;

    if (!pixshift_enabled)
    {
        bmp_printf(FONT_MED, 50, 50, "PixShift: enable first");
        return;
    }

    if (is_movie_mode())
    {
        bmp_printf(FONT_MED, 50, 50, "PixShift: photo mode only!");
        return;
    }

    if (pixshift_mode == 0 && !lens_info.lens_exists)
    {
        bmp_printf(FONT_MED, 50, 50, "PixShift: no lens!");
        return;
    }

    if (pixshift_mode == 0 && !pixshift_has_af_motor())
    {
        bmp_printf(FONT_MED, 50, 50, "No AF motor! Use manual mode.");
        return;
    }

    pixshift_running = 1;

    uint32_t task = task_create("pixshift_task", 0x1c, 0x4000,
                                pixshift_shoot_task, NULL);
    if (!task)
    {
        bmp_printf(FONT_MED, 50, 50, "task_create failed!");
        pixshift_running = 0;
    }
}

/* =========================================================
 * Menu
 * ========================================================= */

static MENU_UPDATE_FUNC(pixshift_update)
{
    if (pixshift_running)
        MENU_SET_WARNING(MENU_WARN_INFO, "Shooting %d/%d...",
                         pixshift_frame_num, pixshift_frames_count());

    if (!pixshift_enabled)
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Module disabled");

    if (pixshift_mode == 0 && !lens_info.lens_exists)
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "No lens detected");
}

static MENU_SELECT_FUNC(pixshift_toggle)
{
    pixshift_enabled = !pixshift_enabled;
}

static MENU_SELECT_FUNC(pixshift_shoot_now)
{
    if (!pixshift_running)
        pixshift_start();
}

static struct menu_entry pixshift_menu[] = {
    {
        .name     = "Pixel Shift",
        .priv     = &pixshift_enabled,
        .max      = 1,
        .update   = pixshift_update,
        .select   = pixshift_toggle,
        .help     = "Multi-frame pixel shift for increased resolution.",
        .help2    = "Shoot series with sub-pixel lens motor displacement.",
        .children = (struct menu_entry[]) {
            {
                .name   = "Shoot Now",
                .select = pixshift_shoot_now,
                .help   = "Start pixel shift sequence immediately.",
            },
            {
                .name    = "Frames",
                .priv    = &pixshift_frames,
                .max     = 1,
                .choices = CHOICES("2 frames", "4 frames"),
                .help    = "2 frames = x2 res, 4 frames = x4 res.",
            },
            {
                .name    = "Shift Mode",
                .priv    = &pixshift_mode,
                .max     = 1,
                .choices = CHOICES("Auto (lens motor)", "Manual (tripod)"),
                .help    = "Auto: EF lens motor. Manual: half-shutter between shots.",
            },
            {
                .name  = "Motor Step",
                .priv  = &pixshift_step,
                .min   = 1,
                .max   = 10,
                .unit  = UNIT_DEC,
                .help  = "Focus motor steps per shift. Start with 2.",
            },
            {
                .name  = "Stabiliz. Delay",
                .priv  = &pixshift_delay,
                .min   = 100,
                .max   = 2000,
                .unit  = UNIT_TIME_MS,
                .help  = "Wait after shift before shooting (ms).",
            },
            MENU_EOL,
        },
    },
};

/* =========================================================
 * Lifecycle
 * ========================================================= */

static unsigned int pixshift_init(void)
{
    menu_add("Shoot", pixshift_menu, COUNT(pixshift_menu));
    printf("PixShift module loaded\n");
    return 0;
}

static unsigned int pixshift_deinit(void)
{
    pixshift_running = 0;
    int timeout = 5000;
    while (pixshift_task_alive && timeout > 0)
    {
        msleep(100);
        timeout -= 100;
    }
    menu_remove("Shoot", pixshift_menu, COUNT(pixshift_menu));
    return 0;
}

MODULE_INFO_START()
    MODULE_INIT(pixshift_init)
    MODULE_DEINIT(pixshift_deinit)
MODULE_INFO_END()

MODULE_CONFIGS_START()
    MODULE_CONFIG(pixshift_enabled)
    MODULE_CONFIG(pixshift_frames)
    MODULE_CONFIG(pixshift_delay)
    MODULE_CONFIG(pixshift_step)
    MODULE_CONFIG(pixshift_mode)
MODULE_CONFIGS_END()

MODULE_CBRS_START()
MODULE_CBRS_END()

MODULE_PROPINFO_START()
MODULE_PROPINFO_END()
