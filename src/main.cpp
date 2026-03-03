#include "MC_PULL_calibration.h"
#include "ws2812.h"

#include "Flash_saves.h"
#include "Motion_control.h"
#include "_bus_hardware.h"
#include "ams.h"
#include "ahub_bus.h"
#include "bambu_bus_ams.h"
#include "ADC_DMA.h"
#include "Debug_log.h"
#include <string.h>

WS2812_class SYS_RGB;
WS2812_class RGBOUT[4];

void RGB_init()
{
    SYS_RGB.init(1, GPIOD, GPIO_Pin_1);
    RGBOUT[0].init(2, GPIOA, GPIO_Pin_11);
    RGBOUT[1].init(2, GPIOA, GPIO_Pin_8);
    RGBOUT[2].init(2, GPIOB, GPIO_Pin_1);
    RGBOUT[3].init(2, GPIOB, GPIO_Pin_0);
}

void RGB_update()
{
    if (!(SYS_RGB.is_dirty() ||
          RGBOUT[0].is_dirty() || RGBOUT[1].is_dirty() ||
          RGBOUT[2].is_dirty() || RGBOUT[3].is_dirty()))
        return;

    static uint64_t next   = 0;
    static uint64_t period = 0;

    if (!period)
    {
        const uint32_t tpm = time_hw_ticks_per_ms();
        uint64_t p = (uint64_t)tpm * 10ull;
        period = (p ? p : 1ull);
    }

    const uint64_t now = time_ticks64();
    if (now < next) return;

    next = now + period;

    SYS_RGB.updata();
    RGBOUT[0].updata();
    RGBOUT[1].updata();
    RGBOUT[2].updata();
    RGBOUT[3].updata();
}

static uint8_t g_fil_dirty   = 0;
static uint8_t g_loaded_ch   = 0xFF;
static uint8_t g_state_dirty = 0;

static inline void ram_to_flashinfo(uint8_t fil, Flash_FilamentInfo* o)
{
    const _ams* a = &ams[BAMBU_BUS_AMS_NUM];

    _filament f;
    memcpy(&f, &a->filament[fil], sizeof(f));

    memcpy(o->bambubus_filament_id, f.bambubus_filament_id, 8);
    o->color_R = f.color_R;
    o->color_G = f.color_G;
    o->color_B = f.color_B;
    o->color_A = f.color_A;
    o->temperature_min = f.temperature_min;
    o->temperature_max = f.temperature_max;
    memcpy(o->name, f.name, 16);
}

static inline void flashinfo_to_ram(uint8_t fil, const Flash_FilamentInfo* i)
{
    _ams* a = &ams[BAMBU_BUS_AMS_NUM];

    _filament f;
    memcpy(&f, &a->filament[fil], sizeof(f));

    memcpy(f.bambubus_filament_id, i->bambubus_filament_id, 8);
    f.color_R = i->color_R;
    f.color_G = i->color_G;
    f.color_B = i->color_B;
    f.color_A = i->color_A;
    f.temperature_min = i->temperature_min;
    f.temperature_max = i->temperature_max;

    memset(f.name, 0, sizeof(f.name));
    memcpy(f.name, i->name, sizeof(i->name));
    f.name[19] = 0;

    memcpy(&a->filament[fil], &f, sizeof(f));
}

bool ams_datas_read()
{
    bool any = false;

    for (uint8_t fil = 0; fil < 4; fil++)
    {
        Flash_FilamentInfo fi;
        if (Flash_AMS_filament_read(fil, &fi))
        {
            flashinfo_to_ram(fil, &fi);
            any = true;
        }
    }

    return any;
}

void ams_datas_set_need_to_save_filament(uint8_t filament_idx)
{
    if (filament_idx >= 4) return;
    g_fil_dirty |= (1u << filament_idx);
}

void ams_state_set_loaded(uint8_t filament_ch)
{
    if (filament_ch >= 4u) return;
    if (g_loaded_ch != 0xFFu) return;
    g_loaded_ch = filament_ch;
    g_state_dirty = 1;
}

void ams_state_set_unloaded(uint8_t filament_ch)
{
    if (g_loaded_ch == 0xFFu) return;
    if (filament_ch < 4u && g_loaded_ch != filament_ch) return;
    g_loaded_ch = 0xFFu;
    g_state_dirty = 1;
}

uint8_t ams_state_get_loaded(void)
{
    return g_loaded_ch;
}

static void ams_state_save_run()
{
    if (!g_state_dirty) return;

    if (Flash_AMS_state_write(g_loaded_ch, nullptr))
        g_state_dirty = 0;
}

void ams_datas_save_run()
{
    if (!g_fil_dirty) return;

    uint8_t fil = 0xFF;
    for (uint8_t i = 0; i < 4; i++)
        if (g_fil_dirty & (1u << i)) { fil = i; break; }

    if (fil == 0xFF) return;

    Flash_FilamentInfo now;
    ram_to_flashinfo(fil, &now);

    if (Flash_AMS_filament_write(fil, &now, g_loaded_ch))
        g_fil_dirty &= ~(1u << fil);
}

int main(void)
{
    SystemInit();
    SystemCoreClockUpdate();
    time_hw_init();

    __enable_irq();

    WWDG_DeInit();
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_WWDG, DISABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);

    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);

    GPIO_PinRemapConfig(GPIO_Remap_PD01, ENABLE);

    RGB_init();
    delay(10);

    SYS_RGB.set_RGB(0x10, 0x00, 0x00, 0);
    for (int i = 0; i < 4; i++) RGBOUT[i].set_RGB(0, 0, 0, 0);
    RGB_update();
    delay(50);

    DEBUG_init();
    ams_init();
    Flash_saves_init();

    ADC_DMA_init();
    ADC_DMA_wait_full();

    MC_PULL_calibration_boot();

    ams_datas_read();

    {
        uint8_t ch = 0xFFu;
        if (Flash_AMS_state_read(&ch))
        {
            g_loaded_ch = ch;

            if (ch < 4u)
            {
                _ams* a = &ams[BAMBU_BUS_AMS_NUM];

                a->now_filament_num  = ch;
                a->filament_use_flag = 0x04;
                a->pressure          = 0x2B00;

                for (uint8_t i = 0; i < 4u; i++)
                    a->filament[i].motion = _filament_motion::idle;

                a->filament[ch].motion = _filament_motion::on_use;
            }
        }
    }

    ADC_DMA_poll();
    Motion_control_init();

    bus_init();

    DEBUG("START\n");

    while (1)
    {
        const ahubus_package_type   ahub_stu     = ahubus_run();
        const bambubus_package_type bambubus_stu = bambubus_run();
        bus_port_to_host.send_package();

        static int error = 0;

        if ((ahub_stu != ahubus_package_type::none) || (bambubus_stu != bambubus_package_type::none))
        {
            if ((ahub_stu != ahubus_package_type::error) || (bambubus_stu != bambubus_package_type::error))
            {
                error = 0;

                if (bambubus_stu == bambubus_package_type::heartbeat)
                {
                    SYS_RGB.set_RGB(0x38, 0x35, 0x32, 0);
                    bus_host_device_type = host_device_type_ams;
                }

                if (ahub_stu == ahubus_package_type::heartbeat)
                {
                    bus_host_device_type = host_device_type_ahub;
                }

                ams_datas_save_run();
                ams_state_save_run();
            }
            else
            {
                error = -1;
                SYS_RGB.set_RGB(0x10, 0x00, 0x00, 0);
            }
        }

        ADC_DMA_poll();
        Motion_control_run(error);
        RGB_update();
    }
}