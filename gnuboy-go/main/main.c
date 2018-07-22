#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/i2s.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "esp_task_wdt.h"
#include "esp_spiffs.h"
#include "driver/rtc_io.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"

#include "../components/gnuboy/loader.h"
#include "../components/gnuboy/hw.h"
#include "../components/gnuboy/lcd.h"
#include "../components/gnuboy/fb.h"
#include "../components/gnuboy/cpu.h"
#include "../components/gnuboy/mem.h"
#include "../components/gnuboy/sound.h"
#include "../components/gnuboy/pcm.h"
#include "../components/gnuboy/regs.h"
#include "../components/gnuboy/rtc.h"
#include "../components/gnuboy/gnuboy.h"

#include <string.h>

#include "hourglass_empty_black_48dp.h"

#include "../components/odroid/odroid_settings.h"
#include "../components/odroid/odroid_input.h"
#include "../components/odroid/odroid_display.h"
#include "../components/odroid/odroid_audio.h"
#include "../components/odroid/odroid_system.h"
#include "../components/odroid/odroid_sdcard.h"

// #include "./adafruit_gfx.h"
#include "./adafruit_gfx.c"

// #include "mruby.h"
// #include "mruby/irep.h"
// #include "mruby/compile.h"
// #include "mruby/error.h"
// #include "mruby/string.h"

extern int debug_trace;

struct fb fb;
struct pcm pcm;


uint16_t* displayBuffer[2]; //= { fb0, fb0 }; //[160 * 144];
uint8_t currentBuffer;

uint16_t* framebuffer;
int frame = 0;
uint elapsedTime = 0;

int32_t* audioBuffer[2];
volatile uint8_t currentAudioBuffer = 0;
volatile uint16_t currentAudioSampleCount;
volatile int16_t* currentAudioBufferPtr;

odroid_battery_state battery_state;

const char* StateFileName = "/storage/gnuboy.sav";

#define GAMEBOY_WIDTH (160)
#define GAMEBOY_HEIGHT (144)

#define AUDIO_SAMPLE_RATE (32000)

const char* SD_BASE_PATH = "/sd";

// make CONFIG_PYTHON=python3
// python3 /Users/barry/Sites/esp-idf/components/esptool_py/esptool/esptool.py --chip esp32 --port /dev/cu.SLAB_USBtoUART --baud 921600 write_flash -fs detect --flash_freq 40m --flash_mode qio 0x300000 /Users/barry/Sites/go-play/gnuboy-go/build/gnuboy-go.bin
// --- ADDITIONS


static void SaveState()
{
    // Save sram
    odroid_input_battery_monitor_enabled_set(0);
    odroid_system_led_set(1);

    char* romPath = odroid_settings_RomFilePath_get();
    if (romPath)
    {
        char* fileName = odroid_util_GetFileName(romPath);
        if (!fileName) abort();

        char fileNameTagged[512];
        sprintf(fileNameTagged,"%s.latest.gbc",fileName);

        char* pathName = odroid_sdcard_create_savefile_path(SD_BASE_PATH, fileNameTagged);
        if (!pathName) abort();

        FILE* f = fopen(pathName, "w");
        if (f == NULL)
        {
            printf("%s: fopen save failed\n", __func__);
            abort();
        }

        savestate(f);
        fclose(f);

        printf("%s: savestate OK.\n", __func__);

        free(pathName);
        free(fileName);
        free(romPath);
    }
    else
    {
        FILE* f = fopen(StateFileName, "w");
        if (f == NULL)
        {
            printf("SaveState: fopen save failed\n");
        }
        else
        {
            savestate(f);
            fclose(f);

            printf("SaveState: savestate OK.\n");
        }
    }


    odroid_system_led_set(0);
    odroid_input_battery_monitor_enabled_set(1);
}

static void SaveStateRtc()
{
    // Save sram
    odroid_input_battery_monitor_enabled_set(0);
    odroid_system_led_set(1);

    char* romPath = odroid_settings_RomFilePath_get();
    if (romPath)
    {
        char* fileName = odroid_util_GetFileName(romPath);
        if (!fileName) abort();

        char fileNameTagged[64];
        sprintf(fileNameTagged,"%s.%03dd%02dh%02dm%02ds.gbc",fileName,rtc.d,rtc.h,rtc.m,rtc.s);

        char* pathName = odroid_sdcard_create_savefile_path(SD_BASE_PATH, fileNameTagged);
        if (!pathName) abort();

        FILE* f = fopen(pathName, "w");
        if (f == NULL)
        {
            printf("%s: fopen save failed\n", __func__);
            abort();
        }

        savestate(f);
        fclose(f);

        printf("%s: savestate OK.\n", __func__);

        free(pathName);
        free(fileName);
        free(romPath);
    }
    else
    {
        FILE* f = fopen(StateFileName, "w");
        if (f == NULL)
        {
            printf("SaveState: fopen save failed\n");
        }
        else
        {
            savestate(f);
            fclose(f);

            printf("SaveState: savestate OK.\n");
        }
    }


    odroid_system_led_set(0);
    odroid_input_battery_monitor_enabled_set(1);
}

bool scaling_enabled = true;


byte menu_visible = 0;
byte previous_menu_id = 0;
bool menu_exit_on_b = true;
int menu_item_index = 0;

int menu_draw_x_label = 0;
int menu_draw_x_value = 110;
int menu_draw_y = 0;
byte menu_draw_row_height = 10;
uint16_t menu_draw_bg_color = 0xFFFF;
int menu_draw_bg_x = 0;
int menu_draw_bg_y = 0;
int menu_draw_bg_w = 160;
int menu_draw_bg_h = 144;

uint16_t menu_draw_text_color = 0x0000;

typedef void (*menu_item_callback)(byte);

menu_item_callback menu_tick_callback = 0;

typedef struct menu_item {
  char label[16];
  char value_label[16];
  int value;
  void (*callback)(byte);
};

#define MAX_MENU_ITEMS 12
byte max_menu_items = MAX_MENU_ITEMS;
struct menu_item menu_items[MAX_MENU_ITEMS] = {};
void show_menu(byte menu_id);

int value_incr(int value, int min, int max, bool wrap){
  value++;
  if(value > max){
    if(wrap)
      value = min;
    else
      value = max;
  }
  return value;
};
int value_decr(int value, int min, int max, bool wrap){
  value--;
  if(value < min){
    if(wrap)
      value = max;
    else
      value = min;
  }
  return value;
};

void menu_item_callback_submenu(byte button){
  switch(button){
    case 3:
      show_menu( menu_items[menu_item_index].value );
    break;
  }
};

void menu_item_callback_volume(byte button){
  printf("menu_item_callback_volume %d\n",button);
  int value = menu_items[menu_item_index].value;
  switch(button){
    case 255:
      value = odroid_audio_volume_get();
    break;
    case 1:
      value = value_decr(value,0,ODROID_VOLUME_LEVEL_COUNT - 1,false);
    break;
    case 2:
      value = value_incr(value,0,ODROID_VOLUME_LEVEL_COUNT - 1,false);
    break;
  }
  sprintf(menu_items[menu_item_index].value_label,"%d",value);
  menu_items[menu_item_index].value = value;
  if(button != 255){
    odroid_audio_volume_set(value);
  }
};

void menu_item_callback_brightness(byte button){
  printf("menu_item_callback_brightness %d\n",button);
  int value = menu_items[menu_item_index].value;
  switch(button){
    case 255:
      value = 0;
    break;
    case 1:
      value = value_decr(value,0,9,false);
    break;
    case 2:
      value = value_incr(value,0,9,false);
    break;
  }
  sprintf(menu_items[menu_item_index].value_label,"%d",value);
  menu_items[menu_item_index].value = value;
  if(button != 255){
    // backlight_percentage_set(value * 10);
    int duty = 0x1fff * (value * 10 * 0.01f);
    ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty, 1);
    ledc_fade_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, LEDC_FADE_WAIT_DONE /*LEDC_FADE_NO_WAIT*/);
  }
};

void menu_item_callback_scaling(byte button){
  printf("menu_item_callback_scaling %d\n",button);
  int value = menu_items[menu_item_index].value;
  switch(button){
    case 255:
      value = scaling_enabled;
    break;
    case 1:
      value = value_decr(value,0,1,false);
    break;
    case 2:
      value = value_incr(value,0,1,false);
    break;
  }
  sprintf(menu_items[menu_item_index].value_label, value ? "On" : "Off");
  menu_items[menu_item_index].value = value;
  if(button != 255){
    scaling_enabled = value != 0;
  }
};

void menu_item_callback_rtc(byte m, byte button){
  printf("menu_item_callback_rtc %d %d\n",m,button);
  int value = menu_items[menu_item_index].value;
  switch(button){
    case 255:
      switch(m){
        case 0:
          value = rtc.carry;
        break;
        case 1:
          value = rtc.d;
        break;
        case 2:
          value = rtc.h;
        break;
        case 3:
          value = rtc.m;
        break;
      }
    break;
    case 1:
      switch(m){
        case 0:
          value = value_decr(value,0,1,false);
          rtc.carry = value;
        break;
        case 1:
          value = value_decr(value,0,364,false);
          rtc.d = value;
        break;
        case 2:
          value = value_decr(value,0,23,true);
          rtc.h = value;
        break;
        case 3:
          value = value_decr(value,0,59,true);
          rtc.m = value;
        break;
      }
    break;
    case 2:
      switch(m){
        case 0:
          value = value_incr(value,0,1,false);
          rtc.carry = value;
        break;
        case 1:
          value = value_incr(value,0,364,false);
          rtc.d = value;
        break;
        case 2:
          value = value_incr(value,0,23,true);
          rtc.h = value;
        break;
        case 3:
          value = value_incr(value,0,59,true);
          rtc.m = value;
        break;
      }
    break;
  }
  menu_items[menu_item_index].value = value;
  sprintf(menu_items[menu_item_index].value_label,"%d",value);
}

void menu_item_callback_rtc_c(byte button){
  menu_item_callback_rtc(0,button);
}
void menu_item_callback_rtc_d(byte button){
  menu_item_callback_rtc(1,button);
}
void menu_item_callback_rtc_h(byte button){
  menu_item_callback_rtc(2,button);
}
void menu_item_callback_rtc_m(byte button){
  menu_item_callback_rtc(3,button);
}


void menu_item_callback_save_state(byte button){
  printf("menu_item_callback_save_state %d\n",button);
  switch(button){
    case 255:

    break;
    case 3:
      SaveState();
      SaveStateRtc();
    break;
  }
};

void menu_item_callback_exit(byte button){
  printf("menu_item_callback_exit %d\n",button);
  switch(button){
    case 255:

    break;
    case 3:
      // DoMenuHome();
    break;
  }
};

byte add_menu_item(const char* label, menu_item_callback callback){
  sprintf(menu_items[menu_item_index].label,label);
  menu_items[menu_item_index].callback = callback;
  if(callback)
    callback(255);
  byte i = menu_item_index;
  menu_item_index++;
  return i;
};
byte add_menu_item_value(const char* label, int value, menu_item_callback callback){
  sprintf(menu_items[menu_item_index].label,label);
  menu_items[menu_item_index].value = value;
  menu_items[menu_item_index].callback = callback;
  if(callback)
    callback(255);
  byte i = menu_item_index;
  menu_item_index++;
  return i;
};
byte add_menu_item_submenu(const char* label, int value){
  return add_menu_item_value(label,value,&menu_item_callback_submenu);
};


void menu_init_1(){
  add_menu_item_value("Volume",0,&menu_item_callback_volume);
  add_menu_item_value("Brightness",0,&menu_item_callback_brightness);
  add_menu_item_value("Scaling",0,&menu_item_callback_scaling);
  add_menu_item_submenu("RTC",2);
  add_menu_item("Save State",&menu_item_callback_save_state);
  add_menu_item("Exit Emulator",&menu_item_callback_exit);
  add_menu_item_submenu("Exit Menu",0);

  previous_menu_id = 0;
};

void menu_2_tick(byte button){
  byte game_start_d = mem_read(0xd4b6);
  byte game_start_h = mem_read(0xd4b7);
  byte game_start_m = mem_read(0xd4b8);
  byte game_start_s = mem_read(0xd4b9);
  // byte dst = mem_read(0xd4c2);

  int day = game_start_d + rtc.d;
  if(rtc.carry)
    day += 365;

  int week = day / 7;
  byte weekday = day % 7;

  int hour = (game_start_h + rtc.h);
  // if(dst)
  //   hour++;
  int min = (game_start_m + rtc.m);
  int sec = (game_start_s + rtc.s);

  sprintf(menu_items[4].value_label,"%d",week);
  sprintf(menu_items[5].value_label,"%d",day);
  sprintf(menu_items[6].value_label,"%d",weekday);
  sprintf(menu_items[7].value_label,"%d,%d,%d",hour,rtc.h,game_start_h);
  sprintf(menu_items[8].value_label,"%d,%d,%d",min,rtc.m,game_start_m);
  sprintf(menu_items[9].value_label,"%d,%d,%d",sec,rtc.s,game_start_s);
};

void menu_init_2(){
  menu_draw_x_value = 90;

  add_menu_item_value("RTC Carry",0,&menu_item_callback_rtc_c);
  add_menu_item_value("RTC Day",0,&menu_item_callback_rtc_d);
  add_menu_item_value("RTC Hour",0,&menu_item_callback_rtc_h);
  add_menu_item_value("RTC Minute",0,&menu_item_callback_rtc_m);

  add_menu_item_value("Game Week",0,0);
  add_menu_item_value("Game Day",0,0);
  add_menu_item_value("Game Weekday",0,0);
  add_menu_item_value("Game Hour",0,0);
  add_menu_item_value("Game Minute",0,0);
  add_menu_item_value("Game Second",0,0);

  add_menu_item_submenu("Back",1);
  menu_tick_callback = menu_2_tick;

  previous_menu_id = 1;
};

void show_menu(byte menu_id){
  menu_visible = menu_id;

  menu_tick_callback = 0;

  menu_exit_on_b = true;
  menu_draw_x_label = 2;
  menu_draw_x_value = 110;
  menu_draw_y = 14;
  menu_draw_row_height = 10;
  menu_draw_bg_color = 0x0000;
  menu_draw_bg_x = 2;
  menu_draw_bg_y = 13;
  menu_draw_bg_w = 160;
  menu_draw_bg_h = 90;
  menu_draw_text_color = 0xffff;

  for(int i = 0; i < MAX_MENU_ITEMS; i++){
    menu_items[i].label[0] = 0;
    menu_items[i].value_label[0] = 0;
    menu_items[i].value = 0;
    menu_items[i].callback = 0;
  }

  menu_item_index = 0;
  int i = 1;
  switch(menu_id){
    case 1:
      menu_init_1();
    break;
    case 2:
      menu_init_2();
    break;
  }

  max_menu_items = menu_item_index;
  menu_item_index = 0;
};

void hide_menu(){
  menu_visible = 0;
  menu_item_index = 0;
}

uint startTime;
uint stopTime;
uint totalElapsedTime = 0;
uint actualFrameCount = 0;

odroid_gamepad_state lastJoysticState;
odroid_gamepad_state joystick;


#define SERIAL_BUFFER_SIZE 32
char serial_in[SERIAL_BUFFER_SIZE];
byte serial_i = 0;

void serial_in_cursor_set(byte i){
  serial_i = i;
}
byte serial_in_pop(){
  if(serial_i > SERIAL_BUFFER_SIZE)
    return 0;
  return serial_in[serial_i++];
}

int serial_in_get_int_hex(){
  if(serial_in[serial_i] != '0' || serial_in[serial_i + 1] != 'x')
    return -1;
  serial_i += 2;
  int r = 0;
  byte v = 0;
  while(true){
    if(serial_i > SERIAL_BUFFER_SIZE)
      break;
    v = serial_in[serial_i];
    byte type = 0;

    if( v >= 48 && v <= 57) // 0-9
      type = 1;
    if( v >= 65 && v <= 70) // A-F
      type = 2;
    if( v >= 97 && v <= 102) // a-f
      type = 3;

    if(type == 0)
      break;

    if(type == 1)
      v -= 48;

    if(type == 2)
      v = 10 + (v - 65);

    if(type == 3)
      v = 10 + (v - 97);

    r *= 16;
    r += v;
    serial_i++;
  }
  return r;
}

int serial_in_get_int(){
  if(serial_in[serial_i] == '0' && serial_in[serial_i + 1] == 'x')
    return serial_in_get_int_hex();
  int r = 0;
  byte v = 0;
  while(true){
    if(serial_i > SERIAL_BUFFER_SIZE)
      break;
    v = serial_in[serial_i];
    // non-ascii number
    if(v < 48 || v > 57)
      break;
    v -= 48;
    r *= 10;
    r += v;
    serial_i++;
  }
  return r;
}


void gbaext_serial_handler_rtc(){

  // RTC.h= prefix
  if(serial_in[3] == '.' && serial_in[5] == '='){
    serial_in_cursor_set(6);
    int value = serial_in_get_int();
    if(value < 0)
      value = 0;
    printf("value: %d\n",value);

    switch( serial_in[4] ){
      case 'c':
        if(value > 0)
          value = 1;
        rtc.carry = value;
        break;
      case 'd':
        if(value > 364)
          value = 364;
        rtc.d = value;
        break;
      case 'h':
        if(value > 23)
          value = 23;
        rtc.h = value;
        break;
      case 'm':
        if(value > 59)
          value = 59;
        rtc.m = value;
        break;
      case 's':
        if(value > 59)
          value = 59;
        rtc.s = value;
        break;
    }

    printf("value: %d\n",value);

    printf("RTC: carry: %d, d: %d, h: %d, m: %d, s: %d, t: %d\n",rtc.carry,rtc.d,rtc.h,rtc.m,rtc.s,rtc.t);

  }else{
    printf("RTC: carry: %d, d: %d, h: %d, m: %d, s: %d, t: %d\n",rtc.carry,rtc.d,rtc.h,rtc.m,rtc.s,rtc.t);
  }

}

void gbaext_serial_handler_mem(){
  // MEMR 4 3000
  if(serial_in[3] == 'R'){
    serial_in_cursor_set(5);
    byte size = serial_in_get_int();
    serial_in_pop();
    uint16_t addr = serial_in_get_int();
    // printf("MEM: read %d at %d (%x)\n",size,addr,addr);

    // crystal: DCDF
    // gs     : DA2A
    // KRYS   : 8A,91,98,92
    // BARR   : 81,80,91,91
    printf("MEMR[%d,%d]=0x,",size,addr);
    for(byte i = 0; i < size; i++){
      byte value = mem_read(addr + i);
      if(i == size - 1)
        printf("%x\n",value);
      else
        printf("%x,",value);
    }
  }
}

void gbaext_serial_handler_vol(){
  // VOL=4
  int vol = odroid_audio_volume_get();
  if(serial_in[3] == '='){
    serial_in_cursor_set(4);
    vol = serial_in_get_int();
    if(vol < 0)
      vol = 0;
    if(vol > ODROID_VOLUME_LEVEL_COUNT)
      vol = ODROID_VOLUME_LEVEL_COUNT;
    odroid_audio_volume_set(vol);
  }
  printf("VOL=%d\n",vol);
}

void gbaext_serial_handler_scr(){
  // SCR
  printf("SCR=%d,%d,%d\n",160,144,2);
  for(int y = 0; y < 144; y++){
    for(int x = 0; x < 160; x++){
      uint16_t pixel = framebuffer[ (y * 160) + x ];
      byte low = pixel & 0xff; 
      byte high = (pixel>>8) & 0xff;
      // printf(low);
      // printf(high);
      putchar(high);
      putchar(low);
    }
    // printf("\n");
  }
  printf("END\n");
}


// mrb_state *mrb;
// void mruby_task(void *pvParameter)
// {
//   printf("starting mruby_task\n");
//   // printf("starting mruby_task\n");

//   // mrb_state *mrb = mrb_open();
//   mrb = mrb_open();

//   printf("mruby_task: created\n");

//   mrb_close(mrb);

//   printf("mruby_task: closed\n");

//   // This task should never end, even if the
//   // script ends.
//   while (1) {
//   }
// }

void gbaext_init(){

  printf("mruby test :O~\n");

  odroid_audio_volume_set(0);

  // printf("creating mruby_task\n");
  // xTaskCreate(&mruby_task, "mruby_task", 3000, NULL, 5, NULL);
  // printf("created mruby_task\n");

  // mrb_state *mrb;

  // printf("creating mrb_state\n");

  // mrb = mrb_open();

  // printf("created mrb_state\n");

  // mrb_close(mrb);

  // printf("closed mrb_state\n");

  // for(uint16_t addr = 0xc000; addr < 0xe000; addr++){

  //   if( mem_read(addr) == 0x8a && mem_read(addr+1) == 0x91 && mem_read(addr + 2) == 0x98 && mem_read(addr + 3) == 0x92){
  //     printf("found KRYS at %d %x\n",addr,addr);
  //   }
  //   if( mem_read(addr) == 0x81 && mem_read(addr+1) == 0x80 && mem_read(addr + 2) == 0x91 && mem_read(addr + 3) == 0x91){
  //     printf("found BARR at %d %x\n",addr,addr);
  //   }

  // }


}

void gbaext_serial_handle(){
  if( gets(serial_in) ){
    serial_in_cursor_set(0);

    // RTC prefix
    if(serial_in[0] == 'R' && serial_in[1] == 'T' && serial_in[2] == 'C'){
      gbaext_serial_handler_rtc();
    }

    // MEM prefix
    if(serial_in[0] == 'M' && serial_in[1] == 'E' && serial_in[2] == 'M'){
      gbaext_serial_handler_mem();
    }

    // VOL prefix
    if(serial_in[0] == 'V' && serial_in[1] == 'O' && serial_in[2] == 'L'){
      gbaext_serial_handler_vol();
    }

    // SCR prefix
    if(serial_in[0] == 'S' && serial_in[1] == 'C' && serial_in[2] == 'R'){
      gbaext_serial_handler_scr();
    }

  }
}

bool clock_adjust_mode = false;
int clock_adjust_field = 1;
bool clock_adjust_mode_just_changed = false;
void gbaext_every_frame(){

  gbaext_serial_handle();

  if(menu_visible){
    if(menu_tick_callback){
      menu_tick_callback(menu_visible);
    }
  }

  // toggle menu on volume button press
  if (!lastJoysticState.values[ODROID_INPUT_VOLUME] && joystick.values[ODROID_INPUT_VOLUME]){
    if(menu_visible > 0){
      // menu_visible = 0;
      hide_menu();
    }else{
      show_menu(1);
      // menu_visible = 1;

      // for(int i = 0; i < MAX_MENU_ITEMS; i++){
      //   menu_items[i].label[0] = 0;
      //   menu_items[i].value_label[0] = 0;
      //   menu_items[i].value = 0;
      //   menu_items[i].callback = 0;
      // }

      // sprintf(menu_items[0].label,"volume");
      // menu_items[0].value = 0;
      // menu_items[0].callback = &menu_item_callback_volume;

      // sprintf(menu_items[1].label,"brightness");
      // menu_items[1].value = 0;
      // menu_items[1].callback = &menu_item_callback_brightness;

      // sprintf(menu_items[2].label,"scaling");
      // menu_items[2].value = 0;
      // menu_items[2].callback = &menu_item_callback_scaling;

      // sprintf(menu_items[3].label,"save state");
      // menu_items[3].callback = &menu_item_callback_save_state;

      // sprintf(menu_items[4].label,"exit");
      // menu_items[4].callback = &menu_item_callback_exit;

      // for(int i = 0; i < MAX_MENU_ITEMS; i++){
      //   if(menu_items[i].callback){
      //     menu_item_index = i;
      //     menu_items[i].callback(255);
      //   }
      // }

      // menu_item_index = 0;
    }
  }

  if(menu_visible){
    // disable input to emulator if menu visible
    pad_set(PAD_UP, 0);
    pad_set(PAD_RIGHT, 0);
    pad_set(PAD_DOWN, 0);
    pad_set(PAD_LEFT, 0);

    pad_set(PAD_SELECT, 0);
    pad_set(PAD_START, 0);

    pad_set(PAD_A, 0);
    pad_set(PAD_B, 0);

    if (!lastJoysticState.values[ODROID_INPUT_UP] && joystick.values[ODROID_INPUT_UP]){
      menu_item_index--;
      if(menu_item_index < 0)
        menu_item_index++;
    }
    if (!lastJoysticState.values[ODROID_INPUT_DOWN] && joystick.values[ODROID_INPUT_DOWN]){
      menu_item_index++;
      if(menu_item_index >= MAX_MENU_ITEMS || menu_item_index >= max_menu_items || !menu_items[menu_item_index].label[0])
        menu_item_index--;
    }

    byte button = 0;
    if (!lastJoysticState.values[ODROID_INPUT_LEFT] && joystick.values[ODROID_INPUT_LEFT]){
      button = 1;
    }
    if (!lastJoysticState.values[ODROID_INPUT_RIGHT] && joystick.values[ODROID_INPUT_RIGHT]){
      button = 2;
    }
    if (!lastJoysticState.values[ODROID_INPUT_A] && joystick.values[ODROID_INPUT_A]){
      button = 3;
    }
    if (!lastJoysticState.values[ODROID_INPUT_B] && joystick.values[ODROID_INPUT_B]){
      button = 4;
    }

    if(button){

      if(menu_items[menu_item_index].callback){
        menu_items[menu_item_index].callback(button);
      }

      if(button == 4 && menu_exit_on_b){
        // if we're exiting a menu, send the B event to all items
        for(menu_item_index = 0; menu_item_index < MAX_MENU_ITEMS; menu_item_index++){
          if(menu_items[menu_item_index].callback){
            menu_items[menu_item_index].callback(button);
          }
        }
        if(previous_menu_id)
          show_menu(previous_menu_id);
        else
          hide_menu();
      }

    }

  }


  // in_menu = $client.read_uint8( PokeMan::SYMBOLS_CRYSTAL[:ui_in_menu] )
  // gear_menu = $client.read_uint8( PokeMan::SYMBOLS_CRYSTAL[:ui_gear_card] )
  // menu_2 = $client.read_uint8( 0xcf71 )
  // menu_3 = $client.read_uint8( 0xffd6 )
  // menu_4 = $client.read_uint8( 0xffd7 )

  bool in_pokegear_clock_menu = false;
  if( mem_read(0xffaa) == 1 && mem_read(0xcf64) == 0 && mem_read(0xcf71) == 161 && mem_read(0xffd6) == 0 && mem_read(0xffd7) == 156 )
    in_pokegear_clock_menu = true;

  if(clock_adjust_mode_just_changed){
    clock_adjust_mode_just_changed = false;
    // joystick.values[ODROID_INPUT_SELECT] = 1;
    lastJoysticState.values[ODROID_INPUT_SELECT] = 1;
  }

  if(in_pokegear_clock_menu){

    if(!clock_adjust_mode && joystick.values[ODROID_INPUT_SELECT] && !lastJoysticState.values[ODROID_INPUT_SELECT])
    {
      printf("detected select, enabling, cancelling\n");
      clock_adjust_mode = true;
      clock_adjust_mode_just_changed = true;
    }else if(clock_adjust_mode && joystick.values[ODROID_INPUT_SELECT] && !lastJoysticState.values[ODROID_INPUT_SELECT])
    {
      printf("detected select, disabling, cancelling\n");
      clock_adjust_mode = false;
      clock_adjust_mode_just_changed = true;
    }

  }else{
    clock_adjust_mode = false;
  }


  if(clock_adjust_mode || clock_adjust_mode_just_changed){
    // joystick.values[ODROID_INPUT_SELECT] = 0;
    // lastJoysticState.values[ODROID_INPUT_SELECT] = 0;

    if(joystick.values[ODROID_INPUT_LEFT] && !lastJoysticState.values[ODROID_INPUT_LEFT]){
      clock_adjust_field++;
      if(clock_adjust_field > 2)
        clock_adjust_field = 0;
      printf("clock_adjust_field: %d\n",clock_adjust_field);
    }
    if(joystick.values[ODROID_INPUT_RIGHT] && !lastJoysticState.values[ODROID_INPUT_RIGHT]){
      clock_adjust_field--;
      if(clock_adjust_field < 0)
        clock_adjust_field = 2;
      printf("clock_adjust_field: %d\n",clock_adjust_field);
    }

    if(joystick.values[ODROID_INPUT_UP] && !lastJoysticState.values[ODROID_INPUT_UP]){
      switch(clock_adjust_field){
        case 0:
          rtc.m += 1;
          if(rtc.m > 59)
            rtc.m = 59;
        break;
        case 1:
          rtc.h += 1;
          if(rtc.h > 23)
            rtc.h = 23;
        break;
        case 2:
          rtc.d += 1;
          if(rtc.d > 364)
            rtc.d = 364;
        break;
      }
    }
    if(joystick.values[ODROID_INPUT_DOWN] && !lastJoysticState.values[ODROID_INPUT_DOWN]){
      switch(clock_adjust_field){
        case 0:
          rtc.m -= 1;
          if(rtc.m < 0)
            rtc.m = 0;
        break;
        case 1:
        rtc.h -= 1;
        if(rtc.h < 0)
          rtc.h = 0;
        break;
        case 2:
          rtc.d -= 1;
          if(rtc.d < 0)
            rtc.d = 0;
        break;
      }
    }

    pad_set(PAD_SELECT, 0);
    pad_set(PAD_UP, 0);
    pad_set(PAD_RIGHT, 0);
    pad_set(PAD_DOWN, 0);
    pad_set(PAD_LEFT, 0);
  }

}

void gbaext_every_second(){



}

float vfps = 60.0;
void gbaext_before_draw_frame(){

  if(menu_visible){
    set_adagfx_buffer(framebuffer,160,144);

    // writeFillRect(menu_draw_bg_x,menu_draw_bg_y,menu_draw_bg_w,menu_draw_bg_h,menu_draw_bg_color);

    setTextSize(1);
    setTextColor(menu_draw_text_color);
    setTextBgColor(menu_draw_text_color);
    setOutline(true);
    setOutlineColor(menu_draw_bg_color);

    // setCursor(2,1);
    // drawPrint("bat memfree fps frame ____");

    setCursor(2,1);

    int bat = battery_state.percentage;
    if(bat > 99) bat = 99;
    drawPrintInt(bat);
    drawPrint("% ");

    drawPrintFloat( (float)esp_get_free_heap_size() / 1024.0 , 2 );
    drawPrint("kb ");

    float seconds = totalElapsedTime / (CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ * 1000000.0f); // 240000000.0f; // (240Mhz)
    vfps = actualFrameCount / seconds;
    drawPrintFloat( vfps , 0 );

    drawPrint("  ");

    drawPrintInt( elapsedTime / (CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ * 1000) );
    drawPrint("ms");

    for(int i = 0; i < MAX_MENU_ITEMS; i++){
      if(menu_items[i].label[0]){
        setCursor(menu_draw_x_label,menu_draw_y + (i * menu_draw_row_height));
        if(i == menu_item_index){
          drawPrint("> ");
        }else{
          drawPrint("  ");
        }
        drawPrint(menu_items[i].label);
        setCursor(menu_draw_x_value,menu_draw_y + (i * menu_draw_row_height));
        drawPrint(menu_items[i].value_label);
      }
    }
  }

}


void gbaext_before_screen_write(){
  
}
void gbaext_after_screen_write(){
  
}


// --- MAIN
QueueHandle_t vidQueue;
QueueHandle_t audioQueue;

float Volume = 0.1f;

int pcm_submit()
{
    odroid_audio_submit(currentAudioBufferPtr, currentAudioSampleCount >> 1);

    return 1;
}


int BatteryPercent = 100;


void run_to_vblank()
{
  /* FRAME BEGIN */

  /* FIXME: djudging by the time specified this was intended
  to emulate through vblank phase which is handled at the
  end of the loop. */
  cpu_emulate(2280);

  /* FIXME: R_LY >= 0; comparsion to zero can also be removed
  altogether, R_LY is always 0 at this point */
  while (R_LY > 0 && R_LY < 144)
  {
    /* Step through visible line scanning phase */
    emu_step();
  }

  /* VBLANK BEGIN */


  gbaext_before_draw_frame();

  //vid_end();
  if ((frame % 2) == 0)
  {

      xQueueSend(vidQueue, &framebuffer, portMAX_DELAY);

      // swap buffers
      currentBuffer = currentBuffer ? 0 : 1;
      framebuffer = displayBuffer[currentBuffer];

      fb.ptr = framebuffer;
  }

  rtc_tick();

  sound_mix();

  //if (pcm.pos > 100)
  {
        currentAudioBufferPtr = audioBuffer[currentAudioBuffer];
        currentAudioSampleCount = pcm.pos;

        void* tempPtr = 0x1234;
        xQueueSend(audioQueue, &tempPtr, portMAX_DELAY);

        // Swap buffers
        currentAudioBuffer = currentAudioBuffer ? 0 : 1;
        pcm.buf = audioBuffer[currentAudioBuffer];
        pcm.pos = 0;
  }

  if (!(R_LCDC & 0x80)) {
    /* LCDC operation stopped */
    /* FIXME: djudging by the time specified, this is
    intended to emulate through visible line scanning
    phase, even though we are already at vblank here */
    cpu_emulate(32832);
  }

  while (R_LY > 0) {
    /* Step through vblank phase */
    emu_step();
  }
}


uint16_t* menuFramebuffer = 0;

volatile bool videoTaskIsRunning = false;
bool previous_scale_enabled = true;

void videoTask(void *arg)
{
  esp_err_t ret;

  videoTaskIsRunning = true;

  uint16_t* param;
  while(1)
  {
        xQueuePeek(vidQueue, &param, portMAX_DELAY);

        if (param == 1)
            break;

        if (previous_scale_enabled != scaling_enabled)
        {
            // Clear display
            ili9341_write_frame_gb(NULL, true);
            previous_scale_enabled = scaling_enabled;
        }

        gbaext_before_screen_write();

        ili9341_write_frame_gb(param, scaling_enabled);
        odroid_input_battery_level_read(&battery_state);

        gbaext_after_screen_write();

        xQueueReceive(vidQueue, &param, portMAX_DELAY);
    }


    // Draw hourglass
    send_reset_drawing((320 / 2 - 48 / 2), 96, 48, 48);

    // split in half to fit transaction size limit
    uint16_t* icon = image_hourglass_empty_black_48dp.pixel_data;

    send_continue_line(icon, 48, 24);
    send_continue_line(icon + 24 * 48, 48, 24);

    send_continue_wait();


    videoTaskIsRunning = false;
    vTaskDelete(NULL);

    while (1) {}
}


volatile bool AudioTaskIsRunning = false;
void audioTask(void* arg)
{
  // sound
  uint16_t* param;

  AudioTaskIsRunning = true;
  while(1)
  {
    xQueuePeek(audioQueue, &param, portMAX_DELAY);

    if (param == 0)
    {
        // TODO: determine if this is still needed
        abort();
    }
    else if (param == 1)
    {
        break;
    }
    else
    {
        pcm_submit();
    }

    xQueueReceive(audioQueue, &param, portMAX_DELAY);
  }

  printf("audioTask: exiting.\n");
  odroid_audio_terminate();

  AudioTaskIsRunning = false;
  vTaskDelete(NULL);

  while (1) {}
}




static void LoadState(const char* cartName)
{
    char* romName = odroid_settings_RomFilePath_get();
    if (romName)
    {
        char* fileName = odroid_util_GetFileName(romName);
        if (!fileName) abort();

        char fileNameTagged[64];
        sprintf(fileNameTagged,"%s.latest.gbc",fileName);

        char* pathName = odroid_sdcard_create_savefile_path(SD_BASE_PATH, fileNameTagged);
        if (!pathName) abort();

        FILE* f = fopen(pathName, "r");
        if (f == NULL)
        {
            printf("LoadState: fopen load failed\n");
        }
        else
        {
            loadstate(f);
            fclose(f);

            vram_dirty();
            pal_dirty();
            sound_dirty();
            mem_updatemap();

            printf("LoadState: loadstate OK.\n");
        }

        free(pathName);
        free(fileName);
        free(romName);
    }
    else
    {
        FILE* f = fopen(StateFileName, "r");
        if (f == NULL)
        {
            printf("LoadState: fopen load failed\n");
        }
        else
        {
            loadstate(f);
            fclose(f);

            vram_dirty();
            pal_dirty();
            sound_dirty();
            mem_updatemap();

            printf("LoadState: loadstate OK.\n");
        }
    }


    // Volume = odroid_settings_Volume_get();
}

static void PowerDown()
{
    uint16_t* param = 1;

    // Clear audio to prevent studdering
    printf("PowerDown: stopping audio.\n");

    xQueueSend(audioQueue, &param, portMAX_DELAY);
    while (AudioTaskIsRunning) {}


    // Stop tasks
    printf("PowerDown: stopping tasks.\n");

    xQueueSend(vidQueue, &param, portMAX_DELAY);
    while (videoTaskIsRunning) {}


    // state
    printf("PowerDown: Saving state.\n");
    SaveState();
    SaveStateRtc();

    // LCD
    printf("PowerDown: Powerdown LCD panel.\n");
    ili9341_poweroff();

    odroid_system_sleep();


    // Should never reach here
    abort();
}

static void DoMenuHome()
{
    esp_err_t err;
    uint16_t* param = 1;

    // Clear audio to prevent studdering
    printf("PowerDown: stopping audio.\n");

    xQueueSend(audioQueue, &param, portMAX_DELAY);
    while (AudioTaskIsRunning) {}


    // Stop tasks
    printf("PowerDown: stopping tasks.\n");

    xQueueSend(vidQueue, &param, portMAX_DELAY);
    while (videoTaskIsRunning) {}


    // state
    printf("PowerDown: Saving state.\n");
    SaveState();
    SaveStateRtc();


    // Set menu application
    odroid_system_application_set(0);


    // Reset
    esp_restart();
}



void app_main(void)
{
    printf("gnuboy mod start.\n");

    nvs_flash_init();

    odroid_system_init();

    odroid_input_gamepad_init();



    // Boot state overrides
    bool forceConsoleReset = false;

    switch (esp_sleep_get_wakeup_cause())
    {
        case ESP_SLEEP_WAKEUP_EXT0:
        {
            printf("app_main: ESP_SLEEP_WAKEUP_EXT0 deep sleep wake\n");
            break;
        }

        case ESP_SLEEP_WAKEUP_EXT1:
        case ESP_SLEEP_WAKEUP_TIMER:
        case ESP_SLEEP_WAKEUP_TOUCHPAD:
        case ESP_SLEEP_WAKEUP_ULP:
        case ESP_SLEEP_WAKEUP_UNDEFINED:
        {
            printf("app_main: Non deep sleep startup\n");

            odroid_gamepad_state bootState = odroid_input_read_raw();

            if (bootState.values[ODROID_INPUT_MENU])
            {
                // Force return to factory app to recover from
                // ROM loading crashes

                // Set menu application
                odroid_system_application_set(0);

                // Reset
                esp_restart();
            }

            if (bootState.values[ODROID_INPUT_START])
            {
                // Reset emulator if button held at startup to
                // override save state
                forceConsoleReset = true;
            }

            break;
        }
        default:
            printf("app_main: Not a deep sleep reset\n");
            break;
    }


    // Display
    ili9341_prepare();
    ili9341_init();
    //odroid_display_show_splash();

    // Load ROM
    loader_init(NULL);

    // Clear display
    ili9341_write_frame_gb(NULL, true);

    // Audio hardware
    odroid_audio_init(AUDIO_SAMPLE_RATE);

    // Allocate display buffers
    displayBuffer[0] = heap_caps_malloc(160 * 144 * 2, MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    displayBuffer[1] = heap_caps_malloc(160 * 144 * 2, MALLOC_CAP_8BIT | MALLOC_CAP_DMA);

    if (displayBuffer[0] == 0 || displayBuffer[1] == 0)
        abort();

    framebuffer = displayBuffer[0];

    for (int i = 0; i < 2; ++i)
    {
        memset(displayBuffer[i], 0, 160 * 144 * 2);
    }

    printf("app_main: displayBuffer[0]=%p, [1]=%p\n", displayBuffer[0], displayBuffer[1]);

    // blue led
    gpio_set_direction(GPIO_NUM_2, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_2, 0);

    //  Charge
    odroid_input_battery_level_init();

    // video
    vidQueue = xQueueCreate(1, sizeof(uint16_t*));
    audioQueue = xQueueCreate(1, sizeof(uint16_t*));

    xTaskCreatePinnedToCore(&videoTask, "videoTask", 1024, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(&audioTask, "audioTask", 2048, NULL, 5, NULL, 1); //768


    //debug_trace = 1;

    emu_reset();

    //&rtc.carry, &rtc.stop,
    rtc.d = 1;
    rtc.h = 1;
    rtc.m = 1;
    rtc.s = 1;
    rtc.t = 1;

    // vid_begin
    memset(&fb, 0, sizeof(fb));
    fb.w = 160;
    fb.h = 144;
    fb.pelsize = 2;
    fb.pitch = fb.w * fb.pelsize;
    fb.indexed = 0;
    fb.ptr = framebuffer;
    fb.enabled = 1;
    fb.dirty = 0;


    // Note: Magic number obtained by adjusting until audio buffer overflows stop.
    const int audioBufferLength = AUDIO_SAMPLE_RATE / 10 + 1;
    //printf("CHECKPOINT AUDIO: HEAP:0x%x - allocating 0x%x\n", esp_get_free_heap_size(), audioBufferLength * sizeof(int16_t) * 2 * 2);
    const int AUDIO_BUFFER_SIZE = audioBufferLength * sizeof(int16_t) * 2;

    // pcm.len = count of 16bit samples (x2 for stereo)
    memset(&pcm, 0, sizeof(pcm));
    pcm.hz = AUDIO_SAMPLE_RATE;
    pcm.stereo = 1;
    pcm.len = /*pcm.hz / 2*/ audioBufferLength;
    pcm.buf = heap_caps_malloc(AUDIO_BUFFER_SIZE, MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    pcm.pos = 0;

    audioBuffer[0] = pcm.buf;
    audioBuffer[1] = heap_caps_malloc(AUDIO_BUFFER_SIZE, MALLOC_CAP_8BIT | MALLOC_CAP_DMA);

    if (audioBuffer[0] == 0 || audioBuffer[1] == 0)
        abort();


    sound_reset();


    lcd_begin();


    // Load state
    LoadState(rom.name);


    // uint startTime;
    // uint stopTime;
    // uint totalElapsedTime = 0;
    // uint actualFrameCount = 0;
    // odroid_gamepad_state lastJoysticState;

    ushort menuButtonFrameCount = 0;
    bool ignoreMenuButton = lastJoysticState.values[ODROID_INPUT_MENU];

    // Reset if button held at startup
    if (forceConsoleReset)
    {
        emu_reset();
    }


    printf("main loop starting.\n");


    odroid_input_gamepad_read(&lastJoysticState);

    printf("running gbaext_init.\n");
    gbaext_init();

    while (true)
    {
        // odroid_gamepad_state joystick;
        odroid_input_gamepad_read(&joystick);

        if (ignoreMenuButton)
        {
            ignoreMenuButton = lastJoysticState.values[ODROID_INPUT_MENU];
        }

        if (!ignoreMenuButton && lastJoysticState.values[ODROID_INPUT_MENU] && joystick.values[ODROID_INPUT_MENU])
        {
            ++menuButtonFrameCount;
        }
        else
        {
            menuButtonFrameCount = 0;
        }

        //if (!lastJoysticState.Menu && joystick.Menu)
        if (menuButtonFrameCount > 60 * 2)
        {
            // Save state
            gpio_set_level(GPIO_NUM_2, 1);

            PowerDown();

            gpio_set_level(GPIO_NUM_2, 0);
        }

        if (!ignoreMenuButton && lastJoysticState.values[ODROID_INPUT_MENU] && !joystick.values[ODROID_INPUT_MENU])
        {
            // Save state
            gpio_set_level(GPIO_NUM_2, 1);

            //DoMenu();
            DoMenuHome();

            gpio_set_level(GPIO_NUM_2, 0);
        }


        // if (!lastJoysticState.values[ODROID_INPUT_VOLUME] && joystick.values[ODROID_INPUT_VOLUME])
        // {
        //     odroid_audio_volume_change();
        //     printf("main: Volume=%d\n", odroid_audio_volume_get());
        // }


        // Scaling
        if (joystick.values[ODROID_INPUT_START] && !lastJoysticState.values[ODROID_INPUT_RIGHT] && joystick.values[ODROID_INPUT_RIGHT])
        {
            scaling_enabled = !scaling_enabled;
        }

        pad_set(PAD_UP, joystick.values[ODROID_INPUT_UP]);
        pad_set(PAD_RIGHT, joystick.values[ODROID_INPUT_RIGHT]);
        pad_set(PAD_DOWN, joystick.values[ODROID_INPUT_DOWN]);
        pad_set(PAD_LEFT, joystick.values[ODROID_INPUT_LEFT]);

        pad_set(PAD_SELECT, joystick.values[ODROID_INPUT_SELECT]);
        pad_set(PAD_START, joystick.values[ODROID_INPUT_START]);

        pad_set(PAD_A, joystick.values[ODROID_INPUT_A]);
        pad_set(PAD_B, joystick.values[ODROID_INPUT_B]);

        gbaext_every_frame();

        startTime = xthal_get_ccount();
        run_to_vblank();
        stopTime = xthal_get_ccount();


        lastJoysticState = joystick;

        if (stopTime > startTime)
          elapsedTime = (stopTime - startTime);
        else
          elapsedTime = ((uint64_t)stopTime + (uint64_t)0xffffffff) - (startTime);

        totalElapsedTime += elapsedTime;
        ++frame;
        ++actualFrameCount;


        if (actualFrameCount == 60)
        {
          float seconds = totalElapsedTime / (CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ * 1000000.0f); // 240000000.0f; // (240Mhz)
          float fps = actualFrameCount / seconds;

          printf("HEAP:0x%x, FPS:%f, BATTERY:%d [%d]\n", esp_get_free_heap_size(), fps, battery_state.millivolts, battery_state.percentage);

          actualFrameCount = 0;
          totalElapsedTime = 0;

          gbaext_every_second();

        }

    }
}
