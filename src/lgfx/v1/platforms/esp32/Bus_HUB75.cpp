/*----------------------------------------------------------------------------/
  Lovyan GFX - Graphics library for embedded devices.

Original Source:
 https://github.com/lovyan03/LovyanGFX/

Licence:
 [FreeBSD](https://github.com/lovyan03/LovyanGFX/blob/master/license.txt)

Author:
 [lovyan03](https://twitter.com/lovyan03)

Contributors:
 [ciniml](https://github.com/ciniml)
 [mongonta0716](https://github.com/mongonta0716)
 [tobozo](https://github.com/tobozo)
/----------------------------------------------------------------------------*/
#if defined (ESP_PLATFORM)
#include <sdkconfig.h>
#if defined (CONFIG_IDF_TARGET_ESP32)

#include "Bus_HUB75.hpp"
#include "../../misc/pixelcopy.hpp"

#include <soc/dport_reg.h>
#include <rom/gpio.h>
#include <esp_log.h>

namespace lgfx
{
 inline namespace v1
 {

  uint8_t* Bus_HUB75::_gamma_tbl = nullptr;
  uint8_t* Bus_HUB75::_bitinvert_tbl = nullptr;

//----------------------------------------------------------------------------

  static constexpr uint32_t _conf_reg_default = I2S_TX_MSB_RIGHT | I2S_TX_RIGHT_FIRST | I2S_RX_RIGHT_FIRST | I2S_TX_MONO;
  static constexpr uint32_t _conf_reg_start   = _conf_reg_default | I2S_TX_START;
  static constexpr uint32_t _conf_reg_reset   = _conf_reg_default | I2S_TX_RESET;
  static constexpr uint32_t _sample_rate_conf_reg_direct = 16 << I2S_TX_BITS_MOD_S | 16 << I2S_RX_BITS_MOD_S | 1 << I2S_TX_BCK_DIV_NUM_S | 1 << I2S_RX_BCK_DIV_NUM_S;
  static constexpr uint32_t _fifo_conf_default = 1 << I2S_TX_FIFO_MOD | 1 << I2S_RX_FIFO_MOD | 16 << I2S_TX_DATA_NUM_S | 16 << I2S_RX_DATA_NUM_S;
  static constexpr uint32_t _fifo_conf_dma     = _fifo_conf_default | I2S_DSCR_EN;

  static __attribute__ ((always_inline)) inline volatile uint32_t* reg(uint32_t addr) { return (volatile uint32_t *)ETS_UNCACHED_ADDR(addr); }

  __attribute__((always_inline))
  static inline i2s_dev_t* getDev(i2s_port_t port)
  {
#if defined (CONFIG_IDF_TARGET_ESP32S2)
    return &I2S0;
#else
    return (port == 0) ? &I2S0 : &I2S1;
#endif
  }

  void Bus_HUB75::config(const config_t& cfg)
  {
    _cfg = cfg;
    _dev = getDev(cfg.i2s_port);
  }

  bool Bus_HUB75::init(void)
  {
    auto idx_base = (_cfg.i2s_port == I2S_NUM_0) ? I2S0O_DATA_OUT8_IDX : I2S1O_DATA_OUT8_IDX;

    for (size_t i = 0; i < 14; ++i)
    {
      if (_cfg.pin_data[i] < 0) continue;
      gpio_pad_select_gpio(_cfg.pin_data[i]);
      gpio_matrix_out(_cfg.pin_data[i  ], idx_base + i, 0, 0);
    }

    idx_base = (_cfg.i2s_port == I2S_NUM_0) ? I2S0O_WS_OUT_IDX : I2S1O_WS_OUT_IDX;
    gpio_matrix_out(_cfg.pin_clk, idx_base, 1, 0); // clock Active-low

    uint32_t dport_clk_en;
    uint32_t dport_rst;

    int intr_source = ETS_I2S0_INTR_SOURCE;
    if (_cfg.i2s_port == I2S_NUM_0) {
      idx_base = I2S0O_WS_OUT_IDX;
      dport_clk_en = DPORT_I2S0_CLK_EN;
      dport_rst = DPORT_I2S0_RST;
    }
#if !defined (CONFIG_IDF_TARGET_ESP32S2)
    else
    {
      intr_source = ETS_I2S1_INTR_SOURCE;
      idx_base = I2S1O_WS_OUT_IDX;
      dport_clk_en = DPORT_I2S1_CLK_EN;
      dport_rst = DPORT_I2S1_RST;
    }
#endif

    DPORT_SET_PERI_REG_MASK(DPORT_PERIP_CLK_EN_REG, dport_clk_en);
    DPORT_CLEAR_PERI_REG_MASK(DPORT_PERIP_RST_EN_REG, dport_rst);

    auto i2s_dev = (i2s_dev_t*)_dev;
    //Reset I2S subsystem
    i2s_dev->conf.val = I2S_TX_RESET | I2S_RX_RESET | I2S_TX_FIFO_RESET | I2S_RX_FIFO_RESET;
    i2s_dev->conf.val = _conf_reg_default;

    i2s_dev->timing.val = 0;

    //Reset DMA
    i2s_dev->lc_conf.val = I2S_IN_RST | I2S_OUT_RST | I2S_AHBM_RST | I2S_AHBM_FIFO_RST;
    i2s_dev->lc_conf.val = I2S_OUT_EOF_MODE | I2S_OUTDSCR_BURST_EN | I2S_OUT_DATA_BURST_EN;

    i2s_dev->in_link.val = 0;
    i2s_dev->out_link.val = 0;

    i2s_dev->conf1.val = I2S_TX_PCM_BYPASS;
    i2s_dev->conf2.val = I2S_LCD_EN;
    i2s_dev->conf_chan.val = 1 << I2S_TX_CHAN_MOD_S | 1 << I2S_RX_CHAN_MOD_S;

    i2s_dev->int_ena.val = 0;
    i2s_dev->int_clr.val = ~0u;
    i2s_dev->int_ena.out_eof = 1;

/* DMAディスクリプタリストの各役割、 各行先頭がデータ転送期間、２列目以降が拡張点灯期間 
  ↓転送期間 ↓拡張点灯期間 
  [11] ↲
  [12] ↲
  [13] ↲
  [14] ↲
  [15] ↲
  [16] → 0 ↲
  [17] → 1→ 2→ 3 ↲
  [18] → 4→ 5→ 6→ 7→ 8→ 9→10↲(EOF,次ライン)
   色深度8を再現するために、各ビットに対応した点灯を行うため同一ラインに8回データを送る。
   8回の点灯期間は、1回進む毎に点灯期間が前回の2倍になる。
   後半の点灯期間がとても長くなるため、データ転送をせず点灯のみを行う拡張点灯期間を設ける。
   全ての拡張点灯期間はメモリ上の同一地点を利用しメモリを節約している。


  [7] → 8→ 9→10→11→12→13→14 ↲
  [6] →15→16→17 ↲
  [5] →18 ↲
  [4] ↲
  [3] ↲
  [2] ↲
  [1] ↲
  [0] ↲(EOF,次ライン)
*/
    static constexpr const uint8_t dma_link_idx_tbl[] = {
      // 17, 2, 3, 18, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 0, 1, 4

      7, 0, 1, 2, 3, 18, 15, 8, 9, 10, 11, 12, 13, 14, 6, 16, 17, 5, 4,
    };

    // (データ転送期間8回 + 拡張点灯期間11回 = 19) * 2ライン分
    if (_dmadesc) heap_caps_free(_dmadesc);
    _dmadesc = (lldesc_t*)heap_caps_malloc(sizeof(lldesc_t) * TOTAL_PERIOD_COUNT * 2, MALLOC_CAP_DMA);

    uint32_t panel_width = _panel_width;

    size_t buf_bytes = panel_width * (TRANSFER_PERIOD_COUNT + 1) * sizeof(uint16_t);
    for (int i = 0; i < 2; i++) {
      // ラインバッファ確保。 点灯期間1回分 + データ転送期間 8回分の合計9回分を連続領域として確保する
      // 拡張点灯期間は合計11回あるが、同じ領域を使い回すためバッファは1回分でよい;
      _dma_buf[i] = (uint16_t*)heap_alloc_dma(buf_bytes);
      if (_dma_buf[i] == nullptr) {
        ESP_EARLY_LOGE("Bus_HUB75", "memory allocate error.");
      }

      memset(_dma_buf[i], 0x01, buf_bytes); // OE(消灯)で埋める

      // ディスクリプタリストの先頭に点灯期間19回分のDMA情報を纏めて配置する
      for (int j = 0; j < TOTAL_PERIOD_COUNT; j++) {
        uint32_t idx = i * TOTAL_PERIOD_COUNT + j;
        int bufidx = panel_width * (j < TRANSFER_PERIOD_COUNT ? j : TRANSFER_PERIOD_COUNT);
        bool eof = j == 0;
        _dmadesc[idx].buf = (volatile uint8_t*)&(_dma_buf[i][bufidx]);
        _dmadesc[idx].eof = eof; // 最後の拡張点灯期間のみEOFイベントを発生させる
        _dmadesc[idx].empty = (uint32_t)(&_dmadesc[dma_link_idx_tbl[j] + (i ^ eof) * TOTAL_PERIOD_COUNT]);
        _dmadesc[idx].owner = 1;
        _dmadesc[idx].length = panel_width * sizeof(uint16_t);
        _dmadesc[idx].size = panel_width * sizeof(uint16_t);
      }
    }
    setBrightness(_brightness);
    if (esp_intr_alloc(intr_source, ESP_INTR_FLAG_LEVEL1 | ESP_INTR_FLAG_IRAM,
        i2s_intr_handler_hub75, this, &_isr_handle) != ESP_OK) {
      ESP_EARLY_LOGE("Bus_HUB75","esp_intr_alloc failure ");
      return false;
    }
    ESP_EARLY_LOGV("Bus_HUB75","esp_intr_alloc success ");


    if (_gamma_tbl == nullptr)
    {
      static constexpr const uint8_t gamma_tbl[] =
      {
          0,   1,   2,   3,   4,   5,   6,   7,
          8,  10,  12,  13,  15,  17,  19,  21,
         24,  26,  29,  31,  34,  37,  40,  43,
         46,  49,  53,  56,  60,  64,  67,  71,
         75,  80,  84,  88,  93,  97, 102, 107,
        112, 117, 122, 127, 133, 138, 144, 149,
        155, 161, 167, 173, 179, 186, 192, 199,
        205, 212, 219, 226, 233, 241, 248, 255
      };
      _gamma_tbl = (uint8_t*)heap_alloc_dma(sizeof(gamma_tbl));
      memcpy(_gamma_tbl, gamma_tbl, sizeof(gamma_tbl));
    }
    if (_bitinvert_tbl == nullptr)
    {
      static constexpr const uint8_t bitinvert_tbl[] = { 0, 16, 8, 24, 4, 20, 12, 28, 2, 18, 10, 26, 6, 22, 14, 30, 1, 17, 9, 25, 5, 21, 13, 29, 3, 19, 11, 27, 7, 23, 15, 31 };
      _bitinvert_tbl = (uint8_t*)heap_alloc_dma(sizeof(bitinvert_tbl));
      memcpy(_bitinvert_tbl, bitinvert_tbl, sizeof(bitinvert_tbl));
    }

    return true;
  }

  __attribute__((always_inline))
  static inline uint32_t _gcd(uint32_t a, uint32_t b)
  {
    uint32_t c = a % b;
    while (c != 0) {
      a = b;
      b = c;
      c = a % b;
    }
    return b;
  }

  static uint32_t getClockDivValue(uint32_t baseClock, uint32_t targetFreq)
  {
    uint32_t n = baseClock / targetFreq;
    if (n > 255) { n = 255; }
    uint32_t a = 1;
    uint32_t b = 0;

    // div_nが小さい場合に小数成分を含めると誤動作するのでここ除外する
    if (n > 4)
    {
      uint32_t delta_hz = baseClock - targetFreq * n;
      if (delta_hz) {
        uint32_t gcd = _gcd(targetFreq, delta_hz);
        a = targetFreq / gcd;
        b = delta_hz / gcd;
        uint32_t d = a / 63 + 1;
        a /= d;
        b /= d;
      }
    }

    return       I2S_CLK_EN
          | a << I2S_CLKM_DIV_A_S
          | b << I2S_CLKM_DIV_B_S
          | n << I2S_CLKM_DIV_NUM_S
          ;
  }

  void Bus_HUB75::setBrightness(uint8_t brightness)
  {
    _brightness = brightness;
    int br = brightness + 1;
    br = (br * br);
    auto panel_width = _panel_width;
    uint32_t light_len_limit = (panel_width - 8) >> 1;
    uint32_t slen = (light_len_limit * br) >> 16;

    for (int period = TRANSFER_PERIOD_COUNT - 1; period >= 0; --period)
    {
      if (period <= 3) { slen >>= 1; }
      _light_period[period] = slen + 2; // 末尾2サイクルがLAT+OEとなるので+1する
    }
  }

  void Bus_HUB75::release(void)
  {
    endTransaction();
    if (_dmadesc)
    {
      heap_caps_free(_dmadesc);
      _dmadesc = nullptr;
    }
    for (int i = 0; i < 2; i++) {
      if (_dma_buf[i])
      {
        heap_free(_dma_buf[i]);
        _dma_buf[i] = nullptr;
      }
    }
  }

  void Bus_HUB75::beginTransaction(void)
  {
    auto i2s_dev = (i2s_dev_t*)_dev;
    i2s_dev->out_link.val = 0;
    i2s_dev->fifo_conf.val = _fifo_conf_dma;
    i2s_dev->sample_rate_conf.val = _sample_rate_conf_reg_direct;

    // 総転送データ量とリフレッシュレートに基づいて送信クロックを設定する
    uint32_t pll_d2_clock = 80 * 1000 * 1000;
    uint32_t freq_write = (TOTAL_PERIOD_COUNT * _panel_width * _panel_height * _cfg.refresh_rate) >> 1;
    i2s_dev->clkm_conf.val = getClockDivValue(pll_d2_clock, freq_write);

    esp_intr_enable(_isr_handle);
    i2s_dev->int_clr.val = ~0u;

    i2s_dev->conf.val = _conf_reg_reset;
    i2s_dev->out_link.val = I2S_OUTLINK_START | ((uint32_t)&_dmadesc[0] & I2S_OUTLINK_ADDR);
    i2s_dev->conf.val = _conf_reg_start;
  }

  void Bus_HUB75::endTransaction(void)
  {
    esp_intr_disable(_isr_handle);

    auto i2s_dev = (i2s_dev_t*)_dev;
    i2s_dev->out_link.stop = 1;
    i2s_dev->conf.val = _conf_reg_reset;
    i2s_dev->out_link.val = 0;

    i2s_dev->int_clr.val = ~0u;
  }

  void IRAM_ATTR Bus_HUB75::i2s_intr_handler_hub75(void *arg)
  {
    auto me = (Bus_HUB75*)arg;
    auto dev = getDev(me->_cfg.i2s_port);
    auto st = dev->int_st.val;
    bool flg_eof = st & I2S_OUT_EOF_INT_ST;
    dev->int_clr.val = st;
    if (!flg_eof) { return; }

// DEBUG
lgfx::gpio_hi(15);

    int yidx = me->_dma_y;
    auto panel_height = me->_panel_height;

    uint_fast8_t prev_y = _bitinvert_tbl[yidx];
    yidx = (yidx + 1) & ((panel_height>>1) - 1);
    me->_dma_y = yidx;
    uint_fast8_t y = _bitinvert_tbl[yidx];

    if (panel_height <= 32)
    {
      prev_y >>= 1;
      y >>= 1;
    }

    uint32_t prev_addr = prev_y << 9 | prev_y << 25;  // | _mask_oe;
    uint32_t addr = y << 9 | y << 25;  // | _mask_oe;

    auto desc = (lldesc_t*)dev->out_eof_des_addr;
    auto d32 = (uint32_t*)desc->buf;

    auto panel_width = me->_panel_width;
    const uint32_t len32 = panel_width >> 1;

    uint16_t* light_period = me->_light_period;
    light_period[TRANSFER_PERIOD_COUNT] = len32;

    auto src1 = &((uint16_t*)(me->_frame_buffer))[y * panel_width];
    auto src2 = &src1[panel_width * (panel_height>>1)];

    auto s1 = (uint32_t*)src1;
    auto s2 = (uint32_t*)src2;
    // d32 += len32;
    uint32_t x = 0;
    uint32_t addrs[TRANSFER_PERIOD_COUNT] = { addr, addr, addr, addr, addr, addr, addr, prev_addr };
    int light_idx = 0;
    for (;;)
    {
      uint32_t swap565x2_1 = *s1++;
      uint32_t swap565x2_2 = *s2++;

      uint32_t r1 = swap565x2_1 >> 2;
      uint32_t r2 = swap565x2_2 >> 2;

      uint32_t g1 = swap565x2_1 & 0x070007;
      uint32_t g2 = swap565x2_2 & 0x070007;

      uint32_t b1 = r1 >> 5;
      uint32_t b2 = r2 >> 5;

      r1 &= 0x3E003E;
      r2 &= 0x3E003E;

      uint32_t g3 = b1 >> 6;
      uint32_t g4 = b2 >> 6;

      b1 &= 0x3E003E;
      b2 &= 0x3E003E;

      r1 += r1 >> 5;
      r2 += r2 >> 5;

      g3 &= 0x070007;
      g4 &= 0x070007;

      b1 += b1 >> 5;
      b2 += b2 >> 5;

      uint32_t r3 = r1 >> 16;
      uint32_t r4 = r2 >> 16;

      g1 = (g1 << 3) + g3;
      g2 = (g2 << 3) + g4;

      r1 &= 0x3F;
      r2 &= 0x3F;

      uint32_t b3 = b1 >> 16;
      uint32_t b4 = b2 >> 16;

      b1 &= 0x3F;
      b2 &= 0x3F;

      g3 = g1 >> 16;
      g4 = g2 >> 16;

      g1 &= 0x3F;
      g2 &= 0x3F;

      b1 = _gamma_tbl[b1];
      b2 = _gamma_tbl[b2];
      b3 = _gamma_tbl[b3];
      b4 = _gamma_tbl[b4];
      g1 = _gamma_tbl[g1];
      g2 = _gamma_tbl[g2];
      g3 = _gamma_tbl[g3];
      g4 = _gamma_tbl[g4];
      r1 = _gamma_tbl[r1];
      r2 = _gamma_tbl[r2];
      r3 = _gamma_tbl[r3];
      r4 = _gamma_tbl[r4];

      uint32_t bbbb = (b1 << 16) + (b2 << 24) + b3 + (b4 <<  8);
      uint32_t gggg = (g1 << 16) + (g2 << 24) + g3 + (g4 <<  8);
      uint32_t rrrr = (r1 << 16) + (r2 << 24) + r3 + (r4 <<  8);

      int32_t i = 0;
      uint32_t mask = 0x01010101u;
      do
      {
        uint32_t b = bbbb & mask;
        uint32_t g = gggg & mask;
        uint32_t r = rrrr & mask;
        b >>= i;
        g >>= i;
        r >>= i;
        uint32_t rgb = r + (g << 1) + (b << 2);
        mask <<= 1;
        rgb += (rgb >> 5);
        d32[i * len32] = addrs[i] | (rgb & 0x3F003F);
      } while (++i != TRANSFER_PERIOD_COUNT);
      ++d32;

      if (++x < light_period[light_idx]) { continue; }
      if (light_idx == TRANSFER_PERIOD_COUNT) { break; }
      if (light_idx == 4)
      {
        addrs[TRANSFER_PERIOD_COUNT - 1] = addr | _mask_oe;
      }
      do {
        addrs[(light_idx - 1) & (TRANSFER_PERIOD_COUNT - 1)] |= _mask_oe;
      } while (x >= light_period[++light_idx]);
    }

    for (int i = 0; i < TRANSFER_PERIOD_COUNT; ++i)
    { // 各転送期間の末尾にLATを指定する
      d32[i * len32 - 1] |= _mask_lat | _mask_oe;
    }

    d32 += len32 * (TRANSFER_PERIOD_COUNT - 1);

    addr |= _mask_oe;
    // 点灯期間のYアドレス設定
    d32[len32 - 1] = addr;
    memset(d32    , addr >> 8, sizeof(uint32_t) * (len32 - light_period[5]));
    addr &= ~_mask_oe;
    memset(&d32[len32 - light_period[5]], y<<1  , sizeof(uint32_t) * (light_period[5]-1));

// DEBUG
lgfx::gpio_lo(15);
  }
//*/

//----------------------------------------------------------------------------
 }
}

#endif
#endif
