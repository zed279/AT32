#include "at32f403a_407_clock.h"
#include "at32f403a_407_board.h"
#include "sgl.h"
#include "sgl_port_at32.h"

int main(void)
{
    system_clock_config();
    at32_board_init();
    uart_print_init(115200);

    /* 初始化 SGL + LCD */
    sgl_port_init();

    /* ===== UI 界面 ===== */
    sgl_obj_t *screen = sgl_screen_act();

    /* 标题 */
    sgl_obj_t *title = sgl_label_create(screen);
    sgl_obj_set_pos(title, 10, 5);
    sgl_obj_set_size(title, 300, 20);
    sgl_label_set_text(title, "AT32F403A");
    sgl_label_set_font(title, &consolas14);
    sgl_label_set_text_color(title, sgl_rgb(0, 0, 255));

    /* LED 状态 */
    sgl_obj_t *led = sgl_led_create(screen);
    sgl_obj_set_pos(led, 10, 35);
    sgl_obj_set_size(led, 16, 16);
    sgl_led_set_on_color(led, sgl_rgb(0, 255, 0));
    sgl_led_on(led);

    sgl_obj_t *led_label = sgl_label_create(screen);
    sgl_obj_set_pos(led_label, 32, 35);
    sgl_obj_set_size(led_label, 100, 18);
    sgl_label_set_text(led_label, "LED: ON");
    sgl_label_set_font(led_label, &consolas14);

    /* 亮度滑块 */
    sgl_obj_t *bright_label = sgl_label_create(screen);
    sgl_obj_set_pos(bright_label, 10, 62);
    sgl_obj_set_size(bright_label, 200, 18);
    sgl_label_set_text(bright_label, "Brightness: 50%");
    sgl_label_set_font(bright_label, &consolas14);

    sgl_obj_t *slider = sgl_slider_create(screen);
    sgl_obj_set_pos(slider, 10, 82);
    sgl_obj_set_size(slider, 300, 12);
    sgl_slider_set_value(slider, 50);

    /* 两个按钮 */
    sgl_obj_t *btn1 = sgl_button_create(screen);
    sgl_obj_set_pos(btn1, 10, 110);
    sgl_obj_set_size(btn1, 145, 36);
    sgl_button_set_text(btn1, "LED");
    sgl_button_set_font(btn1, &consolas14);
    sgl_button_set_radius(btn1, 5);

    sgl_obj_t *btn2 = sgl_button_create(screen);
    sgl_obj_set_pos(btn2, 165, 110);
    sgl_obj_set_size(btn2, 145, 36);
    sgl_button_set_text(btn2, "Mode");
    sgl_button_set_font(btn2, &consolas14);
    sgl_button_set_radius(btn2, 5);
    sgl_button_set_color(btn2, sgl_rgb(0, 150, 255));
    sgl_button_set_text_color(btn2, sgl_rgb(255, 255, 255));

    /* 底部状态 */
    sgl_obj_t *status = sgl_label_create(screen);
    sgl_obj_set_pos(status, 10, 218);
    sgl_obj_set_size(status, 300, 16);
    sgl_label_set_text(status, "SGL+ST7789+SPI1");
    sgl_label_set_font(status, &consolas14);
    sgl_label_set_text_color(status, sgl_rgb(128, 128, 128));

    /* 主循环 */
    while(1)
    {
        sgl_port_task_handler();
    }
}
