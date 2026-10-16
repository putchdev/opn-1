// license:BSD-3-Clause
// copyright-holders:Nicola Salmoria
// The Sega VDP PSG configuration of MAME's src/devices/sound/sn76496.cpp (https://github.com/mamedev/mame), its
// write() and sound_stream_update() bodies kept as written, with the device framework replaced by plain members
// and the volume table computed as device_start() does. For host tests only (tests/psg_compare.cpp).
#pragma once
#include <cstdint>

struct mame_segapsg {
    // segapsg_device: feedbackmask 0x8000, noisetap1 0x01, noisetap2 0x08, negate, mono, divider 8, not ncr, sega
    const int m_feedback_mask = 0x8000, m_whitenoise_tap1 = 0x01, m_whitenoise_tap2 = 0x08;
    const bool m_negate = true, m_stereo = false, m_ncr_style_psg = false, m_sega_style_psg = true;
    const int m_clock_divider = 8;
    int32_t m_vol_table[16], m_register[8], m_last_register, m_volume[4];
    uint32_t m_RNG;
    int32_t m_current_clock, m_stereo_mask, m_period[4], m_count[4], m_output[4];

    static bool BIT(int32_t v, int b) { return (v >> b) & 1; }

    mame_segapsg()                              // device_start()
    {
        static constexpr int MAX_OUTPUT = 0x7fff;
        m_last_register = m_sega_style_psg ? 3 : 0;
        for (int i = 0; i < 8; i += 2) {
            m_register[i] = 0;
            m_register[i + 1] = m_sega_style_psg ? 0xf : 0;
        }
        for (int i = 0; i < 4; i++) {
            m_output[i] = 0;
            m_count[i] = 0;
            m_period[i] = (m_sega_style_psg || i == 3) ? 0 : 0x400;
        }
        m_RNG = m_feedback_mask;
        m_output[3] = m_RNG & 1;
        m_stereo_mask = 0xff;
        m_current_clock = m_clock_divider - 1;
        double out = MAX_OUTPUT / 4;
        for (int i = 0; i < 15; i++) {
            if (out > MAX_OUTPUT / 4) m_vol_table[i] = MAX_OUTPUT / 4;
            else m_vol_table[i] = out;
            out /= 1.258925412;
        }
        m_vol_table[15] = 0;
        for (int i = 0; i < 4; i++) m_volume[i] = m_vol_table[m_register[i * 2 + 1]];
    }

    void write(uint8_t data)
    {
        int r;
        if (data & 0x80) {
            r = (data & 0x70) >> 4;
            m_last_register = r;
            if ((m_ncr_style_psg && (r == 6)) && ((data & 0x04) != (m_register[6] & 0x04)))
                m_RNG = m_feedback_mask;
            m_register[r] = (m_register[r] & 0x3f0) | (data & 0x0f);
        } else {
            r = m_last_register;
        }
        int c = r >> 1;
        switch (r) {
        case 0: case 2: case 4:
            if ((data & 0x80) == 0) m_register[r] = (m_register[r] & 0x0f) | ((data & 0x3f) << 4);
            if ((m_register[r] != 0) || m_sega_style_psg) m_period[c] = m_register[r];
            else m_period[c] = 0x400;
            if (r == 4) {
                if ((m_register[6] & 0x03) == 0x03) m_period[3] = m_period[2] << 1;
            }
            break;
        case 1: case 3: case 5: case 7:
            m_volume[c] = m_vol_table[data & 0x0f];
            if ((data & 0x80) == 0) m_register[r] = (m_register[r] & 0x3f0) | (data & 0x0f);
            break;
        case 6: {
            if ((data & 0x80) == 0) m_register[r] = (m_register[r] & 0x3f0) | (data & 0x0f);
            int n = m_register[6];
            m_period[3] = ((n & 3) == 3) ? (m_period[2] << 1) : (1 << (5 + (n & 3)));
            if (!m_ncr_style_psg) m_RNG = m_feedback_mask;
        } break;
        }
    }

    int16_t sample()                            // one pass of sound_stream_update's loop (mono)
    {
        int16_t out;
        if (m_current_clock > 0) {
            m_current_clock--;
        } else {
            m_current_clock = m_clock_divider - 1;
            for (int i = 0; i < 3; i++) {
                m_count[i]--;
                if (m_count[i] <= 0) {
                    m_output[i] ^= 1;
                    m_count[i] = m_period[i];
                }
            }
            m_count[3]--;
            if (m_count[3] <= 0) {
                if (((m_RNG & m_whitenoise_tap1) != 0) !=
                    (((m_RNG & m_whitenoise_tap2) != (m_ncr_style_psg ? m_whitenoise_tap2 : 0)) && BIT(m_register[6], 2))) {
                    m_RNG >>= 1;
                    m_RNG |= m_feedback_mask;
                } else {
                    m_RNG >>= 1;
                }
                m_output[3] = m_RNG & 1;
                m_count[3] = m_period[3];
            }
        }
        out = ((m_output[0] != 0) ? m_volume[0] : 0) + ((m_output[1] != 0) ? m_volume[1] : 0) +
              ((m_output[2] != 0) ? m_volume[2] : 0) + ((m_output[3] != 0) ? m_volume[3] : 0);
        if (m_negate) out = -out;
        return out;
    }
};
