#pragma once
#include "crc_bus.h"
#include <string.h>

enum class _bus_data_type : uint8_t
{
    bambubus = 0x3D,
    ahub_bus = 0x33,
    none = 0x00
};

class _bus_port_deal // 中断数据处理
{
public:
    uint8_t send_data_buf[1280] __attribute__((aligned(4)));
private:
    uint8_t tx_dma_buf[1280] __attribute__((aligned(4)));
    uint8_t recv_data_buf[2][1280] __attribute__((aligned(4)));
    uint8_t tx_build_sel = 0;
    int _index = 0;
    int length = 999;
    uint8_t data_length_index = 0;
    uint8_t data_CRC8_index = 0;
    _bus_data_type irq_package_type = _bus_data_type::none;
    uint8_t *bus_irq_data_ptr = recv_data_buf[0];
    void (*port_send_datas)(uint8_t *data, uint16_t len);
public:
    uint8_t * volatile bus_recv_data_ptr = recv_data_buf[0];
    volatile int recv_data_len = 0;
    volatile int send_data_len = 0;
    volatile _bus_data_type bus_package_type = _bus_data_type::none;
    volatile bool idle = true;
    inline uint8_t* tx_build_buf()
        {
            return tx_build_sel ? tx_dma_buf : send_data_buf;
        }
    void init(void (*_port_send_datas)(uint8_t *data, uint16_t len))
    {
        _index = 0;
        bus_irq_data_ptr = recv_data_buf[0];
        bus_recv_data_ptr = recv_data_buf[1];
        idle = true;
        send_data_len = 0;
        recv_data_len = 0;
        tx_build_sel  = 0;
        port_send_datas = _port_send_datas;
    }
    void irq(uint8_t data)
    {
        const int BUF_SZ = (int)sizeof(recv_data_buf[0]);

        if (_index == 0) // 状态为等待帧头
        {
            if (data == 0x3D || data == 0x33)
            {
                bus_irq_data_ptr[0] = data;
                data_length_index = 4;
                length = data_CRC8_index = 6;
                _index = 1;
                irq_package_type = (_bus_data_type)data;
            }
            return;
        }

        if (_index < 0 || _index >= BUF_SZ) {
            _index = 0;
            return;
        }

        bus_irq_data_ptr[_index] = data;

        if (_index == 1) // 包类型字节
        {
            if (data & 0x80) // 短帧头
            {
                data_length_index = 2;
                data_CRC8_index = 3;
            }
            else // 长帧头
            {
                data_CRC8_index = 6;
                data_length_index = (irq_package_type == _bus_data_type::bambubus) ? 5 : 4;
            }
        }

        if (_index == data_length_index) // 数据长度字节
        {
            if (irq_package_type == _bus_data_type::bambubus)
            {
                if (data_length_index == 2)
                {
                    length = data;
                } else {
                    length = (int)bus_irq_data_ptr[4] | ((int)data << 8);
                }
            }
            else if (irq_package_type == _bus_data_type::ahub_bus)
            {
                length = (((int)data) << 2) + 12;
            }

            if (length <= (int)data_CRC8_index || length > BUF_SZ) {
                _index = 0;
                return;
            }
        }

        if (_index == data_CRC8_index) // CRC8校验字节
        {
            if (data != bus_crc8(bus_irq_data_ptr, (uint32_t)data_CRC8_index))
            {
                _index = 0;
                return;
            }
        }

        ++_index;

        if (_index >= length) // 接收完毕，交换缓冲器指针
        {
            _index = 0;
            if (recv_data_len == 0)
            {
                uint8_t *_data_ptr = bus_recv_data_ptr;
                bus_recv_data_ptr = bus_irq_data_ptr;
                bus_irq_data_ptr = _data_ptr;
                bus_package_type = irq_package_type;
                recv_data_len = length;
            }
        }
    }
    void send_package()
    {
        const int len = send_data_len;
        if (len > 0 && len <= 1280)
        {
            if (!idle) return;

            uint8_t* tx = tx_build_buf();
            tx_build_sel ^= 1;

            port_send_datas(tx, (uint16_t)len);
            send_data_len = 0;
        }
    }

    void send_package(uint8_t *data, uint16_t len)
    {
        if (len > 0 && len <= 1280)
        {
            if (!idle) return;

            port_send_datas(data, len);
        }
    }
}__attribute__((aligned(4)));

extern _bus_port_deal bus_port_to_host;
extern void bus_init();

#define host_device_type_none 0x0000
#define host_device_type_ahub 0x0001
#define host_device_type_ams 0x0700
extern uint16_t bus_host_device_type;