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


void gbaext_init(){

  printf("new code omg :O~\n");

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

  }
}

bool clock_adjust_mode = false;
int clock_adjust_field = 1;
bool clock_adjust_mode_just_changed = false;
void gbaext_every_frame(){

  // gbaext_serial_handle();

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



// --- MAIN
QueueHandle_t vidQueue;
QueueHandle_t audioQueue;

float Volume = 1.0f;

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

//   for(byte buffid = 0; buffid < 2; buffid++){
//   //   set_adagfx_buffer(displayBuffer[buffid],160,144);
//   //   for(int i = 0; i < 140; i++){
//   //     writePixel(i,i,0xFF0F);
//   //   }
//   // }
//   set_adagfx_buffer(displayBuffer[buffid],160,144);
//   // writePixel(10,20,0xFF0F);
//   // writePixel(20,10,0xFF0F);
//   // for(int i = 10; i < 20; i++){
//   //   writePixel(i,i,0xFF0F);
//   // }
//   // writeFillRect(20,20,20,20,0xFFFF);
//   // writeFillRect(21,21,20,20,0xFFFF);
//   writeFillRect(0,0,80,10,0xFFFF);
//   drawChar(2,2,'O',0x0000,0xFFFF,1);
//   drawChar(12,2,'M',0x0000,0xFFFF,1);
//   drawChar(22,2,'G',0x0000,0xFFFF,1);

//   __asm__("memw");
//   __asm__("nop");
//   __asm__("nop");
//   __asm__("nop");
//   __asm__("nop");
//   __asm__("nop");
//   __asm__("nop");
//   __asm__("nop");
// }



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
bool scaling_enabled = true;
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

        __asm__("memw");
        // set_adagfx_buffer(param,160,144);
        // for(int i = 0; i < 140; i++){
        //   writePixel(i,i,0xFF0F);
        // }
        __asm__("memw");
        // // writePixel(1,1,0xFFFF);
        // // writePixel(2,2,0x0000);
        // // writePixel(3,3,0xFFFF);
        // writeFillRect(0,0,80,10,0xFFFF);
        // // drawChar(0,0,'O',0x0000,0xFFFF,1);
        // // drawChar(10,0,'M',0x0000,0xFFFF,1);
        // // drawChar(20,0,'G',0x0000,0xFFFF,1);
        // writeFillRect(20,20,20,20,0xFFFF);

        ili9341_write_frame_gb(param, scaling_enabled);
        odroid_input_battery_level_read(&battery_state);



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

        char* pathName = odroid_sdcard_create_savefile_path(SD_BASE_PATH, fileName);
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

static void LoadState(const char* cartName)
{
    char* romName = odroid_settings_RomFilePath_get();
    if (romName)
    {
        char* fileName = odroid_util_GetFileName(romName);
        if (!fileName) abort();

        char* pathName = odroid_sdcard_create_savefile_path(SD_BASE_PATH, fileName);
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


    Volume = odroid_settings_Volume_get();
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


    uint startTime;
    uint stopTime;
    uint totalElapsedTime = 0;
    uint actualFrameCount = 0;
    // odroid_gamepad_state lastJoysticState;

    ushort menuButtonFrameCount = 0;
    bool ignoreMenuButton = lastJoysticState.values[ODROID_INPUT_MENU];

    // Reset if button held at startup
    if (forceConsoleReset)
    {
        emu_reset();
    }


    printf("gnuboy mod main loop starting.\n");


    odroid_input_gamepad_read(&lastJoysticState);

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


        if (!lastJoysticState.values[ODROID_INPUT_VOLUME] && joystick.values[ODROID_INPUT_VOLUME])
        {
            odroid_audio_volume_change();
            printf("main: Volume=%d\n", odroid_audio_volume_get());
        }


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
