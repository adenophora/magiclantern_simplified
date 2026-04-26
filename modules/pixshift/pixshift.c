/**
 * pixshift.c — Pixel Shift Resolution module for Magic Lantern
 *
 * Снимает серию кадров (2 или 4) со смещением фокусировочного актуатора
 * между кадрами, имитируя механический pixel shift.
 *
 * Архитектура:
 *   - Модуль управляет серией через ML intervalometer API
 *   - Смещение между кадрами достигается коротким импульсом focus motor
 *     (sub-step режим, минимальная дистанция)
 *   - Все кадры пишутся как обычные RAW, merge — постпроцессинг на ПК
 *   - На 50D нет сенсорного актуатора → используем ленсовый мотор (MF)
 *
 * Ограничения 50D:
 *   - Нет IBIS, нет пьезо-актуатора → pixel shift через объектив
 *   - Только MF-объективы с моторизованным фокусом (EF) дадут предсказуемый сдвиг
 *   - С немоторизованными (мануальный фокус) — только штатив + ручной сдвиг
 *
 * Совместимость: универсальный код, проверен структурно для 50D.111
 * v1.1: исправлены баги — остановка таска, frames=3, enabled, task_create
 *
 * Чем руководствовался:
 *   1. Структура модуля взята из intervalometer/hdr_video — те же макросы MODULE_*
 *   2. API: take_a_pic(), lens_focus_move(), msleep(), GUI_Control()
 *   3. MENU_* макросы — стандарт для всех модулей с настройками
 *   4. На 50D нет CONFIG_DIGIC_V+ поэтому избегаем digic_poke
 *   5. Память: alloc/free через AllocateMemory (не malloc) — требование DryOS
 */

#include <module.h>
#include <dryos.h>
#include <property.h>
#include <bmp.h>
#include <menu.h>
#include <shoot.h>
#include <lens.h>
#include <config.h>
#include <console.h>

/* =========================================================
 * Параметры модуля (сохраняются в ML config)
 * ========================================================= */

/* Количество кадров в серии: 0=2 кадра, 1=4 кадра (индекс в CHOICES) */
static CONFIG_INT("pixshift.frames",   pixshift_frames,   1);

/* Задержка между кадрами (мс) — время на стабилизацию после сдвига */
static CONFIG_INT("pixshift.delay",    pixshift_delay,    500);

/* Величина сдвига фокуса (шаги актуатора, 1–10) */
static CONFIG_INT("pixshift.step",     pixshift_step,     2);

/* Режим сдвига: 0=фокус-мотор, 1=только штатив (напоминание, без авто-сдвига) */
static CONFIG_INT("pixshift.mode",     pixshift_mode,     0);

/* Включён ли модуль */
static CONFIG_INT("pixshift.enabled",  pixshift_enabled,  0);

/* Текущее состояние */
static volatile int pixshift_running    = 0;
static volatile int pixshift_frame_num  = 0;
static volatile int pixshift_task_alive = 0;  /* 1 пока таск исполняется */

/* Хелпер: реальное кол-во кадров из индекса CHOICES */
static inline int pixshift_frames_count(void)
{
    return (pixshift_frames == 0) ? 2 : 4;
}

/* =========================================================
 * Вспомогательные функции
 * ========================================================= */

/**
 * Выполнить микро-сдвиг через фокусный мотор объектива.
 * steps > 0  → бесконечность (дальше)
 * steps < 0  → ближе
 *
 * На 50D lens_mli_to_focus() работает только с EF-объективами
 * с поддержкой AF (мотор присутствует).
 * С мануальными стёклами функция вернёт 0 без эффекта.
 */
static int pixshift_do_focus_step(int steps)
{
    /* lens_present — правильное поле в ML lens_info (не lens_exists) */
    if (!lens_info.lens_present)
    {
        bmp_printf(FONT_MEDIUM, 50, 50, "No lens detected!");
        return 0;
    }

    /* lens_focus_move(dir, speed, steps):
     *   dir=1    → бесконечность
     *   dir=-1   → минимальная дистанция
     *   speed=1  → медленно (минимальный шаг, нужен для pixel shift)
     *   steps    → количество шагов актуатора
     * Возвращает количество реально пройденных шагов.
     *
     * Примечание: lens_mli_to_focus() отсутствует в DIGIC IV сборках (50D).
     * lens_focus_move() — актуальный публичный API для всех камер.
     */
    int dir   = (steps > 0) ? 1 : -1;
    int abs_s = (steps > 0) ? steps : -steps;

    return lens_focus_move(dir, 1, abs_s);
}

/**
 * Компенсирующий обратный сдвиг после серии.
 */
static void pixshift_return_focus(int total_steps_taken)
{
    if (total_steps_taken != 0)
        lens_focus_move((total_steps_taken > 0) ? -1 : 1,
                        1, ABS(total_steps_taken));
}

/**
 * Проверка наличия AF-мотора.
 * Делаем тестовый сдвиг на 1 шаг и немедленно возвращаем.
 * Если мотора нет (Samyang, советские адаптеры с чипом) — вернёт 0.
 */
static int pixshift_has_af_motor(void)
{
    if (!lens_info.lens_present) return 0;
    int moved = lens_focus_move(1, 1, 1);
    if (moved > 0)
        lens_focus_move(-1, 1, moved);  /* возврат */
    return moved > 0;
}

/* =========================================================
 * Основная логика серийной съёмки
 * ========================================================= */

/**
 * Съёмочный цикл. Запускается в отдельном таске через task_create.
 *
 * Алгоритм:
 *   1. Для каждого кадра (0..N-1):
 *      a. Если не первый — выполнить сдвиг
 *      b. Подождать стабилизацию (pixshift_delay мс)
 *      c. Сделать снимок (take_a_pic с ожиданием записи)
 *   2. Вернуть фокус в исходное положение
 *   3. Сбросить флаг running
 *
 * Почему task_create, а не прямой вызов:
 *   take_a_pic() блокирует поток до окончания экспозиции + записи,
 *   поэтому нельзя вызывать из GUI-таска — зависнет интерфейс.
 */
static void pixshift_shoot_task(void* arg)
{
    pixshift_task_alive = 1;  /* таск стартовал */
    int n        = pixshift_frames_count();
    int step     = pixshift_step;
    int delay_ms = pixshift_delay;
    int total_steps = 0;

    console_show();
    printf("PixShift: starting %d-frame sequence\n", n);

    for (int i = 0; i < n; i++)
    {
        /* Баг #1 fix: проверяем флаг в каждой итерации.
         * deinit сбрасывает pixshift_running=0, таск сам аккуратно выходит.
         */
        if (!pixshift_running)
        {
            printf("PixShift: aborted at frame %d\n", i + 1);
            break;
        }

        pixshift_frame_num = i + 1;

        bmp_printf(FONT_MEDIUM | FONT_ALIGN_CENTER, 360, 400,
                   "PixShift %d/%d", i + 1, n);

        if (i > 0)
        {
            if (pixshift_mode == 0)
            {
                /* Баг #3 fix: логируем если мотор не двинулся.
                 * Серию не прерываем — первый кадр уже записан,
                 * пусть пользователь сам решает что делать с серией.
                 */
                int moved = pixshift_do_focus_step(step);
                if (moved == 0)
                {
                    printf("PixShift: WARNING frame %d — focus motor returned 0 steps\n",
                           i + 1);
                    bmp_printf(FONT_MEDIUM | FONT_ALIGN_CENTER, 360, 420,
                               "WARNING: motor stalled!");
                }
                else
                {
                    total_steps += moved;
                    printf("PixShift: frame %d, focus stepped %d (total %d)\n",
                           i + 1, moved, total_steps);
                }
            }
            else
            {
                bmp_printf(FONT_MEDIUM | FONT_ALIGN_CENTER, 360, 420,
                           "Move camera, then HALF-SHUTTER");
                int waited = 0;
                /* Баг #1 fix: проверяем running и в этом цикле */
                while (!get_halfshutter_pressed() && waited < 30000
                       && pixshift_running)
                {
                    msleep(100);
                    waited += 100;
                }
                if (!pixshift_running) break;
            }

            msleep(delay_ms);
        }

        int ret = take_a_pic(ML_SHOOT_WAIT_CARD);
        if (ret != 0)
        {
            printf("PixShift: take_a_pic failed (ret=%d), aborting\n", ret);
            /* Баг #7 fix: сбрасываем флаг сразу при break, не ждём конца цикла */
            pixshift_running = 0;
            break;
        }

        printf("PixShift: frame %d done\n", i + 1);
    }

    /* Возврат фокуса в исходную позицию */
    if (pixshift_mode == 0 && total_steps != 0)
    {
        printf("PixShift: returning focus (%d steps)\n", -total_steps);
        pixshift_return_focus(total_steps);
    }

    bmp_printf(FONT_MEDIUM | FONT_ALIGN_CENTER, 360, 400,
               "PixShift done! (%d frames)", n);
    msleep(2000);

    pixshift_running    = 0;
    pixshift_frame_num  = 0;
    pixshift_task_alive = 0;  /* таск завершён — deinit может продолжать */
    printf("PixShift: sequence complete\n");
}

/**
 * Точка запуска серии.
 * Вызывается из menu callback или кнопки.
 */
static void pixshift_start(void)
{
    if (pixshift_running)
    {
        printf("PixShift: already running\n");
        return;
    }

    /* Баг #6 fix: уважаем флаг enabled */
    if (!pixshift_enabled)
    {
        bmp_printf(FONT_MEDIUM, 50, 50, "PixShift: enable module first");
        return;
    }

    if (is_movie_mode())
    {
        bmp_printf(FONT_MEDIUM, 50, 50, "PixShift: photo mode only!");
        return;
    }

    if (pixshift_mode == 0 && !lens_info.lens_present)
    {
        bmp_printf(FONT_MEDIUM, 50, 50,
                   "PixShift: no lens! Use manual mode.");
        return;
    }

    /* Проверка AF-мотора: тестовый сдвиг на 1 шаг.
     * Чипованные мануальники (Samyang и т.п.) вернут 0.
     * Добавляем 200ms задержку перед тестом — мотор должен быть готов.
     */
    if (pixshift_mode == 0)
    {
        msleep(200);
        if (!pixshift_has_af_motor())
        {
            bmp_printf(FONT_MEDIUM, 50, 50,
                       "PixShift: lens has no AF motor! Use manual mode.");
            return;
        }
    }

    pixshift_running = 1;

    /* Баг #9 fix: проверяем результат task_create.
     * В ML task_create возвращает указатель на task struct или NULL.
     */
    void* task = task_create("pixshift_task", 0x1c, 0x4000,
                              pixshift_shoot_task, NULL);
    if (!task)
    {
        bmp_printf(FONT_MEDIUM, 50, 50, "PixShift: task_create failed!");
        printf("PixShift: task_create returned NULL\n");
        pixshift_running = 0;
    }
}

/* =========================================================
 * Menu (GUI)
 * ========================================================= */

static MENU_UPDATE_FUNC(pixshift_update)
{
    if (pixshift_running)
        MENU_SET_WARNING(MENU_WARN_INFO,
                         "Shooting %d/%d...",
                         pixshift_frame_num, pixshift_frames_count());

    if (!pixshift_enabled)
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Module disabled");

    if (pixshift_mode == 0 && !lens_info.lens_present)
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING,
                         "No motorized lens detected");
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
        .name    = "Pixel Shift",
        .priv    = &pixshift_enabled,
        .max     = 1,
        .update  = pixshift_update,
        .select  = pixshift_toggle,
        .help    = "Multi-frame pixel shift for increased resolution.",
        .help2   = "Shoot series with sub-pixel lens motor displacement.",
        .children = (struct menu_entry[]) {
            {
                .name  = "Shoot Now",
                .priv  = NULL,
                .select = pixshift_shoot_now,
                .help  = "Start pixel shift sequence immediately.",
            },
            {
                /* Баг #5 fix: CHOICES вместо min/max — убираем возможность
                 * выбрать 3 кадра. Значение 0=2 кадра, 1=4 кадра.
                 */
                .name  = "Frames",
                .priv  = &pixshift_frames,
                .max   = 1,
                .choices = CHOICES("2 frames", "4 frames"),
                .help  = "Number of frames: 2 (x2 res) or 4 (x4 res).",
                .help2 = "More frames = better resolution, more motion risk.",
            },
            {
                .name  = "Shift Mode",
                .priv  = &pixshift_mode,
                .min   = 0,
                .max   = 1,
                .choices = CHOICES("Auto (lens motor)", "Manual (tripod)"),
                .help  = "Auto: uses EF lens focus motor for micro-shift.",
                .help2 = "Manual: waits for half-shutter between shots.",
            },
            {
                .name  = "Motor Step",
                .priv  = &pixshift_step,
                .min   = 1,
                .max   = 10,
                .unit  = UNIT_DEC,
                .help  = "Focus motor steps per shift (1=finest, 10=coarse).",
                .help2 = "Optimal: 1-3 steps. More = less overlap, more shift.",
            },
            {
                .name  = "Stabilization Delay",
                .priv  = &pixshift_delay,
                .min   = 100,
                .max   = 2000,
                .unit  = UNIT_MS,
                .help  = "Wait time after shift before shooting (ms).",
                .help2 = "Increase on tripod with weak head, decrease on rigid.",
            },
            MENU_EOL,
        },
    },
};

/* =========================================================
 * Lifecycle: init / deinit
 * ========================================================= */

static unsigned int pixshift_init(void)
{
    menu_add("Shoot", pixshift_menu, COUNT(pixshift_menu));
    console_printf("PixShift module loaded\n");
    return 0;
}

static unsigned int pixshift_deinit(void)
{
    /* Сигнал задаче на остановку */
    pixshift_running = 0;

    /* Ждём завершения таска перед выгрузкой модуля.
     * Без ожидания задача продолжит выполнение в уже выгруженной памяти → краш.
     *
     * pixshift_task_alive сбрасывается самой задачей в самом конце,
     * после всех операций с памятью и bmp_printf — безопасная точка.
     *
     * Таймаут 5с покрывает большинство выдержек. При bulb-съёмке
     * пользователь не должен выгружать модуль в середине серии.
     */
    int timeout = 5000;
    while (pixshift_task_alive && timeout > 0)
    {
        msleep(100);
        timeout -= 100;
    }

    menu_remove("Shoot", pixshift_menu, COUNT(pixshift_menu));
    return 0;
}

/* =========================================================
 * Module descriptor
 * ========================================================= */

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

MODULE_PARAMS_START()
MODULE_PARAMS_END()

MODULE_PROPINFO_START()
MODULE_PROPINFO_END()
