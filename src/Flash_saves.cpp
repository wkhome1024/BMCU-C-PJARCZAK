#include "Flash_saves.h"
#include "hal/irq_wch.h"
#include <string.h>

#include "ch32v20x_rcc.h"
#include "ch32v20x_crc.h"
#include "ch32v20x_flash.h"

void Flash_saves_init()
{
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_CRC, ENABLE);
}

static uint32_t crc32_hw_words(const void* data, uint32_t bytes)
{
    const uint32_t* p = (const uint32_t*)data;
    CRC->CTLR = 1;
    for (uint32_t i = 0; i < (bytes >> 2); i++) CRC->DATAR = p[i];
    return CRC->DATAR;
}

static inline uint32_t ams_fil_page(uint8_t filament_idx)
{
    return FLASH_NVM_AMS_ADDR + (uint32_t)filament_idx * FLASH_NVM256_PAGE_SIZE;
}

static bool flash256_prog(uint32_t page_addr, const uint32_t w[64])
{
    if (page_addr & (FLASH_NVM256_PAGE_SIZE - 1u)) return false;

    if (memcmp((const void*)page_addr, (const void*)w, FLASH_NVM256_PAGE_SIZE) == 0) return true;

    const uint32_t irq = irq_save_wch();
    FLASH_Unlock_Fast();
    FLASH_ClearFlag(FLASH_FLAG_BSY | FLASH_FLAG_EOP | FLASH_FLAG_WRPRTERR);

    FLASH_ErasePage_Fast(page_addr);
    FLASH_ProgramPage_Fast(page_addr, (uint32_t*)w);

    FLASH_Lock_Fast();
    FLASH_Lock();
    irq_restore_wch(irq);

    return (memcmp((const void*)page_addr, (const void*)w, FLASH_NVM256_PAGE_SIZE) == 0);
}

static bool flash256_erase(uint32_t page_addr)
{
    if (page_addr & (FLASH_NVM256_PAGE_SIZE - 1u)) return false;

    const uint32_t irq = irq_save_wch();
    FLASH_Unlock_Fast();
    FLASH_ClearFlag(FLASH_FLAG_BSY | FLASH_FLAG_EOP | FLASH_FLAG_WRPRTERR);

    FLASH_ErasePage_Fast(page_addr);

    FLASH_Lock_Fast();
    FLASH_Lock();
    irq_restore_wch(irq);
    return true;
}

static bool nvm256_write(uint32_t page_addr, uint32_t magic, uint16_t ver, uint32_t rsv,
                         const void* payload, uint16_t len)
{
    if (len > (uint16_t)(NVM256_CRC_OFF - sizeof(NVM256_HDR))) return false;

    alignas(4) uint32_t w[64];
    uint8_t* b = (uint8_t*)w;
    memset(b, 0xFF, FLASH_NVM256_PAGE_SIZE);

    NVM256_HDR h{};
    h.magic = magic;
    h.ver   = ver;
    h.len   = len;
    h.rsv   = rsv;

    memcpy(b + 0, &h, sizeof(h));
    if (len) memcpy(b + sizeof(h), payload, len);

    const uint32_t crc = crc32_hw_words(b, NVM256_CRC_OFF);
    memcpy(b + NVM256_CRC_OFF, &crc, 4u);

    return flash256_prog(page_addr, w);
}

static bool nvm256_read(uint32_t page_addr, uint32_t magic, uint16_t ver,
                        void* out, uint16_t max_len, uint16_t* got_len, uint32_t* rsv_out)
{
    const uint8_t* b = (const uint8_t*)page_addr;

    const uint32_t stored = *(const uint32_t*)(b + NVM256_CRC_OFF);
    if (stored == 0xFFFFFFFFu) return false;

    const uint32_t crc = crc32_hw_words(b, NVM256_CRC_OFF);
    if (crc != stored) return false;

    NVM256_HDR h{};
    memcpy(&h, b + 0, sizeof(h));

    if (h.magic != magic) return false;
    if (h.ver   != ver)   return false;
    if (h.len   > max_len) return false;

    if (h.len) memcpy(out, b + sizeof(h), h.len);
    if (got_len) *got_len = h.len;
    if (rsv_out) *rsv_out = h.rsv;
    return true;
}

static inline uint32_t ams_rsv_pack(uint8_t filament_idx, uint8_t loaded_ch)
{
    return ((uint32_t)BAMBU_BUS_AMS_NUM & 0xFFu) |
           (((uint32_t)filament_idx & 0xFFu) << 8) |
           (((uint32_t)loaded_ch & 0xFFu) << 16) |
           (0xA5u << 24);
}

// ---- STATE LOG (pages 6..15) ----
static constexpr uint32_t STA_TAG = 0xA5u;
static constexpr uint32_t STA_PAGE_FIRST = 6u;
static constexpr uint32_t STA_PAGE_COUNT = 10u; // 6..15
static constexpr uint32_t STA_SLOT_BYTES = 8u;  // 2x word
static constexpr uint32_t STA_SLOTS_PER_PAGE = (FLASH_NVM256_PAGE_SIZE / STA_SLOT_BYTES); // 32
static constexpr uint32_t STA_TOTAL_SLOTS = (STA_PAGE_COUNT * STA_SLOTS_PER_PAGE);       // 320

static uint16_t g_sta_seq  = 0;
static uint16_t g_sta_slot = 0;

static inline uint32_t sta_page_addr(uint32_t page_i)
{
    return FLASH_NVM_BASE_ADDR + (STA_PAGE_FIRST + page_i) * FLASH_NVM256_PAGE_SIZE;
}

static inline uint32_t sta_slot_addr(uint32_t slot)
{
    const uint32_t page_i = slot / STA_SLOTS_PER_PAGE;
    const uint32_t slot_i = slot - page_i * STA_SLOTS_PER_PAGE;
    return sta_page_addr(page_i) + slot_i * STA_SLOT_BYTES;
}

static bool flash_word_prog_std(uint32_t addr, uint32_t data)
{
    const uint32_t irq = irq_save_wch();
    FLASH_Unlock();
    FLASH_ClearFlag(FLASH_FLAG_BSY | FLASH_FLAG_EOP | FLASH_FLAG_WRPRTERR);
    const FLASH_Status st = FLASH_ProgramWord(addr, data);
    FLASH_Lock();
    irq_restore_wch(irq);

    return (st == FLASH_COMPLETE) && (*(volatile uint32_t*)addr == data);
}

bool Flash_AMS_filament_write(uint8_t filament_idx, const Flash_FilamentInfo* info, uint8_t loaded_ch)
{
    (void)loaded_ch;
    if (!info || filament_idx >= 4) return false;

    const uint32_t rsv = ams_rsv_pack(filament_idx, 0xFFu); // decouple from loaded_ch
    return nvm256_write(ams_fil_page(filament_idx), MAGIC_FIL, VER_1, rsv, info, (uint16_t)sizeof(*info));
}

bool Flash_AMS_filament_read(uint8_t filament_idx, Flash_FilamentInfo* out)
{
    if (!out || filament_idx >= 4) return false;

    uint16_t got = 0;
    uint32_t rsv = 0;
    if (!nvm256_read(ams_fil_page(filament_idx), MAGIC_FIL, VER_1, out, (uint16_t)sizeof(*out), &got, &rsv))
        return false;

    if (got != sizeof(*out)) return false;
    (void)rsv;
    return true;
}

bool Flash_AMS_filament_clear(uint8_t filament_idx)
{
    if (filament_idx >= 4) return false;
    return flash256_erase(ams_fil_page(filament_idx));
}

bool Flash_AMS_state_read(uint8_t* loaded_ch)
{
    if (!loaded_ch) return false;

    uint8_t  best_ch   = 0xFFu;
    uint16_t best_seq  = 0;
    uint32_t best_slot = 0;
    uint8_t  have      = 0u;

    for (uint32_t slot = 0; slot < STA_TOTAL_SLOTS; slot++)
    {
        const uint32_t a  = sta_slot_addr(slot);
        const uint32_t w0 = *(const volatile uint32_t*)(a + 0u);
        const uint32_t w1 = *(const volatile uint32_t*)(a + 4u);

        if (w0 == 0xFFFFFFFFu && w1 == 0xFFFFFFFFu) continue;
        if ((w0 >> 24) != STA_TAG) continue;
        if ((w0 ^ w1) != MAGIC_STA) continue;

        const uint16_t seq = (uint16_t)((w0 >> 8) & 0xFFFFu);
        const uint8_t  ch  = (uint8_t)(w0 & 0xFFu);

        if (!have || (int16_t)(seq - best_seq) > 0)
        {
            have = 1u;
            best_seq = seq;
            best_ch  = ch;
            best_slot = slot;
        }
    }

    if (have)
    {
        g_sta_seq  = (uint16_t)(best_seq + 1u);
        g_sta_slot = (uint16_t)((best_slot + 1u) % STA_TOTAL_SLOTS);
    }
    else
    {
        g_sta_seq  = 0u;
        g_sta_slot = 0u;
    }

    *loaded_ch = best_ch;
    return true;
}

bool Flash_AMS_state_write(uint8_t loaded_ch, const Flash_FilamentInfo* filament0_info)
{
    (void)filament0_info;

    const uint16_t seq = g_sta_seq;

    const uint32_t w0 = (STA_TAG << 24) | ((uint32_t)seq << 8) | (uint32_t)loaded_ch;
    const uint32_t w1 = w0 ^ MAGIC_STA;

    const uint32_t start = (uint32_t)g_sta_slot;

    for (uint32_t i = 0; i < STA_TOTAL_SLOTS; i++)
    {
        const uint32_t s = (start + i) % STA_TOTAL_SLOTS;
        const uint32_t a = sta_slot_addr(s);

        const uint32_t cur0 = *(const volatile uint32_t*)(a + 0u);
        const uint32_t cur1 = *(const volatile uint32_t*)(a + 4u);

        if (cur0 == 0xFFFFFFFFu && cur1 == 0xFFFFFFFFu)
        {
            if (!flash_word_prog_std(a + 0u, w0)) return false;
            if (!flash_word_prog_std(a + 4u, w1)) return false;

            g_sta_seq  = (uint16_t)(seq + 1u);
            g_sta_slot = (uint16_t)((s + 1u) % STA_TOTAL_SLOTS);
            return true;
        }
    }

    // log full -> erase 1 page only
    const uint32_t page_i = start / STA_SLOTS_PER_PAGE;
    if (!flash256_erase(sta_page_addr(page_i))) return false;

    {
        const uint32_t a = sta_slot_addr(start);
        if (!flash_word_prog_std(a + 0u, w0)) return false;
        if (!flash_word_prog_std(a + 4u, w1)) return false;

        g_sta_seq  = (uint16_t)(seq + 1u);
        g_sta_slot = (uint16_t)((start + 1u) % STA_TOTAL_SLOTS);
        return true;
    }
}

struct alignas(4) Flash_CAL_payload
{
    float offs[4];
    float vmin[4];
    float vmax[4];
};

bool Flash_MC_PULL_cal_write_all(const float offs[4], const float vmin[4], const float vmax[4])
{
    Flash_CAL_payload p;
    memcpy(p.offs, offs, sizeof(p.offs));
    memcpy(p.vmin, vmin, sizeof(p.vmin));
    memcpy(p.vmax, vmax, sizeof(p.vmax));

    return nvm256_write(FLASH_NVM_CAL_ADDR, MAGIC_CAL, VER_1, 0, &p, (uint16_t)sizeof(p));
}

bool Flash_MC_PULL_cal_read(float offs[4], float vmin[4], float vmax[4])
{
    Flash_CAL_payload p;
    uint16_t got = 0;

    if (!nvm256_read(FLASH_NVM_CAL_ADDR, MAGIC_CAL, VER_1,
                     &p, (uint16_t)sizeof(p), &got, nullptr))
        return false;

    if (got != sizeof(p)) return false;

    memcpy(offs, p.offs, sizeof(p.offs));
    memcpy(vmin, p.vmin, sizeof(p.vmin));
    memcpy(vmax, p.vmax, sizeof(p.vmax));
    return true;
}

bool Flash_MC_PULL_cal_clear(void)
{
    return flash256_erase(FLASH_NVM_CAL_ADDR);
}

bool Flash_Motion_write(const void* in, uint16_t bytes)
{
    if (!in || bytes == 0) return false;
    return nvm256_write(FLASH_NVM_MOTION_ADDR, MAGIC_MOT, VER_1, 0, in, bytes);
}

bool Flash_Motion_read(void* out, uint16_t bytes)
{
    if (!out || bytes == 0) return false;

    uint16_t got = 0;
    if (!nvm256_read(FLASH_NVM_MOTION_ADDR, MAGIC_MOT, VER_1,
                     out, bytes, &got, nullptr))
        return false;

    return (got == bytes);
}

bool Flash_Motion_clear(void)
{
    return flash256_erase(FLASH_NVM_MOTION_ADDR);
}