/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* Render UI screens into a framebuffer and dump raw RGB565 files for before/after comparison */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pa_config.h"
#include "pa_fx.h"
#include "pa_ui.h"
#include "pa_preset.h"
extern pa_fx_chain_t g_fx_chain;
void pa_firmware_init(void);
void pa_firmware_tick(uint32_t buttons, int32_t enc_delta, const uint16_t adc_pots[4]);
static uint16_t fb[240*240];
static uint32_t s_ms = 1000;
uint32_t plat_ms(void){ return s_ms++; }
uint32_t plat_ticks24(void){return 0;}
void st7735s_init(void){}
void st7735s_fill_rect(uint16_t x,uint16_t y,uint16_t w,uint16_t h,uint16_t c){
  for(int j=y;j<y+h&&j<240;j++)for(int i=x;i<x+w&&i<240;i++)fb[j*240+i]=c;}
static void dump(const char*n){FILE*f=fopen(n,"wb");fwrite(fb,2,240*240,f);fclose(f);}
static const uint16_t pots[4]={2048,2048,2048,2048};
static void tick(uint32_t b,int32_t e){ s_ms+=20; pa_firmware_tick(b,e,pots); }
int main(){
  pa_firmware_init();
  tick(0,0); tick(0,0);
  tick(0,+5); tick(0,0); dump("home_P13.raw");                 /* preset 8 -> 13 (P1x '1' glyph) */
  tick(PA_BTN_PARA_CLICK,0); tick(0,0); dump("editor_entry.raw");  /* hover on model? */
  tick(PA_BTN_VOL_MINUS,0); tick(0,0);
  tick(PA_BTN_PARA_HOLD1,0); tick(PA_BTN_PARA_CLICK,0); tick(0,0); dump("saved_toast.raw");
  for(int i=0;i<80;i++) tick(0,0);
  tick(PA_BTN_HOME,0); tick(0,0); dump("settings.raw");
  tick(PA_BTN_PARA_CLICK,0); tick(0,0); dump("io.raw");
  tick(PA_BTN_VOL_MINUS,0); tick(PA_BTN_VOL_MINUS,0); tick(0,0);
  tick(PA_COMBO_TUNER,0); tick(0,0); dump("tuner.raw");
  tick(PA_BTN_HOME,0); tick(0,0); dump("home_after_tuner.raw");
  return 0;
}
