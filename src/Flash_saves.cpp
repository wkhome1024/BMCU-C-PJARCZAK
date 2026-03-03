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

static inline uint32_t ams_fil_pageA(uint8_t filament_idx)
{
    return FLASH_NVM_AMS_ADDR + (uint32_t)filament_idx * FLASH_NVM256_PAGE_SIZE;
}

static inline uint32_t ams_fil_pageB(uint8_t filament_idx)
{
    return FLASH_NVM_AMS2_ADDR + (uint32_t)filament_idx * FLASH_NVM256_PAGE_SIZE;
}

static bool flash256_prog(uint32_t page_addr, const uint32_t w[64])
{
    if (page_addr & (FLASH_NVM256_PAGE_SIZE - 1u)) return false;

    if (memcmp((const void*)page_addr, (const void*)w, FLASH_NVM256_PAGE_SIZE) == 0) return true;

    const volatile uint32_t* cur = (const volatile uint32_t*)page_addr;
    bool can_prog_no_erase = true;
    for (uint32_t i = 0; i < 64u; i++)
    {
        const uint32_t c = cur[i];
        if ((c & w[i]) != w[i]) { can_prog_no_erase = false; break; }
    }

    const uint32_t irq = irq_save_wch();
    FLASH_Unlock_Fast();
    FLASH_ClearFlag(FLASH_FLAG_BSY | FLASH_FLAG_EOP | FLASH_FLAG_WRPRTERR);

    if (!can_prog_no_erase)
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

    const volatile uint32_t* p = (const volatile uint32_t*)page_addr;
    for (uint32_t i = 0; i < 64u; i++)
        if (p[i] != 0xFFFFFFFFu) return false;

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

// FIL2 log (2 pages per filament, 64B record, CRC last word)
static constexpr uint32_t FIL2_MAGIC = 0x324C4946u; // 'FIL2'
static constexpr uint16_t FIL2_VER   = 0x0002u;

static constexpr uint32_t FIL_REC_BYTES   = 64u;
static constexpr uint32_t FIL_REC_CRC_OFF = 60u; // last word
static constexpr uint32_t FIL_SLOTS_PER_PAGE = (FLASH_NVM256_PAGE_SIZE / FIL_REC_BYTES); // 4
static_assert(FIL_SLOTS_PER_PAGE == 4u, "unexpected slots/page");

struct __attribute__((packed, aligned(4))) FIL2_HDR
{
    uint32_t magic;   // FIL2
    uint16_t ver;     // 2
    uint16_t seq;     // monotonic (wrap ok)
    uint16_t len;     // sizeof(Flash_FilamentInfo)
    uint8_t  fil;     // 0..3
    uint8_t  rsv0;    // 0xFF
    uint32_t rsv1;    // ams num etc
};
static_assert(sizeof(FIL2_HDR) == 16u, "FIL2_HDR size");

static uint8_t  g_fil2_inited = 0;
static uint8_t  g_fil2_last_valid = 0;
static Flash_FilamentInfo g_fil2_last[4];

static uint16_t g_fil2_seq_next[4] = {};
static uint8_t  g_fil2_page_next[4] = {}; // 0=A, 1=B
static uint8_t  g_fil2_slot_next[4] = {}; // 0..3

static uint8_t  g_fil2_latest_have = 0;
static uint8_t  g_fil2_latest_page[4] = {}; // 0/1

static inline uint32_t fil2_page_addr(uint8_t fil, uint8_t page_sel)
{
    return page_sel ? ams_fil_pageB(fil) : ams_fil_pageA(fil);
}

static inline uint32_t fil2_slot_addr(uint8_t fil, uint8_t page_sel, uint8_t slot)
{
    return fil2_page_addr(fil, page_sel) + (uint32_t)slot * FIL_REC_BYTES;
}

static bool flash_region_is_erased(uint32_t addr, uint32_t bytes)
{
    if ((addr & 3u) || (bytes & 3u) || (bytes == 0u)) return false;

    const volatile uint32_t* p = (const volatile uint32_t*)addr;
    const uint32_t words = bytes >> 2;

    for (uint32_t i = 0; i < words; i++)
        if (p[i] != 0xFFFFFFFFu) return false;

    return true;
}

static bool flash_words_prog_std(uint32_t addr, const uint32_t* data, uint32_t words)
{
    if ((addr & 3u) || !data || (words == 0u)) return false;

    const uint32_t irq = irq_save_wch();
    FLASH_Unlock();

    for (uint32_t i = 0; i < words; i++)
    {
        const uint32_t v = data[i];
        if (v == 0xFFFFFFFFu) continue;

        FLASH_ClearFlag(FLASH_FLAG_BSY | FLASH_FLAG_EOP | FLASH_FLAG_WRPRTERR);
        const FLASH_Status st = FLASH_ProgramWord(addr + (i << 2), v);
        if (st != FLASH_COMPLETE) { FLASH_Lock(); irq_restore_wch(irq); return false; }

        if (*(const volatile uint32_t*)(addr + (i << 2)) != v) { FLASH_Lock(); irq_restore_wch(irq); return false; }
    }

    FLASH_Lock();
    irq_restore_wch(irq);
    return true;
}

static inline int16_t seq_delta_u16(uint16_t a, uint16_t b)
{
    return (int16_t)(a - b);
}

static bool fil2_scan_best(uint8_t fil, Flash_FilamentInfo* out, uint16_t* seq_out, uint8_t* page_out, uint8_t* slot_out)
{
    bool have = false;
    uint16_t best_seq = 0;
    uint8_t best_page = 0;
    uint8_t best_slot = 0;

    for (uint8_t page = 0; page < 2u; page++)
    {
        for (uint8_t slot = 0; slot < (uint8_t)FIL_SLOTS_PER_PAGE; slot++)
        {
            const uint32_t a = fil2_slot_addr(fil, page, slot);
            const uint32_t stored_crc = *(const volatile uint32_t*)(a + FIL_REC_CRC_OFF);
            if (stored_crc == 0xFFFFFFFFu) continue;

            const uint32_t crc = crc32_hw_words((const void*)a, FIL_REC_CRC_OFF);
            if (crc != stored_crc) continue;

            FIL2_HDR h{};
            memcpy(&h, (const void*)a, sizeof(h));

            if (h.magic != FIL2_MAGIC) continue;
            if (h.ver   != FIL2_VER)   continue;
            if (h.len   != (uint16_t)sizeof(Flash_FilamentInfo)) continue;
            if (h.fil   != fil) continue;

            if (!have || seq_delta_u16(h.seq, best_seq) > 0)
            {
                have = true;
                best_seq = h.seq;
                best_page = page;
                best_slot = slot;
            }
        }
    }

    if (!have) return false;

    const uint32_t a = fil2_slot_addr(fil, best_page, best_slot);
    memcpy(out, (const void*)(a + sizeof(FIL2_HDR)), sizeof(*out));

    *seq_out = best_seq;
    *page_out = best_page;
    *slot_out = best_slot;
    return true;
}

static bool fil2_rebuild(uint8_t fil)
{
    Flash_FilamentInfo best{};
    uint16_t best_seq = 0;
    uint8_t best_page = 0;
    uint8_t best_slot = 0;

    if (fil2_scan_best(fil, &best, &best_seq, &best_page, &best_slot))
    {
        g_fil2_last[fil] = best;
        g_fil2_last_valid |= (uint8_t)(1u << fil);

        g_fil2_seq_next[fil] = (uint16_t)(best_seq + 1u);

        g_fil2_latest_have |= (uint8_t)(1u << fil);
        g_fil2_latest_page[fil] = best_page;

        uint8_t page = best_page;
        uint8_t slot = (uint8_t)(best_slot + 1u);
        if (slot >= (uint8_t)FIL_SLOTS_PER_PAGE) { slot = 0u; page ^= 1u; }

        g_fil2_page_next[fil] = page;
        g_fil2_slot_next[fil] = slot;
    }
    else
    {
        g_fil2_last_valid &= (uint8_t)~(1u << fil);
        g_fil2_seq_next[fil] = 0u;
        g_fil2_page_next[fil] = 0u;
        g_fil2_slot_next[fil] = 0u;
        g_fil2_latest_have &= (uint8_t)~(1u << fil);
        g_fil2_latest_page[fil] = 0u;
    }

    g_fil2_inited |= (uint8_t)(1u << fil);
    return true;
}

static inline void fil2_advance_ptr(uint8_t fil)
{
    uint8_t slot = (uint8_t)(g_fil2_slot_next[fil] + 1u);
    uint8_t page = g_fil2_page_next[fil];

    if (slot >= (uint8_t)FIL_SLOTS_PER_PAGE)
    {
        slot = 0u;
        page ^= 1u;
    }

    g_fil2_slot_next[fil] = slot;
    g_fil2_page_next[fil] = page;
}

static bool fil2_ensure_writable_slot(uint8_t fil, uint32_t* addr_out, uint8_t* page_out, uint8_t* slot_out)
{
    if (!(g_fil2_inited & (uint8_t)(1u << fil)))
        fil2_rebuild(fil);

    for (uint8_t i = 0; i < (uint8_t)(2u * FIL_SLOTS_PER_PAGE); i++)
    {
        const uint8_t page = g_fil2_page_next[fil];
        const uint8_t slot = g_fil2_slot_next[fil];
        const uint32_t a = fil2_slot_addr(fil, page, slot);

        if (flash_region_is_erased(a, FIL_REC_BYTES))
        {
            *addr_out = a;
            *page_out = page;
            *slot_out = slot;
            return true;
        }

        fil2_advance_ptr(fil);
    }

    uint8_t erase_page = 0u;
    if (g_fil2_latest_have & (uint8_t)(1u << fil))
        erase_page = (uint8_t)(g_fil2_latest_page[fil] ^ 1u);

    const uint32_t paddr = fil2_page_addr(fil, erase_page);
    if (!flash256_erase(paddr)) return false;

    g_fil2_page_next[fil] = erase_page;
    g_fil2_slot_next[fil] = 0u;

    const uint32_t a = fil2_slot_addr(fil, erase_page, 0u);
    if (!flash_region_is_erased(a, FIL_REC_BYTES)) return false;

    *addr_out = a;
    *page_out = erase_page;
    *slot_out = 0u;
    return true;
}

bool Flash_AMS_filament_write(uint8_t filament_idx, const Flash_FilamentInfo* info, uint8_t loaded_ch)
{
    (void)loaded_ch;
    if (!info || filament_idx >= 4) return false;

    const uint8_t mask = (uint8_t)(1u << filament_idx);
    if ((g_fil2_last_valid & mask) && (memcmp(&g_fil2_last[filament_idx], info, sizeof(*info)) == 0))
        return true;

    uint32_t addr = 0;
    uint8_t page = 0;
    uint8_t slot = 0;
    if (!fil2_ensure_writable_slot(filament_idx, &addr, &page, &slot)) return false;

    alignas(4) uint32_t w[FIL_REC_BYTES / 4u];
    uint8_t* b = (uint8_t*)w;
    memset(b, 0xFF, FIL_REC_BYTES);

    FIL2_HDR h{};
    h.magic = FIL2_MAGIC;
    h.ver   = FIL2_VER;
    h.seq   = g_fil2_seq_next[filament_idx];
    h.len   = (uint16_t)sizeof(Flash_FilamentInfo);
    h.fil   = filament_idx;
    h.rsv0  = 0xFFu;
    h.rsv1  = ((uint32_t)BAMBU_BUS_AMS_NUM & 0xFFu);

    memcpy(b + 0, &h, sizeof(h));
    memcpy(b + sizeof(h), info, sizeof(*info));

    const uint32_t crc = crc32_hw_words(b, FIL_REC_CRC_OFF);
    memcpy(b + FIL_REC_CRC_OFF, &crc, 4u);

    if (!flash_region_is_erased(addr, FIL_REC_BYTES)) return false;

    if (!flash_words_prog_std(addr, w, (uint32_t)(FIL_REC_CRC_OFF / 4u))) return false;
    if (!flash_words_prog_std(addr + FIL_REC_CRC_OFF, &w[FIL_REC_CRC_OFF / 4u], 1u)) return false;

    const uint32_t stored_crc = *(const volatile uint32_t*)(addr + FIL_REC_CRC_OFF);
    if (stored_crc != crc) return false;

    const uint32_t crc2 = crc32_hw_words((const void*)addr, FIL_REC_CRC_OFF);
    if (crc2 != stored_crc) return false;

    g_fil2_last[filament_idx] = *info;
    g_fil2_last_valid |= mask;

    g_fil2_seq_next[filament_idx] = (uint16_t)(h.seq + 1u);
    g_fil2_latest_have |= mask;
    g_fil2_latest_page[filament_idx] = page;

    fil2_advance_ptr(filament_idx);
    return true;
}

bool Flash_AMS_filament_read(uint8_t filament_idx, Flash_FilamentInfo* out)
{
    if (!out || filament_idx >= 4) return false;

    Flash_FilamentInfo best{};
    uint16_t best_seq = 0;
    uint8_t best_page = 0;
    uint8_t best_slot = 0;

    if (!fil2_scan_best(filament_idx, &best, &best_seq, &best_page, &best_slot))
        return false;

    *out = best;

    g_fil2_last[filament_idx] = best;
    g_fil2_last_valid |= (uint8_t)(1u << filament_idx);

    g_fil2_seq_next[filament_idx] = (uint16_t)(best_seq + 1u);

    g_fil2_latest_have |= (uint8_t)(1u << filament_idx);
    g_fil2_latest_page[filament_idx] = best_page;

    uint8_t page = best_page;
    uint8_t slot = (uint8_t)(best_slot + 1u);
    if (slot >= (uint8_t)FIL_SLOTS_PER_PAGE) { slot = 0u; page ^= 1u; }

    g_fil2_page_next[filament_idx] = page;
    g_fil2_slot_next[filament_idx] = slot;

    g_fil2_inited |= (uint8_t)(1u << filament_idx);
    return true;
}

bool Flash_AMS_filament_clear(uint8_t filament_idx)
{
    if (filament_idx >= 4) return false;

    const bool a = flash256_erase(ams_fil_pageA(filament_idx));
    const bool b = flash256_erase(ams_fil_pageB(filament_idx));

    const uint8_t mask = (uint8_t)(1u << filament_idx);
    g_fil2_inited &= (uint8_t)~mask;
    g_fil2_last_valid &= (uint8_t)~mask;
    g_fil2_latest_have &= (uint8_t)~mask;

    g_fil2_seq_next[filament_idx] = 0u;
    g_fil2_page_next[filament_idx] = 0u;
    g_fil2_slot_next[filament_idx] = 0u;
    g_fil2_latest_page[filament_idx] = 0u;

    return a && b;
}

// ---- STATE LOG (pages 10..15) ----
static constexpr uint32_t STA_TAG = 0xA5u;
static constexpr uint32_t STA_PAGE_FIRST = 10u;
static constexpr uint32_t STA_PAGE_COUNT = 6u;  // 10..15
static constexpr uint32_t STA_SLOT_BYTES = 8u;  // 2x word
static constexpr uint32_t STA_SLOTS_PER_PAGE = (FLASH_NVM256_PAGE_SIZE / STA_SLOT_BYTES); // 32
static constexpr uint32_t STA_TOTAL_SLOTS = (STA_PAGE_COUNT * STA_SLOTS_PER_PAGE);       // 192

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