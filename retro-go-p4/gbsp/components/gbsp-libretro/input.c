/* gameplaySP
 *
 * Copyright (C) 2006 Exophase <exophase@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "common.h"

bool libretro_supports_bitmasks    = false;
bool libretro_supports_ff_override = false;
bool libretro_ff_enabled           = false;
bool libretro_ff_enabled_prev      = false;

unsigned turbo_period      = TURBO_PERIOD_MIN;
unsigned turbo_pulse_width = TURBO_PULSE_WIDTH_MIN;
unsigned turbo_a_counter   = 0;
unsigned turbo_b_counter   = 0;

static u32 old_key = 0;
static retro_input_state_t input_state_cb;

void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

extern void set_fastforward_override(bool fastforward);

static void trigger_key(u32 key)
{
  u32 p1_cnt = read_ioreg(REG_P1CNT);
#if defined(ESP_PLATFORM) && defined(RG_GBA_INPUT_TRACE) && RG_GBA_INPUT_TRACE
  /* 真机诊断：按键边沿到达时记录 KEYCNT 与 CPU/IRQ 状态，判断是否依赖键中断唤醒。 */
  RG_LOGW("GBA_KEYIRQ f=%u keys=%03X P1CNT=%04X PC=%08X HALT=%u IF=%04X IE=%04X IME=%04X\n",
          (unsigned)frame_counter, (unsigned)key, (unsigned)p1_cnt,
          (unsigned)reg[REG_PC], (unsigned)reg[CPU_HALT_STATE],
          (unsigned)read_ioreg(REG_IF), (unsigned)read_ioreg(REG_IE),
          (unsigned)read_ioreg(REG_IME));
#endif

  if((p1_cnt >> 14) & 0x01)
  {
    u32 key_intersection = (p1_cnt & key) & 0x3FF;

    if(p1_cnt >> 15)
    {
      if(key_intersection == (p1_cnt & 0x3FF))
      {
        flag_interrupt(IRQ_KEYPAD);
        check_and_raise_interrupts();
        RG_TRACE("f=%u TRIGGER p1cnt=%04X key=%03X AND-hit -> IRQ_KEYPAD\n", (unsigned)frame_counter, (unsigned)p1_cnt, (unsigned)key);
      }
      else
        RG_TRACE("f=%u TRIGGER p1cnt=%04X key=%03X AND-miss\n", (unsigned)frame_counter, (unsigned)p1_cnt, (unsigned)key);
    }
    else
    {
      if(key_intersection)
      {
        flag_interrupt(IRQ_KEYPAD);
        check_and_raise_interrupts();
        RG_TRACE("f=%u TRIGGER p1cnt=%04X key=%03X OR-hit -> IRQ_KEYPAD\n", (unsigned)frame_counter, (unsigned)p1_cnt, (unsigned)key);
      }
      else
        RG_TRACE("f=%u TRIGGER p1cnt=%04X key=%03X OR-miss\n", (unsigned)frame_counter, (unsigned)p1_cnt, (unsigned)key);
    }
  }
  else
  {
    /* 关键分支：bit15(使能) 清了就整段不认 —— 手册里 bit15=1 才是"键中断使能"。
     * 游戏写 0x8001 这种最普通的"任意键唤醒"，这里会走到这一行。 */
    RG_TRACE("f=%u TRIGGER p1cnt=%04X key=%03X IGNORED (bit15=0? maybe polarity bug)\n", (unsigned)frame_counter, (unsigned)p1_cnt, (unsigned)key);
  }
}

u32 update_input(void)
{
   unsigned i;
   uint32_t new_key = 0;
   bool turbo_a     = false;
   bool turbo_b     = false;

   if (!input_state_cb)
      return 0;

   if (libretro_supports_bitmasks)
   {
      int16_t ret = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);

      for (i = 0; i < sizeof(btn_map) / sizeof(map); i++)
         new_key |= (ret & (1 << btn_map[i].retropad)) ? btn_map[i].gba : 0;

      libretro_ff_enabled = libretro_supports_ff_override &&
            (ret & (1 << RETRO_DEVICE_ID_JOYPAD_R2));

      turbo_a = (ret & (1 << RETRO_DEVICE_ID_JOYPAD_X));
      turbo_b = (ret & (1 << RETRO_DEVICE_ID_JOYPAD_Y));
   }
   else
   {
      for (i = 0; i < sizeof(btn_map) / sizeof(map); i++)
         new_key |= input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, btn_map[i].retropad) ? btn_map[i].gba : 0;

       libretro_ff_enabled = libretro_supports_ff_override &&
            input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2);

      turbo_a = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X);
      turbo_b = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y);
   }

   /* Handle turbo buttons */
   if (turbo_a)
   {
      new_key |= (turbo_a_counter < turbo_pulse_width) ?
            BUTTON_A : 0;

      turbo_a_counter++;
      if (turbo_a_counter >= turbo_period)
         turbo_a_counter = 0;
   }
   else
      turbo_a_counter = 0;

   if (turbo_b)
   {
      new_key |= (turbo_b_counter < turbo_pulse_width) ?
            BUTTON_B : 0;

      turbo_b_counter++;
      if (turbo_b_counter >= turbo_period)
         turbo_b_counter = 0;
   }
   else
      turbo_b_counter = 0;

   // GBP keypad detection hack (only at game startup!)
   if (serial_mode == SERIAL_MODE_GBP) {
     // During the startup screen (aproximate)
     if (frame_counter > 20 && frame_counter < 100) {
       // Emulate 4 keypad buttons pressed (which is impossible).
       new_key = (frame_counter % 3) ? 0x3FF : 0x30F;
     }
   }

   if ((new_key | old_key) != old_key)
      trigger_key(new_key);

   old_key = new_key;
   /* KEYINPUT bit10~15 在 GBATEK 中标为 Not used；上游 gpSP/mGBA 只模拟低 10 位。
    * 当前低位写法是与 `| 0xFC00` 的单变量 A/B 诊断版；菜单是否受影响以同 ROM 真机结果为准。 */
   write_ioreg(REG_P1, (~old_key) & 0x3FF);

#if defined(ESP_PLATFORM) && defined(RG_GBA_INPUT_TRACE) && RG_GBA_INPUT_TRACE
   /* 真机一次性诊断：边沿时记录核心最终收到的 GBA 键位与 P1 寄存器值。
    * 两份相同 WARN 行抗 CDC 丢行；首次触发后压低其它日志噪声。 */
   {
      static u32 last_trace_key = 0xFFFFFFFF;
      static bool trace_quieted = false;
      if (new_key != last_trace_key)
      {
         last_trace_key = new_key;
         if (!trace_quieted)
         {
            trace_quieted = true;
            rg_system_set_log_level(RG_LOG_WARN);
         }
         for (int copy = 0; copy < 2; ++copy)
            RG_LOGW("GBA_INPUT f=%u keys=%03X P1=%04X PC=%08X HALT=%u P1CNT=%04X IF=%04X IE=%04X IME=%04X\n",
                    (unsigned)frame_counter, (unsigned)new_key,
                    (unsigned)read_ioreg(REG_P1), (unsigned)reg[REG_PC],
                    (unsigned)reg[CPU_HALT_STATE], (unsigned)read_ioreg(REG_P1CNT),
                    (unsigned)read_ioreg(REG_IF), (unsigned)read_ioreg(REG_IE),
                    (unsigned)read_ioreg(REG_IME));
      }
      static u32 alive = 0;
      if ((alive++ % 120) == 0)
         RG_LOGW("GBA_ALIVE f=%u PC=%08X HALT=%u P1=%04X P1CNT=%04X IF=%04X IE=%04X IME=%04X\n",
                 (unsigned)frame_counter, (unsigned)reg[REG_PC],
                 (unsigned)reg[CPU_HALT_STATE], (unsigned)read_ioreg(REG_P1),
                 (unsigned)read_ioreg(REG_P1CNT), (unsigned)read_ioreg(REG_IF),
                 (unsigned)read_ioreg(REG_IE), (unsigned)read_ioreg(REG_IME));
   }
#endif

   /* 探针：①我们的输入层有没有把键送进核心 ②每 120 帧打一次"还活着吗"（PC/halt/寄存器）
    * —— "卡在 HALT 等中断"在画面上和"没送键"长得一模一样，只有这两条 trace 能分开。 */
   {
     static u32 last_input = 0xFFFFFFFF, alive = 0;
     if (new_key != last_input)
     {
       RG_TRACE("f=%u INPUT new=%03X old=%03X\n", (unsigned)frame_counter, (unsigned)new_key, (unsigned)old_key);
       last_input = new_key;
     }
     if ((alive++ % 120) == 0)
       RG_TRACE("f=%u ALIVE pc=%08X halt=%u p1=%03X p1cnt=%04X key=%03X\n", (unsigned)frame_counter,
                (unsigned)reg[15], (unsigned)reg[CPU_HALT_STATE], (unsigned)read_ioreg(REG_P1),
                (unsigned)read_ioreg(REG_P1CNT), (unsigned)new_key);
   }

   /* Handle fast forward button */
   if (libretro_ff_enabled != libretro_ff_enabled_prev)
   {
      set_fastforward_override(libretro_ff_enabled);
      libretro_ff_enabled_prev = libretro_ff_enabled;
   }

   return 0;
}

bool input_check_savestate(const u8 *src)
{
  const u8 *p = bson_find_key(src, "input");
  return (p && bson_contains_key(p, "prevkey", BSON_TYPE_INT32));
}

bool input_read_savestate(const u8 *src)
{
  const u8 *p = bson_find_key(src, "input");
  if (p)
    return bson_read_int32(p, "prevkey", &old_key);
  return false;
}

unsigned input_write_savestate(u8 *dst)
{
  u8 *wbptr1, *startp = dst;
  bson_start_document(dst, "input", wbptr1);
  bson_write_int32(dst, "prevkey", old_key);
  bson_finish_document(dst, wbptr1);
  return (unsigned int)(dst - startp);
}


