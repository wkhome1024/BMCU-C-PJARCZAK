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

    // jeśli identyczne - nie dotykamy
    if (memcmp((const void*)page_addr, (const void*)w, FLASH_NVM256_PAGE_SIZE) == 0) return true;

    const uint32_t irq = irq_save_wch();
    FLASH_Unlock_Fast();
    FLASH_ClearFlag(FLASH_FLAG_BSY | FLASH_FLAG_EOP | FLASH_FLAG_WRPRTERR);

    FLASH_ErasePage_Fast(page_addr);
    FLASH_ProgramPage_Fast(page_addr, (uint32_t*)w);

    FLASH_Lock_Fast();
    FLASH_Lock();
    irq_restore_wch(irq);

    // verify
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

    // CRC over bytes[0..251] (63 words)
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

/**
 * @brief Write filament info without state coupling to prevent Page 2 wear.
 */
bool Flash_AMS_filament_write(uint8_t filament_idx, const Flash_FilamentInfo* info, uint8_t loaded_ch)
{
    if (!info || filament_idx >= 4) return false;

    // Force 0xFFu in rsv field to decouple Page 2-5 from loaded_ch status.
    const uint32_t rsv = ams_rsv_pack(filament_idx, 0xFFu); 
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

/**
 * @brief Search for the latest loaded_ch record using rolling pages (Page 6-15).
 */
bool Flash_AMS_state_read(uint8_t* loaded_ch)
{
    if (!loaded_ch) return false;
    
    uint8_t ch = 0xFFu; // Default to unloaded
    
    // Scan from Page 15 down to Page 6 to find the newest record
    for (int p = 15; p >= 6; p--) {
        uint32_t addr = FLASH_NVM_BASE_ADDR + (uint32_t)(p * 256);
        uint16_t got = 0; 
        uint32_t rsv = 0;
        
        // nvm256_read performs CRC32 and Magic check internally
        if (nvm256_read(addr, MAGIC_STA, VER_1, nullptr, 0, &got, &rsv)) {
            ch = (uint8_t)(rsv & 0xFFu);
            break; // Latest valid record found
        }
    }
    
    *loaded_ch = ch;
    return true;
}

/**
 * @brief Write loaded_ch with a rolling-page wear leveling mechanism.
 */
bool Flash_AMS_state_write(uint8_t loaded_ch, const Flash_FilamentInfo* filament0_info)
{
    (void)filament0_info; 
    
    if (!(loaded_ch < 4u || loaded_ch == 0xFFu)) return false;

    int target_p = -1;
    // Find the first empty page (0xFFFFFFFF) in the pool
    for (int p = 6; p <= 15; p++) {
        uint32_t addr = FLASH_NVM_BASE_ADDR + (uint32_t)(p * 256);
        if (*(volatile uint32_t*)addr == 0xFFFFFFFFu) {
            target_p = p;
            break;
        }
    }

    // If pool is full, perform a full sweep of Page 6-15 (approx. 20ms)
    if (target_p == -1) {
        for (int p = 6; p <= 15; p++) {
            flash256_erase(FLASH_NVM_BASE_ADDR + (uint32_t)(p * 256));
        }
        target_p = 6;
    }

    uint32_t addr = FLASH_NVM_BASE_ADDR + (uint32_t)(target_p * 256);
    // Write state using nvm256_write for built-in CRC protection
    return nvm256_write(addr, MAGIC_STA, VER_1, (uint32_t)loaded_ch, nullptr, 0);
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
