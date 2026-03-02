#include "bambu_bus_ams.h"
#include <string.h>
#include "hal/irq_wch.h"
#include "ams.h"
#include "hal/time_hw.h"
#include "app_api.h"
#include "_bus_hardware.h"
#include "crc_bus.h"

uint8_t bambubus_ams_map[4] = {0, 1, 2, 3};
uint16_t bambubus_ams_address = 0x0000;

bool package_check_crc16(uint8_t *data, int data_length)
{
    if (data_length < 4) return false;

    const int crc_off = data_length - 2;
    const uint16_t num = bus_crc16(data, (uint32_t)crc_off);

    return (data[crc_off] == (num & 0xFFu)) &&
           (data[crc_off + 1] == ((num >> 8) & 0xFFu));
}

void bambubus_init()
{
    // data_save.BambuBus_now_filament_num = 0xFF;
}

void package_add_crc(uint8_t *data, int send_data_length) // 为数据包添加crc校验
{
    if (data[1] & 0x80) // 获取数据包头位置
    {

        data[3] = bus_crc8(data, 3); // 短帧头校验
    }
    else
    {
        data[6] = bus_crc8(data, 6); // 长帧头校验
    }
    send_data_length -= 2;                                              // 校验位之前的数据长度
    uint16_t num = bus_crc16(data, (uint32_t)send_data_length); // 计算crc16校验
    data[(send_data_length)] = num & 0xFF;                              // crc16校验低字节
    data[(send_data_length + 1)] = num >> 8;                            // crc16校验高字节
}

struct bambubus_long_packge_data
{
    uint16_t package_number;
    uint16_t package_length;
    uint8_t crc8;
    uint16_t target_address;
    uint16_t source_address;
    uint16_t type;
    uint8_t *datas;
    uint16_t data_length;
} __attribute__((packed));

void bambubus_long_package_get(bambubus_long_packge_data *data)
{
    if (bus_port_to_host.send_data_len != 0) return;
    uint8_t* out = bus_port_to_host.tx_build_buf();

    out[0] = 0x3D;
    out[1] = 0x00;

    data->package_length = data->data_length + 15;
    memcpy(out + 2,  data,        11);
    memcpy(out + 13, data->datas, data->data_length);

    package_add_crc(out, data->data_length + 15);
    bus_port_to_host.send_data_len = data->data_length + 15;
}

void bambubus_long_package_analysis(uint8_t *buf, int data_length, bambubus_long_packge_data *data)
{
    if (data_length < 15) {
        memset(data, 0, sizeof(*data));
        return;
    }
    memcpy(data, buf + 2, 11);
    data->datas = buf + 13;
    data->data_length = data_length - 15; // +2byte CRC16
}

bambubus_long_packge_data printer_data_long;
// 用于分辨bambubus数据包是否应该处理，类型是什么
bambubus_package_type get_packge_type(unsigned char *buf, int length)
{
    if (length < 6) return bambubus_package_type::none;

    if (buf[0] != 0x3D)
    {
        return bambubus_package_type::none;
    }
    if (package_check_crc16(buf, length) == false)
    {
        return bambubus_package_type::none;
    }
    if (buf[1] == 0xC5)
    {

        switch (buf[4])
        {
        case 0x03:
            return bambubus_package_type::filament_motion_short;
        case 0x04:
            return bambubus_package_type::filament_motion_long;
        case 0x05:
            return bambubus_package_type::online_detect;
        case 0x06:
            return bambubus_package_type::REQx6;
        case 0x07:
            return bambubus_package_type::NFC_detect;
        case 0x08:
            return bambubus_package_type::set_filament_info;
        case 0x20:
            return bambubus_package_type::heartbeat;
        default:
            return bambubus_package_type::ETC;
        }
    }
    else if (buf[1] == 0x05)
    {
        if (length < 15) return bambubus_package_type::none;
        bambubus_long_package_analysis(buf, length, &printer_data_long);
        if (printer_data_long.target_address != host_device_type_ams)
        {
            return bambubus_package_type::none;
        }

        bambubus_ams_address = host_device_type_ams;

        switch (printer_data_long.type)
        {
        case 0x21A:
            return bambubus_package_type::MC_online;
        case 0x211:
            return bambubus_package_type::read_filament_info;
        case 0x218:
            return bambubus_package_type::set_filament_info_type2;
        case 0x103:
            return bambubus_package_type::version;
        case 0x402:
            return bambubus_package_type::serial_number;
        default:
            return bambubus_package_type::ETC;
        }
    }
    return bambubus_package_type::none;
}
uint8_t package_num = 0;

uint8_t get_filament_left_char(_ams *ams)
{
    uint8_t data = 0;
    for (int i = 0; i < 4; i++)
    {
        if (ams->filament[i].online == true)
        {
            data |= (0x1 << i) << i; // 1<<(2*i)
            if (bambubus_ams_address == host_device_type_ams)
                if (ams->filament[i].motion != _filament_motion::idle)
                {
                    data |= (0x2 << i) << i; // 2<<(2*i)
                }
        }
    }
    return data;
}

static uint32_t time_sendout_onuse_ticks[4] = {};
 bool set_motion(unsigned char read_num, unsigned char statu_flags, unsigned char fliment_motion_flag, uint8_t ams_num)
 {
     _ams *ams_ptr = &ams[bambubus_ams_map[ams_num]];

     if (bambubus_ams_address == host_device_type_ams) // AMS08
     {
         if (read_num < 4)
         {
             const uint8_t ch = (uint8_t)read_num;

             const bool is_send_out      = ((statu_flags == 0x03) && (fliment_motion_flag == 0x00));
             const bool is_before_on_use = ((statu_flags == 0x09) && (fliment_motion_flag == 0xA5));
             const bool is_stop_on_use   = ((statu_flags == 0x07) && (fliment_motion_flag == 0x00));
             const bool is_on_use        = ((statu_flags == 0x07) && (fliment_motion_flag == 0x7F));
             const bool is_before_pullb  = ((statu_flags == 0x09) && (fliment_motion_flag == 0x3F));

             uint32_t &t_sendout_onuse = time_sendout_onuse_ticks[ch];

             uint8_t loaded = 0xFFu;
             bool allow_any = true;
             bool allow_stop = true;

             if (ams_num == (uint8_t)BAMBU_BUS_AMS_NUM)
             {
                 loaded = ams_state_get_loaded();
                 allow_any  = (loaded == 0xFFu) || (loaded == ch);
                 allow_stop = (loaded == ch);
             }

             const bool accept =
                 (is_send_out) ||
                 (is_before_on_use && allow_any) ||
                 (is_on_use        && allow_any) ||
                 (is_stop_on_use   && allow_stop) ||
                 (is_before_pullb  && allow_stop);

             if (accept)
             {
                 if (ams_ptr->now_filament_num != ch)
                 {
                     if (ams_ptr->now_filament_num < 4)
                     {
                         const uint8_t prev = ams_ptr->now_filament_num;
                         ams_ptr->filament[prev].motion = _filament_motion::idle;
                         ams_ptr->filament_use_flag = 0x00;
                         ams_ptr->pressure = 0xFFFF;

                         time_sendout_onuse_ticks[prev] = 0u;
                     }
                     bus_now_ams_num = bambubus_ams_map[ams_num];
                     ams_ptr->now_filament_num = ch;
                 }
             }

             if (is_send_out)
             {
                 t_sendout_onuse = 0u;

                 if (ams_num == (uint8_t)BAMBU_BUS_AMS_NUM)
                 {
                     const _filament_motion prev = ams_ptr->filament[ch].motion;

                     if (prev != _filament_motion::send_out && ams_state_get_loaded() != 0xFFu)
                         ams_state_set_unloaded(0xFFu);
                 }

                 ams_ptr->filament[ch].motion = _filament_motion::send_out;
                 ams_ptr->pressure = 0x4700;
             }
             else if (is_before_on_use)
             {
                 t_sendout_onuse = 0u;

                 if (!allow_any) return true;

                 const _filament_motion prev = ams_ptr->filament[ch].motion;

                 ams_ptr->filament[ch].motion = _filament_motion::before_on_use;
                 ams_ptr->filament_use_flag   = 0x04;
                 ams_ptr->pressure = (prev == _filament_motion::send_out) ? 0x4700 : 0x2B00;

                 if (ams_num == (uint8_t)BAMBU_BUS_AMS_NUM)
                     ams_state_set_loaded(ch);
             }
             else if (is_stop_on_use)
             {
                 t_sendout_onuse = 0u;

                 if (!allow_stop) return true;

                 const _filament_motion prev = ams_ptr->filament[ch].motion;

                 if (prev == _filament_motion::on_use ||
                     prev == _filament_motion::before_on_use)
                 {
                     ams_ptr->filament[ch].motion = _filament_motion::stop_on_use;
                 }

                 ams_ptr->filament_use_flag   = 0x04;
                 ams_ptr->pressure = (prev == _filament_motion::send_out) ? 0x4700 : 0x2B00;
             }
             else if (is_on_use)
             {
                 if (!allow_any) return true;

                 const _filament_motion prev = ams_ptr->filament[ch].motion;

                 if (prev == _filament_motion::send_out)
                 {
                     if (time_hw_tpms != 0u)
                     {
                         const uint32_t now = time_ticks32();
                         if (t_sendout_onuse == 0u) t_sendout_onuse = now;

                         const uint32_t dt  = (uint32_t)(now - t_sendout_onuse);
                         const uint32_t lim = 5000u * (uint32_t)time_hw_tpms;

                         if (dt < lim)
                         {
                             ams_ptr->filament_use_flag = 0x04;
                             ams_ptr->pressure          = 0x4700;
                             return true;
                         }

                         t_sendout_onuse = 0u;
                     }
                     else
                     {
                         ams_ptr->filament_use_flag = 0x04;
                         ams_ptr->pressure          = 0x4700;
                         return true;
                     }
                 }

                 t_sendout_onuse = 0u;

                 ams_ptr->filament[ch].motion = _filament_motion::on_use;
                 ams_ptr->filament_use_flag   = 0x04;

                 if (ams_ptr->pressure != 0xF06Fu)
                     ams_ptr->pressure = 0x2B00;

                 if (ams_num == (uint8_t)BAMBU_BUS_AMS_NUM)
                     ams_state_set_loaded(ch);
             }
             else if (is_before_pullb)
             {
                 t_sendout_onuse = 0u;

                 if (!allow_stop) return true;

                 const _filament_motion prev = ams_ptr->filament[ch].motion;

                 if (prev == _filament_motion::on_use ||
                     prev == _filament_motion::before_on_use ||
                     prev == _filament_motion::stop_on_use)
                 {
                     ams_ptr->filament[ch].motion = _filament_motion::before_pull_back;
                 }

                 ams_ptr->filament_use_flag = 0x04;
                 ams_ptr->pressure = 0x2B00;

                 if (ams_num == (uint8_t)BAMBU_BUS_AMS_NUM)
                     ams_state_set_unloaded(ch);
             }
             else if (statu_flags == 0x09)
             {
                 t_sendout_onuse = 0u;

                 ams_ptr->filament_use_flag = 0x04;
                 ams_ptr->pressure = 0x2B00;
             }
         }
         else if (read_num == 0xFF)
         {
             if ((statu_flags == 0x03) && (fliment_motion_flag == 0x00)) // wejście w pull_back
             {
                 if (ams_ptr->now_filament_num < 4)
                 {
                     const uint8_t ch = ams_ptr->now_filament_num;
                     const _filament_motion m = ams_ptr->filament[ch].motion;

                     time_sendout_onuse_ticks[ch] = 0u;

                     if (m == _filament_motion::before_pull_back ||
                         m == _filament_motion::on_use ||
                         m == _filament_motion::before_on_use ||
                         m == _filament_motion::stop_on_use)
                     {
                         ams_ptr->filament[ch].motion = _filament_motion::pull_back;
                         ams_ptr->filament_use_flag   = 0x02;
                     }
                        ams_ptr->pressure = 0x4700;

                     if (ams_num == (uint8_t)BAMBU_BUS_AMS_NUM)
                         ams_state_set_unloaded(ch);
                 }
             }
             else if (statu_flags == 0x01)
             {
                 if (ams_num == (uint8_t)BAMBU_BUS_AMS_NUM)
                 {
                     const uint8_t ch = ams_ptr->now_filament_num;
                     if (ch < 4 && ams_ptr->filament_use_flag != 0x04)
                         ams_state_set_unloaded(ch);
                 }
             }
             else
             {
                 if (ams_ptr->now_filament_num < 4)
                 {
                     const uint8_t ch = ams_ptr->now_filament_num;
                     const _filament_motion m = ams_ptr->filament[ch].motion;

                     if (m == _filament_motion::on_use ||
                         m == _filament_motion::before_on_use ||
                         m == _filament_motion::stop_on_use)
                     {
                         return true;
                     }
                 }

                 for (uint8_t i = 0; i < 4; i++)
                 {
                     ams_ptr->filament[i].motion = _filament_motion::idle;
                     time_sendout_onuse_ticks[i] = 0u;
                 }

                 ams_ptr->filament_use_flag = 0x00;
                 ams_ptr->pressure          = 0xFFFF;
                 ams_ptr->now_filament_num  = 0xFF;

                 if (ams_num == (uint8_t)BAMBU_BUS_AMS_NUM)
                     ams_state_set_unloaded(0xFFu); // global clear
             }
         }
     }
    else if (bambubus_ams_address == 0x0000) // none
    {
    }
    else
        return false;

    return true;
}
// 3D C5 0C C8 03 00 07 00 7F 02 36 54
struct bambubus_printer_motion_package_struct
{
    uint8_t magic_byte;
    uint8_t flag;
    uint8_t length;
    uint8_t crc8;
    uint8_t command;
    uint8_t ams_num;
    uint8_t statu_flag;
    uint8_t filamnet_channel;
    uint8_t motion_flag;
    uint8_t unknow;
    uint16_t crc16;
} __attribute__((packed));
struct bambubus_ams_motion_package_struct
{
    uint8_t magic_byte = 0x3D;
    uint8_t flag = 0xC0;
    uint8_t length = 0x2C;
    uint8_t crc8;
    uint8_t command = 0x03;
    uint8_t ams_num = 0;
    uint8_t unknow1 = 0x00;
    uint8_t filament_use_flag = 0x00;
    uint8_t filament_channel = 0x00;
    float meters = 0;
    uint16_t pressure = 0;
    uint16_t unknow2 = 0xFFFF;
    uint8_t unknow3[12];
    uint8_t filament_stu_flag = 0x00;
    uint32_t last1 = 0xFFFFFFFF;
    uint32_t last2 = 0x01010101;
    uint8_t filament_channel_2 = 0x00;
    uint8_t last4 = 0x00;
    uint16_t last5 = 0x0000;
    uint16_t crc16;
} __attribute__((packed));
// 3D F0 2C C1 03 00 00 00 FF 00 00 00 00 6F F0 FB FF 36 00 00 00 F8 FF F7 FF 00 00 27 00 55 F8 EE F9 F0 B7 BA B9 B2 00 00 00 00 88 E6
// 3D D0 2C D1 03 03 00 02 00 00 00 80 3F FF FF FF FF 36 00 00 00 00 00 00 00 00 00 27 00 55 FF FF FF FF 01 01 01 01 00 00 00 00 15 95
static const bambubus_ams_motion_package_struct _bambubus_ams_motion_package_struct_init_data = {
    0x3D,       // magic_byte
    0xC0,       // flag
    0x2C,       // length
    0x00,       // crc8
    0x03,       // command
    0x00,       // ams_num
    0x00,       // unknow1
    0x00,       // filament_use_flag
    0x00,       // filament_channel
    0.0f,       // meters
    0x0000,     // pressure
    0xFFFF,     // unknow2
    {           // unknow3[12]
        0x36,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x27,0x00
    },
    0x00,           // filament_stu_flag
    0xFFFFFFFFu,    // last1
    0x01010101u,    // last2
    0x00,           // filament_channel_2
    0x00,           // last4
    0x0000,         // last5
    0x0000          // crc16
};
void get_package_motion(bambubus_printer_motion_package_struct *package_recv)
{
    if (bus_port_to_host.send_data_len != 0) return;
    uint8_t *out = bus_port_to_host.tx_build_buf();

    bambubus_printer_motion_package_struct in;
    memcpy(&in, package_recv, sizeof(in));

    const uint8_t ams_num = in.ams_num;
    if (ams_num >= 4u) return;

    const uint8_t ams_idx = bambubus_ams_map[ams_num];
    if (!ams[ams_idx].online) return;

    _ams *ams_ptr = &ams[ams_idx];
    if (!set_motion(in.filamnet_channel, in.statu_flag, in.motion_flag, ams_num)) return;

    auto *package_send = (bambubus_ams_motion_package_struct *)out;
    memcpy(package_send, &_bambubus_ams_motion_package_struct_init_data, sizeof(*package_send));

    const uint8_t  ch       = ams_ptr->now_filament_num;
    const uint16_t pressure = ams_ptr->pressure;

    package_send->flag = 0xC0u | (uint8_t)(package_num << 3);
    package_send->ams_num = ams_num;
    package_send->filament_use_flag = ams_ptr->filament_use_flag;
    package_send->filament_channel = ch;
    package_send->filament_channel_2 = ch;

    float meters = 1.0f;
    if (ch < 4u)
    {
        meters = ams_ptr->filament[ch].meters;
    }

    memcpy(&package_send->meters, &meters, sizeof(meters));
    memcpy(&package_send->pressure, &pressure, sizeof(pressure));
    package_send->filament_stu_flag = get_filament_left_char(ams_ptr);

    if (pressure == 0xF06Fu)
    {
        package_send->pressure = 0xF06Fu;
        package_send->unknow2 = 0x1CE7u;
    }

    package_num = (package_num < 7u) ? (uint8_t)(package_num + 1u) : 0u;

    package_add_crc(out, sizeof(bambubus_ams_motion_package_struct));
    bus_port_to_host.send_data_len = sizeof(bambubus_ams_motion_package_struct);
}

// 3D C5 0D F1 04 00 01 00 03 FF 00 B2 C4
struct bambubus_printer_stu_motion_package_struct
{
    uint8_t magic_byte;
    uint8_t flag;
    uint8_t length;
    uint8_t crc8;
    uint8_t command;
    uint8_t ams_num;
    uint8_t statu_flag;
    uint8_t motion_flag;
    uint8_t unknow1;
    uint8_t filamnet_channel;
    uint8_t unknow2;
    uint16_t crc16;
} __attribute__((packed));
static_assert(sizeof(bambubus_printer_stu_motion_package_struct) == 13, "packed size mismatch");

struct bambubus_ams_stu_motion_package_struct
{
    uint8_t magic_byte = 0x3D;
    uint8_t flag = 0xC0;
    uint8_t length = 0x3C;
    uint8_t crc8;
    uint8_t command = 0x04;
    uint8_t ams_num_stu = 0;
    uint16_t temperature = 0; // 温度，单位0.1℃
    uint8_t humidity = 0;     // 湿度，单位%
    uint8_t filament_online_flag[3];
    uint8_t filament_channel_stu = 0x00;
    uint8_t filament_flag_wait_NFC = 0x00;
    uint8_t unknow_stu[3];
    uint8_t ams_num = 0;
    uint8_t unknow1 = 0x00;
    uint8_t filament_use_flag = 0x00;
    uint8_t filament_channel = 0x00;
    float meters = 0;
    uint16_t pressure = 0;
    uint16_t unknow2 = 0xFFFF;
    uint8_t unknow3[12];
    uint8_t filament_stu_flag = 0x00;
    uint32_t last1 = 0xFFFFFFFF;
    uint32_t last2 = 0x01010101;
    uint32_t last3 = 0x00000000;
    uint32_t last4 = 0xFFFFFFFF;
    uint16_t crc16;
} __attribute__((packed));

static const bambubus_ams_stu_motion_package_struct _bambubus_ams_stu_motion_package_struct_init_data = {
    0x3D,       // magic_byte
    0xC0,       // flag
    0x3C,       // length
    0x00,       // crc8
    0x04,       // command
    0x00,       // ams_num_stu
    0x0000,     // temperature
    0x00,       // humidity
    {0x00,0x00,0x00}, // filament_online_flag[3]
    0x00,       // filament_channel_stu
    0x00,       // filament_flag_wait_NFC
    {0x01,0x00,0x00}, // unknow_stu[3]
    0x00,       // ams_num
    0x00,       // unknow1
    0x00,       // filament_use_flag
    0x00,       // filament_channel
    0.0f,       // meters
    0x0000,     // pressure
    0xFFFF,     // unknow2
    {           // unknow3[12]
        0x36,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x27,0x00
    },
    0x00,           // filament_stu_flag
    0xFFFFFFFFu,    // last1
    0x01010101u,    // last2
    0x00000000u,    // last3
    0xFFFFFFFFu,    // last4
    0x0000          // crc16
};
void get_package_stu_motion(bambubus_printer_stu_motion_package_struct *package_recv)
{
    if (bus_port_to_host.send_data_len != 0) return;
    uint8_t *out = bus_port_to_host.tx_build_buf();

    bambubus_printer_stu_motion_package_struct in;
    memcpy(&in, package_recv, sizeof(in));

    unsigned char filament_flag_on  = 0x00;
    unsigned char filament_flag_NFC = 0x00;

    const uint8_t ams_num = in.ams_num;
    if (ams_num >= 4u) return;

    const uint8_t ams_idx = bambubus_ams_map[ams_num];
    if (!ams[ams_idx].online) return;

    _ams *ams_ptr = &ams[ams_idx];

    for (uint8_t i = 0; i < 4u; i++)
        if (ams_ptr->filament[i].online)
            filament_flag_on |= (uint8_t)(1u << i);

    if (!set_motion(in.filamnet_channel, in.statu_flag, in.motion_flag, ams_num)) return;

    auto *package_send = (bambubus_ams_stu_motion_package_struct *)out;
    memcpy(package_send, &_bambubus_ams_stu_motion_package_struct_init_data, sizeof(*package_send));

    int16_t temperature = (int16_t)(
        ams_ptr->filament[0].compartment_temperature +
        ams_ptr->filament[1].compartment_temperature +
        ams_ptr->filament[2].compartment_temperature +
        ams_ptr->filament[3].compartment_temperature
    );
    temperature = (int16_t)(temperature * 10);
    if (temperature < 0) temperature = 0;
    temperature = (int16_t)(temperature >> 2);

    uint16_t humidity = (uint16_t)(
        ams_ptr->filament[0].compartment_humidity +
        ams_ptr->filament[1].compartment_humidity +
        ams_ptr->filament[2].compartment_humidity +
        ams_ptr->filament[3].compartment_humidity
    );
    humidity = (uint16_t)(humidity >> 2);

    const uint8_t  ch       = ams_ptr->now_filament_num;
    const uint16_t pressure = ams_ptr->pressure;

    package_send->flag = 0xC0u | (uint8_t)(package_num << 3);
    package_send->ams_num_stu = ams_num;
    package_send->temperature = (uint16_t)temperature;
    package_send->humidity = (uint8_t)humidity;

    const uint8_t on = (uint8_t)(filament_flag_on - filament_flag_NFC);
    package_send->filament_online_flag[0] = on;
    package_send->filament_online_flag[1] = on;
    package_send->filament_online_flag[2] = on;

    package_send->filament_channel_stu = in.filamnet_channel;
    package_send->filament_flag_wait_NFC = filament_flag_NFC;

    package_send->ams_num = ams_num;
    package_send->filament_use_flag = ams_ptr->filament_use_flag;
    package_send->filament_channel = ch;

    float meters = 1.0f;
    if (ch < 4u)
    {
        meters = ams_ptr->filament[ch].meters;
    }

    memcpy(&package_send->meters, &meters, sizeof(meters));
    memcpy(&package_send->pressure, &pressure, sizeof(pressure));
    package_send->filament_stu_flag = get_filament_left_char(ams_ptr);

    if (pressure == 0xF06Fu)
    {
        package_send->pressure = 0xF06Fu;
        package_send->unknow2 = 0x1CE7u;
    }

    package_add_crc(out, sizeof(bambubus_ams_stu_motion_package_struct));

    package_num = (package_num < 7u) ? (uint8_t)(package_num + 1u) : 0u;

    bus_port_to_host.send_data_len = sizeof(bambubus_ams_stu_motion_package_struct);
}

static inline __attribute__((always_inline)) void bus_wait_idle_5ms(void);
static inline __attribute__((always_inline)) void bus_wait_idle_5ms(void)
{
    const uint32_t t0  = time_ticks32();
    const uint32_t lim = 5u * time_hw_tpms;

    while (!bus_port_to_host.idle && (uint32_t)(time_ticks32() - t0) < lim) {
        __asm__ volatile ("nop");
    }
}

uint8_t online_detect_res[4][29] = {
    {0x3D, 0xC0, 0x1D, 0xB4, 0x05, 0x01, 0x00,
     0x0D, 0x0E, 0xA0, 0x35, 0x35, 0x30, 0x30, 0x00, 0x00, 0x30, 0x30, 0x30, 0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00,
     0x33, 0xF0},
    {0x3D, 0xC0, 0x1D, 0xB4, 0x05, 0x01, 0x00,
     0x0D, 0x0E, 0xA1, 0x30, 0x30, 0x30, 0x30, 0x00, 0x00, 0x30, 0x30, 0x30, 0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00,
     0x33, 0xF0},
    {0x3D, 0xC0, 0x1D, 0xB4, 0x05, 0x01, 0x00,
     0x0D, 0x0E, 0xA2, 0x30, 0x30, 0x30, 0x30, 0x00, 0x00, 0x30, 0x30, 0x30, 0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00,
     0x33, 0xF0},
    {0x3D, 0xC0, 0x1D, 0xB4, 0x05, 0x01, 0x00,
     0x0D, 0x0E, 0xA3, 0x30, 0x30, 0x30, 0x30, 0x00, 0x00, 0x30, 0x30, 0x30, 0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00,
     0x33, 0xF0}};

bool have_registered[4] = {false, false, false, false};

void get_package_online_detect(unsigned char *buf, int length)
{
    (void)length;
    if (bus_port_to_host.send_data_len != 0) return;
    if (buf[5] == 0x00) // 注册AMS序号用
    {
        for (int i = 0; i < 4; i++)
        {
            if (have_registered[i] == true)
                continue;
            if (ams[bambubus_ams_map[i]].online != true)
                continue;

            online_detect_res[i][0] = 0x3D; // 帧头
            online_detect_res[i][1] = 0xC0; // flag
            online_detect_res[i][2] = 29;   // 数据长度-29字节
            online_detect_res[i][3] = 0xB4; // CRC8
            online_detect_res[i][4] = 0x05; // 命令号
            online_detect_res[i][5] = 0x00; // 命令号
            online_detect_res[i][6] = i;    // AMS号码
            package_add_crc(online_detect_res[i], 29);
            uint8_t* out = bus_port_to_host.tx_build_buf();
            memcpy(out, online_detect_res[i], 29);
            bus_port_to_host.send_data_len = 29;

            bus_wait_idle_5ms();
            if (!bus_port_to_host.idle) return;
            bus_port_to_host.send_package();
            bus_wait_idle_5ms();
            if (!bus_port_to_host.idle) return;
        }
    }

    if (buf[5] == 0x01)
    {
        uint8_t ams_num = buf[6];
        if (ams_num >= 4)
        {
            return;
        }
        if (ams[bambubus_ams_map[ams_num]].online != true)
        {
            have_registered[ams_num] = false;
            return;
        }
        online_detect_res[ams_num][0] = 0x3D;    // 帧头
        online_detect_res[ams_num][1] = 0xC0;    // flag
        online_detect_res[ams_num][2] = 29;      // 数据长度-29字节
        online_detect_res[ams_num][3] = 0xB4;    // CRC8
        online_detect_res[ams_num][4] = 0x05;    // 命令号
        online_detect_res[ams_num][5] = 0x01;    // 命令号
        online_detect_res[ams_num][6] = ams_num; // AMS号码
        
        package_add_crc(online_detect_res[ams_num], 29); // 添加校验

        if (memcmp(online_detect_res[ams_num] + 7, buf + 7, 20) == 0)
        {
            have_registered[ams_num] = true;
        }
        else
        {
            have_registered[ams_num] = false;
        }
        uint8_t* out = bus_port_to_host.tx_build_buf();
        memcpy(out, online_detect_res[ams_num], 29);
        bus_port_to_host.send_data_len = 29;
    }
}

unsigned char long_packge_MC_online[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
void get_package_long_packge_MC_online(unsigned char *buf, int length)
{
    bambubus_long_packge_data data;
    bambubus_long_package_analysis(buf, length, &printer_data_long);

    uint8_t ams_num = printer_data_long.datas[0];
    if ((ams_num >= 4) || (ams[bambubus_ams_map[ams_num]].online != true))
    {
        return;
    }

    data.datas = long_packge_MC_online;
    data.datas[0] = ams_num;
    data.data_length = sizeof(long_packge_MC_online);

    data.package_number = printer_data_long.package_number;
    data.type = printer_data_long.type;
    data.source_address = printer_data_long.target_address;
    data.target_address = printer_data_long.source_address;
    bambubus_long_package_get(&data);
}
unsigned char long_packge_filament[] =
    {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x47, 0x46, 0x42, 0x30, 0x30, 0x00, 0x00, 0x00,
        0x41, 0x42, 0x53, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xDD, 0xB1, 0xD4, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x18, 0x01, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
void get_package_long_packge_filament(unsigned char *buf, int length)
{
    bambubus_long_packge_data data;

    uint8_t ams_num = printer_data_long.datas[0];
    uint8_t filament_num = printer_data_long.datas[1];

    if ((ams_num >= 4) || (ams[bambubus_ams_map[ams_num]].online != true) || filament_num >= 4)
    {
        return;
    }

    _ams *ams_ptr = ams + bambubus_ams_map[ams_num];
    long_packge_filament[0] = ams_num;
    long_packge_filament[1] = filament_num;
    memcpy(long_packge_filament + 19, ams_ptr->filament[filament_num].bambubus_filament_id, sizeof(ams_ptr->filament[filament_num].bambubus_filament_id));
    memcpy(long_packge_filament + 27, ams_ptr->filament[filament_num].name, sizeof(ams_ptr->filament[filament_num].name));
    long_packge_filament[59] = ams_ptr->filament[filament_num].color_R;
    long_packge_filament[60] = ams_ptr->filament[filament_num].color_G;
    long_packge_filament[61] = ams_ptr->filament[filament_num].color_B;
    long_packge_filament[62] = ams_ptr->filament[filament_num].color_A;
    memcpy(long_packge_filament + 79, &ams_ptr->filament[filament_num].temperature_max, 2);
    memcpy(long_packge_filament + 81, &ams_ptr->filament[filament_num].temperature_min, 2);

    data.datas = long_packge_filament;
    data.data_length = sizeof(long_packge_filament);

    data.package_number = printer_data_long.package_number;
    data.type = printer_data_long.type;
    data.source_address = printer_data_long.target_address;
    data.target_address = printer_data_long.source_address;
    bambubus_long_package_get(&data);
}

unsigned char long_packge_version_serial_number[] = {16, // 序列号长度=15
                                                     '0', 'E', 'A', '0', '3', '0', '3', '0', '3', '0', '3', '0', '0', '0', '0',
                                                     '0', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                     0x0E, 0xA0, 0x30, 0x30, 0x30, 0x30, 0x00, 0x00, // serial_number#2
                                                     0x30, 0x30, 0x30, 0x30,
                                                     0xFF, 0xFF, 0xFF, 0xFF,
                                                     0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xBB, 0x44, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};

void get_package_long_packge_serial_number(unsigned char *buf, int length)
{
    bambubus_long_packge_data data;
    uint8_t ams_num = printer_data_long.datas[33];
    if ((ams_num >= 4) || (ams[bambubus_ams_map[ams_num]].online != true))
    {
        return;
    }
    long_packge_version_serial_number[4] = 0x30 + ams_num; // 防止SN重复
    long_packge_version_serial_number[34] = 0xA0 + ams_num;
    if (bambubus_ams_address != host_device_type_ams)
    {
        return;
    }

    // long_packge_version_serial_number[0] = ams_ptr->serial_number_length;

    // memcpy(long_packge_version_serial_number + 1, ams_ptr->serial_number, ams_ptr->serial_number_length);
    data.datas = long_packge_version_serial_number;
    data.data_length = sizeof(long_packge_version_serial_number);
    data.datas[65] = ams_num;

    data.package_number = printer_data_long.package_number;
    data.type = printer_data_long.type;
    data.source_address = printer_data_long.target_address;
    data.target_address = printer_data_long.source_address;
    bambubus_long_package_get(&data);
}

//0x0A // 10
//0x14 // 20
//0x1E // 30
//0x28 // 40
//0x32 // 50
//0x3C // 60
//0x46 // 70
//0x50 // 80
//0x5A // 90
unsigned char long_packge_version_version_and_name_AMS08[] = {0x00, 0x00, 0x0A, 0x5A , // verison number
                                                              0x41, 0x4D, 0x53, 0x30, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
//unsigned char long_packge_version_version_and_name_AMS2PRO[] = {
//    0x00, 0x00, 0x00, 0x5A,
//    0x4E, 0x33, 0x46, 0x30, 0x35, 0x00, 0x00, 0x00,
//    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
//};


void get_package_long_packge_version(unsigned char *buf, int length)
{
    (void)buf;
    (void)length;

    bambubus_long_packge_data data;
    const uint8_t ams_num = printer_data_long.datas[0];

    if ((ams_num >= 4) || (ams[bambubus_ams_map[ams_num]].online != true))
        return;

    unsigned char *payload = nullptr;
    uint16_t payload_len = 0;

    if (bambubus_ams_address != host_device_type_ams)
    {
        return;
    }

    payload = long_packge_version_version_and_name_AMS08;
    payload_len = (uint16_t)sizeof(long_packge_version_version_and_name_AMS08);

    payload[payload_len - 1] = ams_num;

    data.datas = payload;
    data.data_length = payload_len;

    data.package_number = printer_data_long.package_number;
    data.type = printer_data_long.type;
    data.source_address = printer_data_long.target_address;
    data.target_address = printer_data_long.source_address;

    bambubus_long_package_get(&data);
}

unsigned char s = 0x01;

unsigned char set_filament_res[] = {0x3D, 0xC0, 0x08, 0xB2, 0x08, 0x60, 0xB4, 0x04};
void get_package_set_filament(unsigned char *buf, int length)
{
    if (bus_port_to_host.send_data_len != 0) return;
    uint8_t* out = bus_port_to_host.tx_build_buf();
    uint8_t b = buf[5];
    uint8_t ams_num  = (b >> 4) & 0x0F;
    uint8_t read_num = (b >> 0) & 0x0F;

    if (ams_num >= 4 || read_num >= 4 || ams[bambubus_ams_map[ams_num]].online != true) return;

    _ams *ams_ptr = ams + bambubus_ams_map[ams_num];
    read_num = read_num & 0x0F;
    memcpy(ams_ptr->filament[read_num].bambubus_filament_id, buf + 7, sizeof(ams_ptr->filament[read_num].bambubus_filament_id));
    ams_ptr->filament[read_num].color_R = buf[15];
    ams_ptr->filament[read_num].color_G = buf[16];
    ams_ptr->filament[read_num].color_B = buf[17];
    ams_ptr->filament[read_num].color_A = buf[18];
    memcpy(&ams_ptr->filament[read_num].temperature_min, buf + 19, 2);
    memcpy(&ams_ptr->filament[read_num].temperature_max, buf + 21, 2);
    memcpy(ams_ptr->filament[read_num].name, buf + 23, sizeof(ams_ptr->filament[read_num].name)); // 20
    ams_ptr->filament[read_num].name[19] = 0;
    memcpy(out, set_filament_res, sizeof(set_filament_res));
    bus_port_to_host.send_data_len = sizeof(set_filament_res);
}
unsigned char set_filament_res_type2[] = {0x00, 0x00, 0x00};
void get_package_set_filament_type2(unsigned char *buf, int length)
{
    if (bus_port_to_host.send_data_len != 0) return;
    (void)buf;
    (void)length;

    bambubus_long_packge_data data;

    const uint8_t ams_num  = printer_data_long.datas[0];
    const uint8_t read_num = printer_data_long.datas[1];

    if (ams_num >= 4 || read_num >= 4 || ams[bambubus_ams_map[ams_num]].online != true) return;

    _ams *ams_ptr = ams + bambubus_ams_map[ams_num];

    memcpy(ams_ptr->filament[read_num].bambubus_filament_id,
           printer_data_long.datas + 2,
           sizeof(ams_ptr->filament[read_num].bambubus_filament_id));

    ams_ptr->filament[read_num].color_R = printer_data_long.datas[10];
    ams_ptr->filament[read_num].color_G = printer_data_long.datas[11];
    ams_ptr->filament[read_num].color_B = printer_data_long.datas[12];
    ams_ptr->filament[read_num].color_A = printer_data_long.datas[13];

    memcpy(&ams_ptr->filament[read_num].temperature_min, printer_data_long.datas + 14, 2);
    memcpy(&ams_ptr->filament[read_num].temperature_max, printer_data_long.datas + 16, 2);
    memset(ams_ptr->filament[read_num].name, 0, sizeof(ams_ptr->filament[read_num].name));  // 20B
    memcpy(ams_ptr->filament[read_num].name, printer_data_long.datas + 18, 16);             // 16B
    ams_ptr->filament[read_num].name[19] = 0;

    set_filament_res_type2[0] = ams_num;
    set_filament_res_type2[1] = read_num;
    set_filament_res_type2[2] = 0x00;

    data.datas        = set_filament_res_type2;
    data.data_length  = sizeof(set_filament_res_type2);

    data.package_number = printer_data_long.package_number;
    data.type           = printer_data_long.type;
    data.source_address = printer_data_long.target_address;
    data.target_address = printer_data_long.source_address;

    bambubus_long_package_get(&data);
}

bambubus_package_type bambubus_run()
{
    bambubus_package_type stu = bambubus_package_type::none;

    static uint64_t deadline_hb = 0;
    const uint64_t now = time_ticks64();

    int rx_len = 0;
    _bus_data_type t = _bus_data_type::none;
    uint8_t *buf = nullptr;

    {
        const uint32_t s = irq_save_wch();
        rx_len = bus_port_to_host.recv_data_len;
        t      = bus_port_to_host.bus_package_type;
        buf    = bus_port_to_host.bus_recv_data_ptr;
        irq_restore_wch(s);
    }

    if (rx_len > 0 && t == _bus_data_type::bambubus)
    {
        if (buf != nullptr && rx_len <= 1280 && buf[0] == 0x3D)
        {
            const int len = rx_len;

            stu = get_packge_type(buf, len);

            switch (stu)
            {
            case bambubus_package_type::heartbeat:
                deadline_hb = now + (uint64_t)1000u * (uint64_t)time_hw_tpms;
                break;

            case bambubus_package_type::filament_motion_short:
                get_package_motion((bambubus_printer_motion_package_struct *)buf);
                break;

            case bambubus_package_type::filament_motion_long:
                get_package_stu_motion((bambubus_printer_stu_motion_package_struct *)buf);
                break;

            case bambubus_package_type::online_detect:
                get_package_online_detect(buf, len);
                break;

            case bambubus_package_type::MC_online:
                get_package_long_packge_MC_online(buf, len);
                break;

            case bambubus_package_type::read_filament_info:
                get_package_long_packge_filament(buf, len);
                break;

            case bambubus_package_type::version:
                get_package_long_packge_version(buf, len);
                break;

            case bambubus_package_type::serial_number:
                get_package_long_packge_serial_number(buf, len);
                break;

            case bambubus_package_type::set_filament_info:
            {
                const uint8_t b = buf[5];
                const uint8_t ams_num = (b >> 4) & 0x0F;
                const uint8_t fil     = (b >> 0) & 0x0F;

                get_package_set_filament(buf, len);

                if (ams_num == (uint8_t)BAMBU_BUS_AMS_NUM && fil < 4)
                    ams_datas_set_need_to_save_filament(fil);
                break;
            }

            case bambubus_package_type::set_filament_info_type2:
                get_package_set_filament_type2(buf, len);
                if (printer_data_long.datas[0] == (uint8_t)BAMBU_BUS_AMS_NUM && printer_data_long.datas[1] < 4)
                    ams_datas_set_need_to_save_filament(printer_data_long.datas[1]);
                break;

            default:
                break;
            }

            if (bus_port_to_host.send_data_len != 0) delay_us(50u);
        }

        {
            const uint32_t s = irq_save_wch();
            bus_port_to_host.recv_data_len    = 0;
            bus_port_to_host.bus_package_type = _bus_data_type::none;
            irq_restore_wch(s);
        }
    }

    if (now > deadline_hb)
        stu = bambubus_package_type::error;

    return stu;
}
