/**
 * TODO List :
 * Pray that everything works...
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "hardware/interp.h"
#include "hardware/adc.h"
#include "hardware/pwm.h"
#include "pico/multicore.h"


#include "st7789_lcd.pio.h"

/////////////////////////////////////////////////////////
// include game sprites

#include "sprite_bot_idle.h"
#include "sprite_bot_knock.h"
#include "sprite_table.h"
#include "sprite_all_knowing.h"
#include "sprite_text.h"            //include words and numbers
#include "sprite_menu_screen.h"
#include "sprite_instructions.h"

//cards
#include "sprite_rock.h"
#include "sprite_paper.h"
#include "sprite_scissors.h"
#include "sprite_special.h"


/////////////////////////////////////////////////////////
// include game sounds

#include "snd_boss_defeated.h"
#include "snd_choose.h"
#include "snd_fail.h"
#include "snd_ost1.h"
#include "snd_ost2.h"
#include "snd_pick.h"
#include "snd_showup.h"
#include "snd_success_papers.h"
#include "snd_success_rock.h"
#include "snd_success_scissors.h"
#include "snd_success_special.h"
#include "snd_tie.h"
#include "snd_valid.h"
#include "snd_boom.h"

/////////////////////////////////////////////////////////
//Hardware defs

//Sound
#define SND_PIN         21
#define SOUND_OFFSET    128

//displays 
#define SCREEN_WIDTH_2 80
#define SCREEN_HEIGHT_2 160

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 128
#define IMAGE_SIZE 256
#define LOG_IMAGE_SIZE 8

#define PIN_DIN_2 13
#define PIN_CLK_2 14
#define PIN_CS_2 10
#define PIN_DC_2 11
#define PIN_RESET_2 12
#define PIN_BL_2 5

#define PIN_DIN 3
#define PIN_CLK 2
#define PIN_CS 6
#define PIN_DC 5
#define PIN_RESET 4
#define PIN_BL 15

#define SERIAL_CLK_DIV 10.f
#define SERIAL_CLK_DIV_2 20.f


//for multicore
#define FLAG_VALUE 123


/////////////////////////////////////////////////
//game definitions

enum cardTypes {
    ROCK_TYPE = 0,
    PAPER_TYPE,
    SCISSORS_TYPE,
    SPECIAL_TYPE,
};

enum roundTypes{
    LOSE_TYPE = 0,
    TIE_TYPE,
    WIN_TYPE,
};

enum effect_type{
    //COMMON EFFECTS
    NO_EFFECT = 0,
    ALWAYS_LOSE,
    ALWAYS_WIN,
    WIN_5_TIMES,
    //ROCK EFFECTS
    NO_DMG,
    ALWAYS_WIN_IF_LOSS,
    ALWAYS_TIE,
    HEAL,
    //PAPER EFFECTS
    SEE_NEXT_CARD,
    VIEW_DECK,
    BOT_PLAY_SAME_CARD,
    ATTACK_3_TIMES,
    //SCISSORS EFFECTS
    WIN_DEAL_2X_DMG,
    DEAL_TAKE_5X_DMG,
    HEALTH_DEAL_2X_DMG,
    TIE_DEAL_3X_DMG,
};


enum dmgType{
    DMG_0_TYPE = 0,
    DMG_15_TYPE,
    DMG_25_TYPE,
    DMG_45_TYPE,
    DMG_50_TYPE,
    DMG_75_TYPE,
    DMG_100_TYPE,
    HEAL_50_TYPE,
    NO_DMG_GET,
};


/////////////////////////////////////////////////
//Display engine

#define RGB565_BLACK      0x0000
#define RGB565_RED        0xF800
#define RGB565_GREEN      0x7E0
#define RGB565_BLUE       0x1F
#define RGB565_YELLOW     0xFFE0
#define RGB565_WHITE      0xFFFF

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
///////////////////////////////////////////////////
//For first display


uint16_t dispArea[128][128]; //main screen buffer
uint16_t newBuf[128][128]; //global temp_1 buffer
uint16_t bigBuffer[160][128]; //calculated big buffer for both screen

// Format: cmd length (including cmd byte), post delay in units of 5 ms, then cmd payload
// Note the delays have been shortened a little
static const uint8_t init_cmd[] = {
        1, 20, 0x01,                        // Software reset
        1, 10, 0x11,                        // Exit sleep mode
        2, 2, 0x3a, 0x55,                   // Set colour mode to 16 bit
        2, 0, 0x36, 0x08,                   // Set MADCTL: row then column, refresh is bottom to top ????
        5, 0, 0x2a, 0x00, 0x02, 0x00, SCREEN_WIDTH+1, //SCREEN_WIDTH >> 8, SCREEN_WIDTH & 0xff,   // CASET: column addresses
        5, 0, 0x2b, 0x00, 0x00, 0x00, SCREEN_HEIGHT-1, //SCREEN_HEIGHT >> 8, SCREEN_HEIGHT & 0xff, // RASET: row addresses
        1, 2, 0x20,                         // Inversion on, then 10 ms delay (supposedly a hack?)
        1, 2, 0x13,                         // Normal display on, then 10 ms delay
        1, 2, 0x29,                         // Main screen turn on, then wait 500 ms
        0                                   // Terminate list
};

/*
    0x2A,   4,              //  1: Column addr set, 4 args, no delay:
      0x00, 0x00,                   //     XSTART = 0
      0x00, 0x4F,                   //     XEND = 79
    0x2B,   4,              //  2: Row addr set, 4 args, no delay:
      0x00, 0x00,                   //     XSTART = 0
      0x00, 0x9F };                 //     XEND = 159
*/

static inline void lcd_set_dc_cs(bool dc, bool cs) {
    sleep_us(1);
    gpio_put_masked((1u << PIN_DC) | (1u << PIN_CS), !!dc << PIN_DC | !!cs << PIN_CS);
    sleep_us(1);
}

static inline void lcd_write_cmd(PIO pio, uint sm, const uint8_t *cmd, size_t count) {
    st7789_lcd_wait_idle(pio, sm);
    lcd_set_dc_cs(0, 0);
    st7789_lcd_put(pio, sm, *cmd++);
    if (count >= 2) {
        st7789_lcd_wait_idle(pio, sm);
        lcd_set_dc_cs(1, 0);
        for (size_t i = 0; i < count - 1; ++i)
            st7789_lcd_put(pio, sm, *cmd++);
    }
    st7789_lcd_wait_idle(pio, sm);
    lcd_set_dc_cs(1, 1);
}

static inline void lcd_init(PIO pio, uint sm, const uint8_t *init_seq) {
    const uint8_t *cmd = init_seq;
    while (*cmd != 0) {
        //printf("%x %d\n", cmd, *cmd);
        lcd_write_cmd(pio, sm, cmd + 2, *cmd);
        sleep_ms(*(cmd + 1) * 5);
        cmd += *cmd + 2;
    }
}

static inline void st7789_start_pixels(PIO pio, uint sm) {
    uint8_t cmd = 0x2c; // RAMWR
    lcd_write_cmd(pio, sm, &cmd, 1);
    lcd_set_dc_cs(1, 0);
}


//////////////////////////////////////////////////////////////
//Function for 2nd display...


static const uint8_t init_cmd_display_2[] = {
        1, 20, 0x01,                        // Software reset
        1, 10, 0x11,                        // Exit sleep mode
        2, 2, 0x3a, 0x55,                   // Set colour mode to 16 bit
        2, 0, 0x36, 0x08,                   // Set MADCTL: row then column, refresh is bottom to top ????
        5, 0, 0x2a, 0x00, 26, 0x00, 105, //SCREEN_WIDTH >> 8, SCREEN_WIDTH & 0xff,   // CASET: column addresses
        5, 0, 0x2b, 0x00, 0x00, 0x00, 160, //SCREEN_HEIGHT >> 8, SCREEN_HEIGHT & 0xff, // RASET: row addresses
        1, 2, 0x21,                         // Inversion on, then 10 ms delay (supposedly a hack?)
        1, 2, 0x13,                         // Normal display on, then 10 ms delay
        1, 2, 0x29,                         // Main screen turn on, then wait 500 ms
        0                                   // Terminate list
};


static inline void lcd_set_dc_cs_2(bool dc, bool cs) {
    sleep_us(1);
    gpio_put_masked((1u << PIN_DC_2) | (1u << PIN_CS_2), !!dc << PIN_DC_2 | !!cs << PIN_CS_2);
    sleep_us(1);
}

static inline void lcd_write_cmd_2(PIO pio, uint sm, const uint8_t *cmd, size_t count) {
    st7789_lcd_wait_idle(pio, sm);
    lcd_set_dc_cs_2(0, 0);
    st7789_lcd_put(pio, sm, *cmd++);
    if (count >= 2) {
        st7789_lcd_wait_idle(pio, sm);
        lcd_set_dc_cs_2(1, 0);
        for (size_t i = 0; i < count - 1; ++i)
            st7789_lcd_put(pio, sm, *cmd++);
    }
    st7789_lcd_wait_idle(pio, sm);
    lcd_set_dc_cs_2(1, 1);
}

static inline void lcd_init_2(PIO pio, uint sm, const uint8_t *init_seq) {
    const uint8_t *cmd = init_seq;
    while (*cmd != 0) {
        //printf("%x %d\n", cmd, *cmd);
        lcd_write_cmd_2(pio, sm, cmd + 2, *cmd);
        sleep_ms(*(cmd + 1) * 5);
        cmd += *cmd + 2;
    }
}

static inline void st7789_start_pixels_2(PIO pio, uint sm) {
    uint8_t cmd = 0x2c; // RAMWR
    lcd_write_cmd_2(pio, sm, &cmd, 1);
    lcd_set_dc_cs_2(1, 0);
}






//////////////////////////////////////////////////
// sprite engine 

//sprite type
typedef struct sprite sprite;
struct sprite {
    uint8_t index; //frame index
    uint8_t maxFrames; //maxframe calculated in loadSprite()
    uint16_t winIncrement; // window buffer increment to load frame
    uint8_t *spriteBuffer; //raw buffer
    uint16_t *paletteBuffer; //raw palette buffer
    uint8_t width; //sprite width
    uint8_t height; //sprite height
};



//////////////////////////////////////////////////
// game engine 

//sprite type
typedef struct card card;
struct card {
    uint8_t type; //frame index
    uint8_t index;
};




//predefined divided values (reduce time to calculate and avoid using divide instruction -> use multiply instead)
const float inv31 = 1/31.0;
const float inv63 = 1/31.0;
const float msRef = 1/1000.0;


//prepare a sprite
void loadSprite(sprite * spr, uint8_t *buffer, uint32_t bufferSize, uint16_t *palette, uint8_t window_width, uint8_t window_height)
{
    spr->spriteBuffer = buffer;
    spr->paletteBuffer = palette;
    spr->width = window_width;
    spr->height = window_height;
    spr->winIncrement = window_height * window_width;
    spr->maxFrames = bufferSize/(window_width*window_height);
    spr->index = 0;
}

//place sprite on main screen
void placeSprite(sprite *spr, int16_t x, int16_t y, uint8_t mask, uint8_t lowBright)
{
    uint32_t k = spr->winIncrement * spr->index; //set frame address
    uint16_t r, g, b;
    for(int j = y; j < y+spr->height; j++)
    {
        for(int i = x; i < x+spr->width; i++)
        {
            if(i < SCREEN_WIDTH && i >= 0)
            {
                if(j >= 0 && j < SCREEN_HEIGHT) //if cursor is inside the area
                {
                    if(spr->spriteBuffer[k] != mask) //check if mask color detected don't draw it (transparency...), execpt 0x00
                    {
                        if(lowBright)
                        {
                            r = (float)((spr->paletteBuffer[spr->spriteBuffer[k]] & 0xf800) >> 11) * 0.25;
                            g = (float)((spr->paletteBuffer[spr->spriteBuffer[k]] & 0x7e0) >> 5) * 0.25;
                            b = (float)(spr->paletteBuffer[spr->spriteBuffer[k]] & 0x1f) * 0.25;
                            dispArea[j][i] = (r << 11) | (g <<  5) | b; //read sprite buffer
                        }
                        else
                            dispArea[j][i] = spr->paletteBuffer[spr->spriteBuffer[k]]; //read sprite buffer
                    }
                        
                }
            }
            
            k++; //increment buffer index 
        }
    }
}

//method to reduce contrast to all previous layers called
void layerContrast(float lvl)
{
    uint16_t r, g, b;
    for(int y = 0; y < 128; y++)
    {
        for(int x = 0; x < 128; x++)
        {
            //force first LSB bit to one to each color to accept fade white
            r = (uint16_t)((float)(((dispArea[y][x] & 0xf800) | 0x800) >> 11) * lvl);
            g = (uint16_t)((float)(((dispArea[y][x] & 0x7e0) | 0x60) >> 5)  * lvl);
            b = (uint16_t)((float)((dispArea[y][x] & 0x1f) | 0x01) * lvl);

            if(r > 31)
                r = 31;

            if(g > 63)
                g = 63;

            if(b > 31)
                b = 31;
            //r = conv_5 * r * inv31;
            //g = conv_6 * g * inv63;
            //b = conv_5 * b * inv31;
            dispArea[y][x] = (r << 11) | (g << 5) | b;
        }   
    }
}

//method to clear the main screen
void clearLayers(void)
{
    for(int j = 0; j < SCREEN_HEIGHT; j++)
        for(int i = 0; i < SCREEN_WIDTH; i++)
            dispArea[j][i] = 0x00;
}

//method to display sprite card on second screen
void loadCardSprite(uint8_t *bufIn, uint16_t *paletteIn, uint8_t frame, PIO l_pio, uint l_sm, float lvl) //here we know the size of the display...
{
    
    int k = 0;
    uint16_t r, g, b;

    st7789_start_pixels_2(l_pio, l_sm);
    for (int y = 0; y < SCREEN_HEIGHT_2; ++y) 
    {
        for (int x = 0; x < SCREEN_WIDTH_2; ++x) 
        {
            uint16_t colour = paletteIn[bufIn[k + 12800 * frame]];
            r = (uint16_t)((float)(((colour & 0xf800) | 0x800) >> 11) * lvl);
            g = (uint16_t)((float)(((colour & 0x7e0) | 0x20) >> 5)  * lvl);
            b = (uint16_t)((float)((colour & 0x1f) | 0x01) * lvl);

            if(r > 31)
                r = 31;

            if(g > 63)
                g = 63;

            if(b > 31)
                b = 31;
            colour = (r << 11) | (g << 5) | b;
            
            st7789_lcd_put(l_pio, l_sm, colour >> 8);
            st7789_lcd_put(l_pio, l_sm, colour & 0xff);
            k++;
        }
    }

}


//method to read cards sprite and scale it in a predefined size
void loadCardSprite_small(uint8_t *bufIn, uint8_t frame, uint8_t bufOut[2120])
{
    //uint8_t localBuf[160][80];
    uint32_t k = 0;
    //At first read card at full size
    for(int j = 0; j < 160; j++)
        for(int i = 0; i < 80; i++)
        {
            bigBuffer[j][i] = bufIn[frame * 12800 + k];
            k++;
        }
            

    k = 0; //reset cnt

    for(int j = 0; j < 159; j+=3)
        for(int i = 0; i < 80; i+=2)
        {
            bufOut[k] = bigBuffer[j][i]; //fill buffer at scaled size
            k++;
        }
    
}



//draw box method (xp : start x pos, yp : same as y pos, width, height, color_rgb565, buffer to write)
void drawBox(int xp, int yp, int w, int h, uint16_t c)
{
    for(int y = yp; y < yp+h; y++)
        for(int x = xp; x < xp+w; x++)
        {
            if(x >= 0 && x < SCREEN_WIDTH)
            {
                if(y >= 0 && y < SCREEN_HEIGHT)
                    dispArea[y][x] = c;
            }
        }
}


//scale algorythm
void displayScale(uint8_t factor) 
{
    //Algo scale BMP du pauvre (nearest neighbour)
    float delta = 128.0/(float)(128.0+factor);
    
    uint8_t i_delta;
    float offset = 0.0;
    offset = 0.0; //reset value

    //do on x axis
    for(int j = 0; j < 128; j++)
    {
        //new[i] = old[ int(offset) ]; //attention à bien cast le int sinon l'adressage sur un float va tout casser(⌐⊙_⊙)╭∩_╮
        //offset += delta;
        offset = delta*(factor/2); //reset value
        for(int i = 0; i < 128; i++)
        {
            i_delta = (int)(offset);
            //printf("scale debug : %f %d\n", offset, i_delta);
            newBuf[j][i] = dispArea[j][i_delta];
            offset += delta; //increment next pixel
        }
    }

    for(int i = 0; i < 128; i++)
    {
        for(int j = 0; j < 128; j++)
        {
            dispArea[j][i] = newBuf[j][i];
        }
    }

    //do on y axis
    for(int i = 0; i < 128; i++)
    {
        //new[i] = old[ int(offset) ]; //attention à bien cast le int sinon l'adressage sur un float va tout casser(⌐⊙_⊙)╭∩_╮
        //offset += delta;
        offset = 0.0; //reset value
        for(int j = 0; j < 128; j++)
        {
            i_delta = (int)(offset);
            //printf("scale debug : %f %d\n", offset, i_delta);
            newBuf[j][i] = dispArea[i_delta][i];
            offset += delta; //increment next pixel
        }
    }


    for(int i = 0; i < 128; i++)
    {
        for(int j = 0; j < 128; j++)
        {
            dispArea[j][i] = newBuf[j][i];
        }
    }
}





void placeSpriteScaleX(uint8_t bufIn[2120], uint16_t *paletteIn, uint8_t w, uint8_t h, int16_t x, int16_t y, int8_t factor) 
{

   // factor = 0;

    //Algo scale BMP du pauvre (nearest neighbour)
    float delta = (float)w/(float)(w+factor);
    
    uint8_t i_delta;
    uint16_t k = 0;
    float offset = 0.0;

    offset = 0.0; //reset value

    if(factor >= w)
    {
        factor = w-2;
    }
    

    //printf("Debug : %d %d\n", h, w);

    for(int j = 0; j < h; j++)
        for(int i = 0; i < w; i++)
        {
            bigBuffer[j][i] = paletteIn[bufIn[k]];  //sprite attribute
            k++;
        }
        
        for(int j = 0; j < 128; j++)
            for(int i = 0; i < 128; i++)
                newBuf[j][i] = 0x07e0;  //attribute local alpha channel

    //do on x axis
    for(int j = 0; j < 128; j++)
    {
        //new[i] = old[ int(offset) ]; //attention à bien cast le int sinon l'adressage sur un float va tout casser(⌐⊙_⊙)╭∩_╮
        //offset += delta;
        offset = delta*(factor/1); //reset value
        for(int i = 0; i < w; i++)
        {
            i_delta = (int)(offset);
            //printf("scale debug : %f %d\n", offset, i_delta);
            newBuf[j][i] = bigBuffer[j][i_delta];
            offset += delta; //increment next pixel
        }
    }

    //factor <<= 1; //divide factor by 2

    for(int j = 0; j < 128; j++)
        for(int i = 0; i < 128; i++)
        {
            if(newBuf[j][i] != 0x7e0) //alpha channel filter
            {
                if(i+x >= 0 && i+x < 128)
                {
                    if(j+y >= 0 && j+y < 128)
                    {
                        if(i > abs(factor) && j < h) //resize took into account
                            dispArea[j+y][i+x] = newBuf[j][i]; //fill that buffer 
                    }
                }
            }
        }


}



////////////////////////////////////////////////////////////////////////
//other functions


//Comparision between cards according rules (win, tie or lose and effects)
void compareCards(card card_p, card card_b, uint8_t *r_result, uint8_t *effect)
{

    /*
    //COMMON EFFECTS
    NO_EFFECT = 0,
    ALWAYS_LOSE,
    ALWAYS_WIN,
    WIN_5_TIMES,
    //ROCK EFFECTS
    NO_DMG,
    ALWAYS_TIE,
    ALWAYS_WIN_IF_LOSS,
    HEAL,
    //PAPER EFFECTS
    SEE_NEXT_CARD,
    VIEW_DECK,
    BOT_PLAY_SAME_CARD,
    ATTACK_3_TIMES,
    //SCISSORS EFFECTS
    WIN_DEAL_2X_DMG,
    DEAL_TAKE_5X_DMG,
    HEALTH_DEAL_2X_DMG,
    TIE_DEAL_3X_DMG,
};
    
    
    */


    *effect = NO_EFFECT; //set by default no effect

    switch(card_p.type) 
    {
        case ROCK_TYPE:
            //compare cards type at first ! simple
            if(card_b.type == PAPER_TYPE)
            {
                *r_result = LOSE_TYPE;
            }
            else if(card_b.type == SCISSORS_TYPE)
            {
                *r_result = WIN_TYPE;
            }
            else //rock
            {
                *r_result = TIE_TYPE;
            }

            //Check effects

            if(card_p.index == 1) 
            {
                *effect = NO_DMG;
            }
            else if(card_p.index == 2)
            {
                *effect = ALWAYS_TIE;
            }
            else if(card_p.index == 3)
            {
                *effect = ALWAYS_WIN_IF_LOSS;
            }
            else if(card_p.index == 4)
            {
                *effect = ALWAYS_LOSE;
            }
            else if(card_p.index == 5)
            {
                *effect = HEAL;
            }
            else
            {
                *effect = NO_EFFECT;
            }


        break;

        case PAPER_TYPE:
            //compare cards type at first ! simple
            if(card_b.type == PAPER_TYPE)
            {
                *r_result = TIE_TYPE;
            }
            else if(card_b.type == SCISSORS_TYPE)
            {
                *r_result = LOSE_TYPE;
            }
            else //rock
            {
                *r_result = WIN_TYPE;
            }

            //Check effects

            if(card_p.index == 0) 
            {
                *effect = SEE_NEXT_CARD;
            }
            else if(card_p.index == 1)
            {
                *effect = VIEW_DECK;
            }
            else if(card_p.index == 3)
            {
                *effect = ALWAYS_LOSE;
            }
            else if(card_p.index == 4)
            {
                *effect = BOT_PLAY_SAME_CARD;
            }
            else if(card_p.index == 5)
            {
                *effect = ATTACK_3_TIMES;
            }
            else
            {
                *effect = NO_EFFECT;
            }

        break;

        case SCISSORS_TYPE:
            //compare cards type at first ! simple
            if(card_b.type == PAPER_TYPE)
            {
                *r_result = WIN_TYPE;
            }
            else if(card_b.type == SCISSORS_TYPE)
            {
                *r_result = TIE_TYPE;
            }
            else //rock
            {
                *r_result = LOSE_TYPE;
            }


            //Check effects

            if(card_p.index == 0) 
            {
                *effect = WIN_DEAL_2X_DMG;
            }
            else if(card_p.index == 1)
            {
                *effect = DEAL_TAKE_5X_DMG;
            }
            else if(card_p.index == 3)
            {
                *effect = HEALTH_DEAL_2X_DMG;
            }
            else if(card_p.index == 4)
            {
                *effect = ALWAYS_LOSE;
            }
            else if(card_p.index == 5)
            {
                *effect = TIE_DEAL_3X_DMG;
            }
            else
            {
                *effect = NO_EFFECT;
            }

        break;

        default: //Special cards
            *r_result = WIN_TYPE;

            //Check effects
            if(card_p.index == 0) 
            {
                *effect = NO_EFFECT;
            }
            else
            {
                *effect = WIN_5_TIMES;
            }

    }
}






uint32_t generate_rand(uint32_t max)
{
    uint32_t number = 0;
    for(int i = 0; i < 100; i++)
    {
        number = (rand() % max);
    }
    return number;
}


///////////////////////////////////////////////////////////////////////////////////////////////////////
//Sound engine

uint32_t multicore_1_data = 0;
uint32_t multicore_0_data = 0;
uint8_t check_core = 0;

uint8_t soundOut;
int8_t *snd_channel = choose;
int8_t *ost_channel;
uint32_t snd_size = sizeof(choose);
uint32_t ost_size = 0;
uint32_t snd_cnt = 0;
uint32_t ost_cnt = 0;
uint32_t timer_cnt = 0;
bool flag_snd = false;
float volumeMenu = 1.0;

void on_pwm_wrap()
{
    soundOut = (ost_channel[ost_cnt]*volumeMenu) + (snd_channel[snd_cnt]*2) + SOUND_OFFSET;

    if(timer_cnt > 8)
    {

        
        if(snd_cnt < snd_size-2)
        {
            snd_cnt++;
        }

        ost_cnt++;
        if(ost_cnt >= ost_size-2)
        {
            ost_cnt = 0;
        }
        timer_cnt = 0;
    }
    timer_cnt++;
    pwm_clear_irq(pwm_gpio_to_slice_num(SND_PIN));
    //Send new value of PWM
    pwm_set_gpio_level(SND_PIN, soundOut);
}

    



void playSound(uint8_t *input, uint32_t size)
{
    snd_channel = input; 
    snd_cnt = 0; //reset cnt
    snd_size = size;
}


void playOst(uint8_t *input, uint32_t size)
{
    ost_channel = input;
    ost_cnt = 0; //reset cnt
    ost_size = size;
}





///////////////////////////////////////////////////////////////////////////////////////////////////////
//Main program



uint32_t k = 0;

    uint8_t cardCount = 0;

    
    uint8_t dispSecond[12800]; //buffer for cards
    uint32_t timer_1 = 0;

    uint16_t adc_temp = 0;
    uint8_t joystick_state = 0;
    uint8_t button_state = 0;

    float theta = 0.f;
    //float theta_max = 2.f * (float) M_PI;
    float contrast = 1.0; //contrast for screen 1
    float contrast_2 = 1.0; //contrast for screen 2
    
    bool allowUpdateSecondScreen = true;

    //////////////////////////////////////////////////////////////
    //Sprite init

    sprite spr_bot_idle, spr_bot_knock;
    sprite spr_table;

    uint8_t bufferCard_r[2120];
    uint8_t bufferCard_p[2120];
    uint8_t bufferCard_s[2120];
    uint8_t bufferCard_x[2120]; 
    sprite spr_card_r, spr_card_p, spr_card_s, spr_card_x;


    sprite spr_menu;
    sprite spr_all_knowing;
    
    

    sprite spr_number, spr_text;

    ////////////////////////////////////
    //Game vars

    uint32_t randomSeed = 0;
    uint16_t readRandomValue = 0;

    float playerHp_conv = 50.0;
    float ennemyHp_conv = 100.0;
    uint8_t playerHp = 35;
    uint8_t ennemyHp = 120;


    uint8_t dmgTake = 0, dmgDeal = 0;

    uint8_t numberYposOffset = 10;
    int16_t cards_fight_delta_b = 0;
    int16_t cards_fight_delta_p = 0;
    uint8_t cardSelectedXposOffset = 0;
    uint8_t sequenceFightTime = 0;
    uint8_t deckYposOffset = 0;
    uint8_t sendAction = 0;
    uint8_t roundState = TIE_TYPE;
    uint8_t roundEffect;

    uint8_t selectedCard = 0;
    uint8_t botSelectedCard = 0;
    uint8_t playerSelectedCard = 0;
    uint8_t setScale;
    int8_t cardResize = 39; 

    uint8_t repeatFight = 0; //event when cards need to be in fight multiple times
    uint8_t fightCnt = 0;   //counter of that event

    bool fail_last_round = 0;
    bool show_deck = 0; //show opponent's deck after fight
    bool show_next_card = 0; //show next card played by opponent
    bool force_same_card = 0; //force opponent play same card

    card card_p[3];
    card card_b[3];
    card choosen_card_player, choosen_card_bot;

    bool menu = 1;
    uint8_t gameState = 0;
    uint8_t bootYOffset = 120;

    bool all_knowing_trig = false;
    
    int8_t screen_shakingX = 0;
    int8_t screen_shakingY = 0;
    uint8_t spr_bot_offsetY = 0;
    int8_t zoom_rand = 0;

    int8_t posXGlitch = 0;
    int8_t posYGlitch = 0;
    uint8_t zoomGlitch = 0;
    uint32_t timer_glitch = 0;

    bool gotRockAllKnowing = false;
    bool gotPaperAllKnowing = false;
    bool gotScissorsAllKnowing = false;
    bool swapCards = false;
    bool botChangedCard = false;
    uint16_t temp_16;
    uint8_t luckyNumber = 0;


int main() {
    stdio_init_all();

    //debug led init
    gpio_init(25); 
    gpio_set_dir(25, GPIO_OUT);




    PIO pio_1 = pio0;
    uint sm_1 = 0;
    uint offset_1 = pio_add_program(pio_1, &st7789_lcd_program);
    st7789_lcd_program_init(pio_1, sm_1, offset_1, PIN_DIN, PIN_CLK, SERIAL_CLK_DIV);
    PIO pio_2 = pio1;
    uint sm_2 = 0;
    uint offset_2 = pio_add_program(pio_2, &st7789_lcd_program);
    st7789_lcd_program_init(pio_2, sm_2, offset_2, PIN_DIN_2, PIN_CLK_2, SERIAL_CLK_DIV_2);

    

    
    gpio_init(PIN_CS);
    gpio_init(PIN_DC);
    gpio_init(PIN_RESET);
    gpio_init(PIN_CS_2);
    gpio_init(PIN_DC_2);
    gpio_init(PIN_RESET_2);
    gpio_init(PIN_BL);
    
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_set_dir(PIN_DC, GPIO_OUT);
    gpio_set_dir(PIN_RESET, GPIO_OUT);
    gpio_set_dir(PIN_CS_2, GPIO_OUT);
    gpio_set_dir(PIN_DC_2, GPIO_OUT);
    gpio_set_dir(PIN_RESET_2, GPIO_OUT);
    gpio_set_dir(PIN_BL, GPIO_OUT);

    gpio_put(25, 0);
    gpio_put(PIN_CS, 1);
    gpio_put(PIN_RESET, 1);
    gpio_put(PIN_CS_2, 1);
    gpio_put(PIN_RESET_2, 1);

    //Init displays
    lcd_init(pio_1, sm_1, init_cmd);
    lcd_init_2(pio_2, sm_2, init_cmd_display_2);
    gpio_put(PIN_BL, 1);

    gpio_put(25, 1);
    sleep_ms(1000);
    gpio_put(25, 0);
    sleep_ms(1000);


    //sprites load

    loadSprite(&spr_menu, menu_screen, sizeof(menu_screen), colorPalette_menu_screen, SPR_WIDTH_MENU_SCREEN, SPR_HEIGHT_MENU_SCREEN);
    loadSprite(&spr_all_knowing, all_knowing, sizeof(all_knowing), colorPalette_all_knowing, SPR_WIDTH_ALL_KNOWING, SPR_HEIGHT_ALL_KNOWING);

    loadSprite(&spr_bot_knock, bot_knock, sizeof(bot_knock), colorPalette_bot_knock, SPR_WIDTH_BOT_KNOCK, SPR_HEIGHT_BOT_KNOCK);
    loadSprite(&spr_bot_idle, bot_idle, sizeof(bot_idle), colorPalette_bot_idle, SPR_WIDTH_BOT_IDLE, SPR_HEIGHT_BOT_IDLE);
    loadSprite(&spr_table, table, sizeof(table), colorPalette_table, SPR_WIDTH_TABLE, SPR_HEIGHT_TABLE);
    

    //loadCardSprite_small(rock, 3, bufferCard);
    loadSprite(&spr_card_r, bufferCard_r, 2120, colorPalette_rock, 40, 53);
    loadSprite(&spr_card_p, bufferCard_p, 2120, colorPalette_paper, 40, 53);
    loadSprite(&spr_card_s, bufferCard_s, 2120, colorPalette_scissors, 40, 53);
    loadSprite(&spr_card_x, bufferCard_x, 2120, colorPalette_special, 40, 53);
    loadSprite(&spr_number, numbers, 612, colorPalette_text, 36, 17);
    loadSprite(&spr_text, text, 768, colorPalette_text, 48, 16);


    //ADC Part
    adc_init();

    //At first generate seed number
    adc_gpio_init(28); //init adc pin
    adc_select_input(2); //select adc pin 28
    adc_set_clkdiv(2); //div by 2

    //generate random base
    for(int i = 0; i < 10; i++)
        randomSeed += adc_read() % 5;

    srand(randomSeed);

    //switch to joystick
    adc_gpio_init(27); //init adc pin
    adc_select_input(1); //select adc pin 27
    adc_set_clkdiv(2); //div by 2

    

    //Button Part
    gpio_init(17);
    gpio_pull_up(17);
    gpio_set_dir(17, false);

    adc_temp = adc_read();


    //////////////////////////
    //Card test attribution

    //player cards
    card_p[0].type = ROCK_TYPE;
    card_p[0].index = 0;
    card_p[1].type = PAPER_TYPE;
    card_p[1].index = 2;
    //card_p[2].type = SCISSORS_TYPE;
    //card_p[2].index = 2;
    card_p[2].type = SCISSORS_TYPE;
    card_p[2].index = 2;

    //bot cards
    card_b[0].type = ROCK_TYPE;
    card_b[1].type = PAPER_TYPE;
    card_b[2].type = SCISSORS_TYPE;

    //choosen_card_bot.type = rand() % 3; 

    

    for(int i = 0; i < 5; i++)
    {
        temp_16 = generate_rand(100);
        //printf("lucky number : %d\n", temp_16);
        if(luckyNumber < temp_16)
        {
            luckyNumber = temp_16;
        }
    }

    if(luckyNumber >= 95) //at first, check if chance is offered to player
    {
        card_p[0].index = 6;
        card_p[1].index = 6;
        card_p[2].index = 6;
    }


    
    botSelectedCard = generate_rand(3); //generate random card played by bot...
    spr_table.index = spr_table.maxFrames-1;
    cards_fight_delta_p = 100; //prepare cards
    cards_fight_delta_b = 100; //prepare cards




    ///////////////////////////////////////////////////
    //Launch sound engine
    printf("Launch sound core\n");

    //////////////////////////////////////////////////////////////
    //Set sound CFG

    // Tell the LED pin that the PWM is in charge of its value.
    gpio_set_function(SND_PIN, GPIO_FUNC_PWM);
    // Figure out which slice we just connected to the LED pin
    uint slice_num = pwm_gpio_to_slice_num(SND_PIN);
    // Mask our slice's IRQ output into the PWM block's single interrupt line,
    // and register our interrupt handler
    pwm_clear_irq(slice_num);
    pwm_set_irq_enabled(slice_num, true);
    irq_set_exclusive_handler(PWM_IRQ_WRAP, on_pwm_wrap);
    irq_set_enabled(PWM_IRQ_WRAP, true);
    pwm_config config, config2;
    // Get some sensible defaults for the slice configuration. By default, the
    // counter is allowed to wrap over its maximum range (0 to 2**16-1)
    pwm_config_set_phase_correct(&config, false);
    pwm_config_set_clkdiv_int(&config, 4);
    pwm_config_set_clkdiv_mode(&config, PWM_DIV_FREE_RUNNING);
    pwm_config_set_output_polarity(&config, false, false);
    pwm_config_set_wrap(&config, 0xFF);
    // Load the configuration into our PWM slice, and set it running.
    pwm_init(slice_num, &config, true);


    ////////////////////////////////////////////////////////////////////////////////////////



    

    playOst(ost1, sizeof(ost1));
    //ost_channel = ost2;
    //ost_size = sizeof(ost2);


    while (1) {

        //debug
        /*card_p[2].type = PAPER_TYPE;
        card_p[2].index = 5;*/

        /////////////////////////////////////////////////////////////////////
        //Input part
        adc_temp = adc_read();
        if(button_state == 0)
        {
            if(!gpio_get(17))
            {
                printf("Button pressed");
                button_state = 1;
                if(sendAction == 0)
                {
                    playerSelectedCard = selectedCard;
                    sendAction = 1; //send card to fight
                    playSound(valid, sizeof(valid));
                }
            }
        }
        else
        {
            if(gpio_get(17))
            {
                printf("Button unpressed");
                button_state = 0;
            }
        }

        if((gameState == 4 && menu == 0) || menu == 1)
        {
            if(sendAction == 0) //let moving action if no cards played
            {
                if(joystick_state == 0)
                {
                    if(adc_temp > 3500)
                    {
                        printf("Left\n");
                        selectedCard--;
                        playSound(choose, sizeof(choose));
                        allowUpdateSecondScreen = true; //allow update screen
                        if(selectedCard > 3)
                            selectedCard = 2;
                        joystick_state = 1;
                    }
                    else if(adc_temp < 1000)
                    {
                        printf("Right\n");
                        joystick_state = 2;
                        selectedCard++;
                        playSound(choose, sizeof(choose));
                        allowUpdateSecondScreen = true; //allow update screen
                    }
                    else
                    {

                    }
                }
                else if(joystick_state == 1)
                {
                    if(adc_temp < 2500)
                    {
                        printf("Center\n");
                        joystick_state = 0;
                    }
                }
                else
                {
                    if(adc_temp > 1500)
                    {
                        printf("Center\n");
                        joystick_state = 0;
                    }
                }

                selectedCard = selectedCard % 3;
            }
        }
        
        


        

        //theta += 0.02f;
        //contrast = 0.4 * sin(2 * M_PI * theta) + 0.5;
        

        //Play with HP lmao....
        playerHp = playerHp_conv * 35/50.0;
        ennemyHp = (100-ennemyHp_conv) * 120/100.0;
        
        rand(); //feed random number...
        

        ///////////////////////////////////////////////////////////////////////////////
        //Display  part

        st7789_start_pixels(pio_1, sm_1); //open display buffer


        //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        //Sprite layer work (mean first instruction call -> bottom layer / last instruction -> top layer)


        //Glitch effect every 0.5
        if(abs(time_us_32() - timer_glitch) >= 500000)
        {
            zoomGlitch = rand() % 3;
            posXGlitch = (rand() % 3) - 1;
            posYGlitch = (rand() % 3) - 1;
            printf("bot cards : %d %d %d %d\n", card_b[0].type, card_b[1].type, card_b[2].type, botSelectedCard);
            timer_glitch = time_us_32();
        }

        

        if(!menu)
        {

            if(gameState == 0)
            {
                if(contrast > 1.0)
                {
                    contrast -=5.0;
                    contrast_2 = contrast;
                    allowUpdateSecondScreen = true;
                }
                else
                {
                    playSound(pick, sizeof(pick));
                    spr_table.index = spr_table.maxFrames-1; //prepare animation
                    gameState = 1;
                }
                bootYOffset = 120;
                //selectedCard = 3; //force to no cards selected...
            }
            else if(gameState == 1)
            {
                if(spr_table.index > 0)
                {
                    spr_table.index--;
                }
                if(spr_table.index == 0)
                {
                    playSound(pick, sizeof(pick));
                    spr_table.index = spr_table.maxFrames-1; //prepare animation
                    gameState = 2;
                }
                bootYOffset = 120;
                //selectedCard = 3; //force to no cards selected...
            }
            else if(gameState == 2)
            {
                if(spr_table.index > 0)
                {
                    spr_table.index--;
                }
                if(spr_table.index == 0)
                {
                    playSound(pick, sizeof(pick));
                    spr_table.index = spr_table.maxFrames-1; //prepare animation
                    gameState = 3;
                }
                bootYOffset = 120;
                //selectedCard = 3; //force to no cards selected...
            }
            else if(gameState == 3)
            {
                if(spr_table.index > 0)
                {
                    spr_table.index--;
                }
                if(spr_table.index == 0) //once card got, show deck
                {
                    if(bootYOffset > 0) 
                    {
                        bootYOffset-=5;
                    }
                    else //deck here ? Start the game
                    {
                        gameState = 4;
                    }
                }
                else
                {
                    bootYOffset = 120;
                }
                //selectedCard = 3; //force to no cards selected...
            }
            else if(gameState == 4)
            {
                //supposed to be in game
            }
            //printf("game debug : %d %d\n", gameState, spr_table.index);

            clearLayers(); //clear all sprites
            //put sprite here
            //
            if(spr_bot_knock.index > 0) //back to zero to loop when last frame reached
            {
                placeSprite(&spr_bot_knock, 10+posXGlitch, 54+posYGlitch, 0x00, false); //0x00 -> no transparency
                if(abs(time_us_32()*msRef - timer_1) >= 100)
                {
                    spr_bot_knock.index--;
                    spr_bot_idle.index = spr_bot_idle.maxFrames-1; //reset other sprite index
                    timer_1 = time_us_32()*msRef;
                }
            }
            else //place idle sprite
            {
                placeSprite(&spr_bot_idle, 10+screen_shakingX+posXGlitch, 54+screen_shakingY-spr_bot_offsetY+posYGlitch, 0x00, false); //0x00 -> no transparency

                if(abs(time_us_32()*msRef - timer_1) >= 100)
                {
                    spr_bot_idle.index--;
                    if(spr_bot_idle.index > spr_bot_idle.maxFrames) //back to zero to loop when last frame reached
                    {
                        spr_bot_idle.index = spr_bot_idle.maxFrames-1;
                    }
                    timer_1 = time_us_32()*msRef;
                }
            }
            
            //printf("time : %d\t%d\n", time_us_32(), timer_1);

            //layerContrast(dispArea, contrast);
            placeSprite(&spr_table, screen_shakingX+posXGlitch, screen_shakingY+posYGlitch, 0xFE, false);

            layerContrast(contrast);

                

            ///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
            // Cards and deck selection (and display method), and contrast

            

            if(sendAction == 1)
            {
                flag_snd = false; //reset flag sound
                cardResize = -40; //set minimum scale for cards appearing
                cards_fight_delta_p = 100; //prepare pos
                //attribute cards played
                
                
                choosen_card_bot.type = card_b[botSelectedCard].type;
                choosen_card_player.type = card_p[playerSelectedCard].type;
                choosen_card_player.index = card_p[playerSelectedCard].index;
                //printf("Debug player : %d %d %d %d\n", playerSelectedCard, selectedCard, choosen_card_player.type, card_p[playerSelectedCard].type);
                allowUpdateSecondScreen = false; //at first, disable refresh 2nd display...
                if(cardSelectedXposOffset < 80)
                {
                    cardSelectedXposOffset+=5;
                }
                else
                {
                    if(deckYposOffset < 120)
                    {
                        deckYposOffset+=5;
                        if(deckYposOffset > 40) //lower bright
                        {
                            if(contrast > 0.2)
                            {
                                contrast-=0.1;
                            }
                        }
                    }
                    else
                    {                       
                        sendAction = 2; //next action
                        if(all_knowing_trig) //alternate win !
                        {
                            sendAction = 13;
                        }
                    }
                }
            }
            else if(sendAction == 2)
            {
                //add card fight sequence
                if(cards_fight_delta_p > 20)
                {
                    cards_fight_delta_p-=5;
                }
                else
                {
                    //wait sequence
                    sequenceFightTime++;
                    if(sequenceFightTime > 10)
                    {
                        sequenceFightTime = 0;
                        sendAction = 3; //switch next step
                    }
                }
                cards_fight_delta_b = cards_fight_delta_p;
            }
            else if(sendAction == 3)
            {
                if(cards_fight_delta_p < 40)
                {
                    cards_fight_delta_p+=2;
                }
                else
                {
                    sendAction = 4;
                }
                cards_fight_delta_b = cards_fight_delta_p;
            }
            else if(sendAction == 4)
            {
                if(cards_fight_delta_p > 0)
                {
                    cards_fight_delta_p-=10;
                }
                else
                {
                    //////////////////////////////////////
                    //Compare cards
                    compareCards(choosen_card_player, choosen_card_bot, &roundState, &roundEffect);

                    //A first attribute damage before effect
                    if(roundState == WIN_TYPE)
                    {
                        dmgTake = NO_DMG_GET;
                        dmgDeal = DMG_50_TYPE;
                    }
                    else if(roundState == LOSE_TYPE)
                    {
                        dmgTake = DMG_50_TYPE;
                        dmgDeal = NO_DMG_GET;
                    }
                    else //TIE
                    {
                        dmgTake = DMG_15_TYPE;
                        dmgDeal = DMG_15_TYPE;
                    }
                    
                    //add effect here
                    switch(roundEffect)
                    {
                        case ALWAYS_LOSE:
                            roundState = LOSE_TYPE;
                            dmgTake = DMG_50_TYPE;
                            dmgDeal = NO_DMG_GET;
                        break;

                        case ALWAYS_WIN:
                            roundState = WIN_TYPE;
                            dmgTake = NO_DMG_GET;
                            dmgDeal = DMG_50_TYPE;
                        break;

                        case WIN_5_TIMES:
                            repeatFight = 4; //repeat 5 times
                            roundState = WIN_TYPE;
                            dmgTake = NO_DMG_GET;
                            dmgDeal = DMG_25_TYPE;
                        break;

                        case NO_DMG:
                            if(roundState == LOSE_TYPE || roundState == TIE_TYPE)
                            {
                                dmgTake = DMG_0_TYPE;
                                dmgDeal = NO_DMG_GET;
                            }
                        break;

                        case ALWAYS_WIN_IF_LOSS:
                            if(fail_last_round) //if last round failed : Force win !
                            {
                                dmgTake = NO_DMG_GET;
                                dmgDeal = DMG_50_TYPE;
                                roundState = WIN_TYPE;
                            }
                        break;

                        case ALWAYS_TIE:
                            if(playerHp_conv < 25)
                            {
                                dmgTake = DMG_15_TYPE;
                                dmgDeal = DMG_15_TYPE;
                                roundState = TIE_TYPE;
                            }                        
                        break;

                        case HEAL:
                            if(roundState == WIN_TYPE)
                            {
                                dmgTake = HEAL_50_TYPE;
                                dmgDeal = DMG_50_TYPE;
                                //roundState = TIE_TYPE;
                            }                        
                        break;

                        case SEE_NEXT_CARD:
                            show_next_card = true; //tell to the game to show next card played by the bot
                        break;

                        case VIEW_DECK:
                            show_deck = true; //tell to the game to show opponent's deck                     
                        break;

                        case BOT_PLAY_SAME_CARD:
                            force_same_card = true; //bot will play same card
                            if(roundState == WIN_TYPE)
                            {
                                dmgTake = NO_DMG_GET;
                                dmgDeal = DMG_50_TYPE;
                                roundState = TIE_TYPE;
                            }                        
                        break;

                        case ATTACK_3_TIMES:
                            repeatFight = 2; //repeat 3 times                  
                        break;

                        case WIN_DEAL_2X_DMG:
                            if(roundState == WIN_TYPE)
                            {
                                dmgTake = NO_DMG_GET;
                                dmgDeal = DMG_100_TYPE;
                            }                        
                        break;

                        case DEAL_TAKE_5X_DMG:
                            if(roundState == TIE_TYPE)
                            {
                                dmgTake = DMG_75_TYPE;
                                dmgDeal = DMG_75_TYPE;
                            }                        
                        break;

                        case HEALTH_DEAL_2X_DMG:
                            if(playerHp_conv < 25 && roundState == WIN_5_TIMES)
                            {
                                //dmgTake = DMG_75_TYPE;
                                dmgDeal = DMG_100_TYPE; //check if 30 Damage sprite exist !
                            }                        
                        break;

                        case TIE_DEAL_3X_DMG:
                            if(roundState == TIE_TYPE)
                            {
                                dmgTake = DMG_15_TYPE;
                                dmgDeal = DMG_45_TYPE;
                            }                        
                        break;

                        default: // NO_EFFECT
                    }

                    //////////////////////////////////////
                    numberYposOffset = 0; //show text !
                    if(roundState == WIN_TYPE || roundState == TIE_TYPE)
                    {
                        spr_bot_knock.index = spr_bot_knock.maxFrames-1; //change bot's sprite by reseting the counter
                    }

                    if(roundState == LOSE_TYPE)
                    {
                        playSound(fail, sizeof(fail));
                    }
                    else if(roundState == TIE_TYPE)
                    {
                        playSound(tie, sizeof(tie));
                    }
                    else //WIN_TYPE
                    {
                        if(choosen_card_player.type == ROCK_TYPE)
                        {
                            playSound(success_rock, sizeof(success_rock));
                        }
                        else if(choosen_card_player.type == PAPER_TYPE)
                        {
                            playSound(success_papers, sizeof(success_papers));
                        }
                        else if(choosen_card_player.type == SCISSORS_TYPE)
                        {
                            playSound(success_scissors, sizeof(success_scissors));
                        }
                        else //SPECIAL_TYPE
                        {
                            playSound(success_special, sizeof(success_special));
                        }
                    }

                    sendAction = 5; //next step
                }
                cards_fight_delta_b = cards_fight_delta_p;
            }
            else if(sendAction == 5) //cards collision
            {

                if(roundState == TIE_TYPE) //TIE ? -> Both cards get out
                {
                    fail_last_round = false; //no fail in last round
                    if(cards_fight_delta_p < 100)
                    {
                        cards_fight_delta_p+=20;
                    }
                    else
                    {
                        //cardResize = -38; //set minimum scale for cards appearing
                        sendAction = 6;
                    }
                    cards_fight_delta_b = cards_fight_delta_p;
                }
                else if(roundState == WIN_TYPE)
                {
                    fail_last_round = false; //no fail in last round
                    if(cards_fight_delta_b < 100)
                    {
                        cards_fight_delta_b+=20;
                    }
                    else
                    {
                        if(cards_fight_delta_p < 150)
                        {
                            cards_fight_delta_p+=10;
                        }
                        else
                        {
                            //cardResize = -38; //set minimum scale for cards appearing
                            sendAction = 6;
                        }
                    }
                }
                else // LOSE
                {
                    fail_last_round = true; //last round failed
                    if(cards_fight_delta_p < 150)
                    {
                        cards_fight_delta_p+=20;
                    }
                    else
                    {
                        if(cards_fight_delta_b < 100)
                        {
                            cards_fight_delta_b+=10;
                        }
                        else
                        {
                            //cardResize = -38; //set minimum scale for cards appearing
                            sequenceFightTime = 0; //if no timer needed , prepare it...
                            sendAction = 6;   
                        }
                    }
                }
                if(sequenceFightTime < 1 && sendAction == 5)
                {
                    sequenceFightTime++;
                    setScale = 20;
                }
                else
                {
                    setScale = 0;
                }
            }
            else if(sendAction == 6)
            {

                ////////////////////////////////////////////////////////////////////
                //bot will pick a card here and prepare next one
                if(repeatFight == 0 || (repeatFight == fightCnt && repeatFight > 0))
                {
                    if(!force_same_card)
                    {
                        //printf("bot :\n");
                        if(!botChangedCard)
                        {
                            temp_16 = generate_rand(299)/100; 
                            //add method to pick a random card and random play !
                            card_b[botSelectedCard].type = temp_16;
                            temp_16 = generate_rand(299)/100; 
                            botSelectedCard = temp_16;
                            printf("Bro, I swapped cards %d\n", botSelectedCard);
                            botChangedCard = true;
                        }
                    }
                    /*else //if player choose same card effect, then no change here !
                    {
                        //force_same_card = false;
                    }*/
                }
                

                if(playerHp_conv <= 0 || ennemyHp_conv == 0) //skip special sequences here (except fight repetition)... game over !
                {
                    show_deck = false;
                    show_next_card = false;
                }   

                //printf("DEBUG Repeat fight %d\n", repeatFight);
                if(fightCnt < repeatFight) //Check if fight must be repeated...
                {
                    sendAction = 1; //repeat the same action...
                    fightCnt++;
                }
                else //if not, then finish event
                {
                    if(!show_deck && !show_next_card) //ensure no card must be displayed
                    {
                        if(deckYposOffset > 0) //put the deck back !
                        {

                            if(swapCards == 0) //do it one time !
                            {
                                //swap cards
                                if(selectedCard == 0)
                                {
                                    card_p[0].type = card_p[1].type;
                                    card_p[0].index = card_p[1].index;
                                    card_p[1].type = card_p[2].type;
                                    card_p[1].index = card_p[2].index;
                                }
                                else if(selectedCard == 1)
                                {
                                    card_p[1].type = card_p[2].type;
                                    card_p[1].index = card_p[2].index;
                                }
                                selectedCard = 2; //whatever, force last card
                                swapCards = true; //we swapped cards, set flag
                            }

                            if(contrast < 1.0)
                            {
                                contrast+=0.1;
                            }
                            else
                            {
                                if(ennemyHp_conv <= 0) //jump to boss defeated !
                                {
                                    playSound(boss_defeated, sizeof(boss_defeated));
                                    sendAction = 8; 
                                }
                                if(playerHp_conv <= 0) //jump to player defeated !
                                {
                                    sendAction = 10; 
                                }
                                deckYposOffset-=5;
                            }
                            spr_table.index = spr_table.maxFrames-1; //prepare animation
                        }
                        else
                        {

                            if(spr_table.index > 0) //back to zero to loop when last frame reached
                            {
                                    spr_table.index--; //play frame
                                    if(spr_table.index == 0) //at the end of animation generate a random card
                                    {
                                        readRandomValue = generate_rand(640); // 0->19 Rock, 20-39 -> Paper, 40->59 Scissors, > 59 -> special
                                        printf("\nDebug Random : %d\n", readRandomValue);
                                        readRandomValue = readRandomValue < 200 ? 0 : (readRandomValue >= 200 && readRandomValue < 400) ? 1 : (readRandomValue >= 400 && readRandomValue < 620) ? 2 : 3; 
                                        
                                        card_p[2].type = readRandomValue; //choose card type
                                        if(card_p[2].type != SPECIAL_TYPE) //rock, paper or scissors ?
                                        {
                                            //while(readRandomValue < 600)
                                            //{
                                            readRandomValue = generate_rand(620);
                                            printf("\nDebug Random : %d\n", readRandomValue);
                                            card_p[2].index = readRandomValue/100;
                                            //}
                                            
                                            if(card_p[2].index == 6) //check not to have duplicata...
                                            {
                                                if(card_p[2].type == ROCK_TYPE) //check when we've got rock here
                                                {
                                                    if(!gotRockAllKnowing)
                                                    {
                                                        gotRockAllKnowing = true;
                                                    }
                                                    else
                                                    {
                                                        if(!gotPaperAllKnowing)
                                                        {
                                                            card_p[2].type = PAPER_TYPE; //convert into paper type...
                                                            gotPaperAllKnowing = true;
                                                        }
                                                        else
                                                        {
                                                            card_p[2].type = SCISSORS_TYPE; //convert into Scissors type...
                                                            gotScissorsAllKnowing = true;
                                                        }
                                                    }
                                                }
                                                else if(card_p[2].type == PAPER_TYPE) //check when we've got paper here
                                                {
                                                    if(!gotPaperAllKnowing)
                                                    {
                                                        gotPaperAllKnowing = true;
                                                    }
                                                    else
                                                    {
                                                        if(!gotRockAllKnowing)
                                                        {
                                                            card_p[2].type = ROCK_TYPE; //convert into paper type...
                                                            gotRockAllKnowing = true;
                                                        }
                                                        else
                                                        {
                                                            card_p[2].type = SCISSORS_TYPE; //convert into Scissors type...
                                                            gotScissorsAllKnowing = true;
                                                        }
                                                    }
                                                }
                                                else //check when we've got scissors here
                                                {
                                                    if(!gotScissorsAllKnowing)
                                                    {
                                                        gotScissorsAllKnowing = true;
                                                    }
                                                    else
                                                    {
                                                        if(!gotPaperAllKnowing)
                                                        {
                                                            card_p[2].type = PAPER_TYPE; //convert into paper type...
                                                            gotPaperAllKnowing = true;
                                                        }
                                                        else
                                                        {
                                                            card_p[2].type = ROCK_TYPE; //convert into Scissors type...
                                                            gotRockAllKnowing = true;
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                        else //special ?
                                        {
                                            readRandomValue = generate_rand(190);
                                            printf("\nDebug Random : %d\n", readRandomValue);
                                            card_p[2].index = readRandomValue/100;
                                        }
                                            
                                    }
                                    allowUpdateSecondScreen = true; //update screen
                            }
                            else
                            {
                                playSound(pick, sizeof(pick));
                                sendAction = 7; //next action state machine
                                spr_table.index = spr_table.maxFrames-1; //put start frame
                            }
                        }
                    }

                    if(show_deck) //show full deck
                    {
                        //First card display
                        if(card_b[0].type == ROCK_TYPE)
                        {
                            loadCardSprite_small(rock, 0, bufferCard_r);
                            placeSpriteScaleX(bufferCard_r, spr_card_r.paletteBuffer, 40, 53, 2-abs(cardResize>>1)+posXGlitch, 40+posYGlitch, cardResize); 
                        }
                        else if(card_b[0].type == PAPER_TYPE)
                        {
                            loadCardSprite_small(paper, 2, bufferCard_p);
                            placeSpriteScaleX(bufferCard_p, spr_card_p.paletteBuffer, 40, 53, 2-abs(cardResize>>1)+posXGlitch, 40+posYGlitch, cardResize); 
                        }
                        else //SCISSORS_TYPE
                        {
                            loadCardSprite_small(scissors, 2, bufferCard_s);
                            placeSpriteScaleX(bufferCard_s, spr_card_s.paletteBuffer, 40, 53, 2-abs(cardResize>>1)+posXGlitch, 40+posYGlitch, cardResize); 
                        }
                        //Second card display
                        if(card_b[1].type == ROCK_TYPE)
                        {
                            loadCardSprite_small(rock, 0, bufferCard_r);
                            placeSpriteScaleX(bufferCard_r, spr_card_r.paletteBuffer, 40, 53, 42-abs(cardResize>>1)+posXGlitch, 40+posYGlitch, cardResize); 
                        }
                        else if(card_b[1].type == PAPER_TYPE)
                        {
                            loadCardSprite_small(paper, 2, bufferCard_p);
                            placeSpriteScaleX(bufferCard_p, spr_card_p.paletteBuffer, 40, 53, 42-abs(cardResize>>1)+posXGlitch, 40+posYGlitch, cardResize); 
                        }
                        else //SCISSORS_TYPE
                        {
                            loadCardSprite_small(scissors, 2, bufferCard_s);
                            placeSpriteScaleX(bufferCard_s, spr_card_s.paletteBuffer, 40, 53, 42-abs(cardResize>>1)+posXGlitch, 40+posYGlitch, cardResize); 
                        }
                        //Third card display
                        if(card_b[2].type == ROCK_TYPE)
                        {
                            loadCardSprite_small(rock, 0, bufferCard_r);
                            placeSpriteScaleX(bufferCard_r, spr_card_r.paletteBuffer, 40, 53, 85-abs(cardResize>>1)+posXGlitch, 40+posYGlitch, cardResize); 
                        }
                        else if(card_b[2].type == PAPER_TYPE)
                        {
                            loadCardSprite_small(paper, 2, bufferCard_p);
                            placeSpriteScaleX(bufferCard_p, spr_card_p.paletteBuffer, 40, 53, 85-abs(cardResize>>1)+posXGlitch, 40+posYGlitch, cardResize); 
                        }
                        else //SCISSORS_TYPE
                        {
                            loadCardSprite_small(scissors, 2, bufferCard_s);
                            placeSpriteScaleX(bufferCard_s, spr_card_s.paletteBuffer, 40, 53, 85-abs(cardResize>>1)+posXGlitch, 40+posYGlitch, cardResize); 
                        }
                        
                        
                        //printf("DEBUG : %d %d %d\n", cardResize, sequenceFightTime, show_next_card);

                        if(cardResize == 0)
                        {
                            if(sequenceFightTime < 20) //wait a bit
                            {
                                sequenceFightTime++;
                            }
                            else
                            { 
                                cardResize-=4; //increment to cancel timer
                            }
                        }
                        else if(cardResize == -40 && sequenceFightTime > 5)
                        {
                            show_deck = false; //cancel it now
                        }
                        else
                        {
                            if(sequenceFightTime < 5)
                                cardResize+=4;
                            else
                                cardResize-=4;
                        }

                        if(!flag_snd) //play one time
                        {
                            playSound(pick, sizeof(pick));
                            flag_snd = true;
                        }

                    }

                    if(show_next_card) //display next card played by the bot
                    {
                        if(card_b[botSelectedCard].type == ROCK_TYPE)
                        {
                            loadCardSprite_small(rock, 0, bufferCard_r);
                            placeSpriteScaleX(bufferCard_r, spr_card_r.paletteBuffer, 40, 53, 40-abs(cardResize>>1)+posXGlitch, 40+posYGlitch, cardResize); 
                        }
                        else if(card_b[botSelectedCard].type == PAPER_TYPE)
                        {
                            loadCardSprite_small(paper, 2, bufferCard_p);
                            placeSpriteScaleX(bufferCard_p, spr_card_p.paletteBuffer, 40, 53, 40-abs(cardResize>>1)+posXGlitch, 40+posYGlitch, cardResize); 
                        }
                        else //SCISSORS_TYPE
                        {
                            loadCardSprite_small(scissors, 2, bufferCard_s);
                            placeSpriteScaleX(bufferCard_s, spr_card_s.paletteBuffer, 40, 53, 40-abs(cardResize>>1)+posXGlitch, 40+posYGlitch, cardResize); 
                        }
                        
                        
                        //printf("DEBUG : %d %d %d\n", cardResize, sequenceFightTime, show_next_card);

                        if(cardResize == 0)
                        {
                            if(sequenceFightTime < 20) //wait a bit
                            {
                                sequenceFightTime++;
                            }
                            else
                            { 
                                cardResize-=4; //increment to cancel timer
                            }
                        }
                        else if(cardResize == -40 && sequenceFightTime > 5)
                        {
                            show_next_card = false; //cancel it now
                        }
                        else
                        {
                            if(sequenceFightTime < 5)
                                cardResize+=4;
                            else
                                cardResize-=4;
                        }

                        if(!flag_snd) //play one time
                        {
                            playSound(pick, sizeof(pick));
                            flag_snd = true;
                        }

                    }


                }
            }
            else if(sendAction == 7)
            {
                fightCnt = 0; // at first reset that counter !
                if(cardSelectedXposOffset > 0) //put the card back to it's original position
                {
                    cardSelectedXposOffset-=5;
                }
                else
                {
                    repeatFight = 0; //remove fight repeat
                    sendAction = 0; //back to first state
                    swapCards = false;
                    botChangedCard = false;
                    force_same_card = false; //remove play same card...

                }
            }


            if(selectedCard != 0)
            {

                drawBox(83+posXGlitch, -2-deckYposOffset-bootYOffset+posYGlitch, 44, 57, RGB565_BLACK);
                
                /////////////////////////////////////////////////////////////
                //load card

                if(card_p[0].type == ROCK_TYPE)
                {
                    spr_card_r.index = card_p[0].index;
                    loadCardSprite_small(rock, card_p[0].index, bufferCard_r);
                    loadSprite(&spr_card_r, bufferCard_r, 2120, colorPalette_rock, 40, 53);
                    placeSprite(&spr_card_r, 85+posXGlitch, 0-deckYposOffset-bootYOffset+posYGlitch, 0x00, true);
                }
                else if(card_p[0].type == PAPER_TYPE)
                {
                    spr_card_p.index = card_p[0].index;
                    loadCardSprite_small(paper, card_p[0].index, bufferCard_p);
                    loadSprite(&spr_card_p, bufferCard_p, 2120, colorPalette_paper, 40, 53);
                    placeSprite(&spr_card_p, 85+posXGlitch, 0-deckYposOffset-bootYOffset+posYGlitch, 0x00, true);
                }
                else if(card_p[0].type == SCISSORS_TYPE)
                {
                    spr_card_s.index = card_p[0].index;
                    loadCardSprite_small(scissors, card_p[0].index, bufferCard_s);
                    loadSprite(&spr_card_s, bufferCard_s, 2120, colorPalette_scissors, 40, 53);
                    placeSprite(&spr_card_s, 85+posXGlitch, 0-deckYposOffset-bootYOffset+posYGlitch, 0x00, true);
                }
                else
                {
                    spr_card_x.index = card_p[0].index;
                    loadCardSprite_small(special, card_p[0].index, bufferCard_x);
                    loadSprite(&spr_card_x, bufferCard_x, 2120, colorPalette_special, 40, 53);
                    placeSprite(&spr_card_x, 85+posXGlitch, 0-deckYposOffset-bootYOffset+posYGlitch, 0x00, true);
                }
            }
            
            if(selectedCard != 1)
            {

                drawBox(71+posXGlitch, -5-deckYposOffset-bootYOffset+posYGlitch, 44, 57, RGB565_BLACK);
                
                /////////////////////////////////////////////////////////////
                //display card on main display

                ///////////////////////////////////////////
                //cards deck

                if(card_p[1].type == ROCK_TYPE)
                {
                    spr_card_r.index = card_p[1].index;
                    loadCardSprite_small(rock, card_p[1].index, bufferCard_r);
                    loadSprite(&spr_card_r, bufferCard_r, 2120, colorPalette_rock, 40, 53);
                    placeSprite(&spr_card_r, 73+posXGlitch, -3-deckYposOffset-bootYOffset+posYGlitch, 0x00, true);
                }
                else if(card_p[1].type == PAPER_TYPE)
                {
                    spr_card_p.index = card_p[1].index;
                    loadCardSprite_small(paper, card_p[1].index, bufferCard_p);
                    loadSprite(&spr_card_p, bufferCard_p, 2120, colorPalette_paper, 40, 53);
                    placeSprite(&spr_card_p, 73+posXGlitch, -3-deckYposOffset-bootYOffset+posYGlitch, 0x00, true);
                }
                else if(card_p[1].type == SCISSORS_TYPE)
                {
                    spr_card_s.index = card_p[1].index;
                    loadCardSprite_small(scissors, card_p[1].index, bufferCard_s);
                    loadSprite(&spr_card_s, bufferCard_s, 2120, colorPalette_scissors, 40, 53);
                    placeSprite(&spr_card_s, 73+posXGlitch, -3-deckYposOffset-bootYOffset+posYGlitch, 0x00, true);
                }
                else
                {
                    spr_card_x.index = card_p[1].index;
                    loadCardSprite_small(special, card_p[1].index, bufferCard_x);
                    loadSprite(&spr_card_x, bufferCard_x, 2120, colorPalette_special, 40, 53);
                    placeSprite(&spr_card_x, 73+posXGlitch, -3-deckYposOffset-bootYOffset+posYGlitch, 0x00, true);
                }
            }

            if(selectedCard != 2)
            {
                drawBox(58+posXGlitch, -7-deckYposOffset-bootYOffset+posYGlitch, 44, 57, RGB565_BLACK);
                //placeSprite(&spr_card_p, 60, -5, dispArea, 0x00, true);

                /////////////////////////////////////////////////////////////
                //display card on main display

                if(card_p[2].type == ROCK_TYPE)
                {
                    spr_card_r.index = card_p[2].index;
                    loadCardSprite_small(rock, card_p[2].index, bufferCard_r);
                    loadSprite(&spr_card_r, bufferCard_r, 2120, colorPalette_rock, 40, 53);
                    placeSprite(&spr_card_r, 60+posXGlitch, -5-deckYposOffset-bootYOffset+posYGlitch, 0x00, true);
                }
                else if(card_p[2].type == PAPER_TYPE)
                {
                    spr_card_p.index = card_p[2].index;
                    loadCardSprite_small(paper, card_p[2].index, bufferCard_p);
                    loadSprite(&spr_card_p, bufferCard_p, 2120, colorPalette_paper, 40, 53);
                    placeSprite(&spr_card_p, 60+posXGlitch, -5-deckYposOffset-bootYOffset+posYGlitch, 0x00, true);
                }
                else if(card_p[2].type == SCISSORS_TYPE)
                {
                    //printf("DEBUG SPRITE : %d %d", spr_card_s.index, spr_card_s.winIncrement)
                    spr_card_s.index = card_p[2].index;
                    loadCardSprite_small(scissors, card_p[2].index, bufferCard_s);
                    loadSprite(&spr_card_s, bufferCard_s, 2120, colorPalette_scissors, 40, 53);
                    placeSprite(&spr_card_s, 60+posXGlitch, -5-deckYposOffset-bootYOffset+posYGlitch, 0x00, true);
                }
                else
                {
                    spr_card_x.index = card_p[2].index;
                    loadCardSprite_small(special, card_p[2].index, bufferCard_x);
                    loadSprite(&spr_card_x, bufferCard_x, 2120, colorPalette_special, 40, 53);
                    placeSprite(&spr_card_x, 60+posXGlitch, -5-deckYposOffset-bootYOffset+posYGlitch, 0x00, true);
                }
            }


            if(selectedCard == 0)
            {

                drawBox(83+cardSelectedXposOffset+posXGlitch, 3-bootYOffset+posYGlitch, 44, 57, RGB565_WHITE);
                

                /////////////////////////////////////////////////////////////
                //load card

                if(card_p[0].type == ROCK_TYPE)
                {
                    spr_card_r.index = card_p[0].index;
                    //placeSprite(&spr_card_r, 85, 0, dispArea, 0x00, true);
                    loadCardSprite_small(rock, card_p[0].index, bufferCard_r);
                    loadSprite(&spr_card_r, bufferCard_r, 2120, colorPalette_rock, 40, 53);
                    placeSprite(&spr_card_r, 85+cardSelectedXposOffset+posXGlitch, 5-bootYOffset+posYGlitch, 0x00, false);
                    if(allowUpdateSecondScreen)
                    {
                        loadCardSprite(rock, colorPalette_rock, card_p[0].index, pio_2, sm_2, contrast_2);
                        allowUpdateSecondScreen = false; //disable reduce cpu usage
                    }
                        
                }
                else if(card_p[0].type == PAPER_TYPE)
                {
                    spr_card_p.index = card_p[0].index;
                    //placeSprite(&spr_card_p, 85, 0, dispArea, 0x00, true);
                    loadCardSprite_small(paper, card_p[0].index, bufferCard_p);
                    loadSprite(&spr_card_p, bufferCard_p, 2120, colorPalette_paper, 40, 53);
                    placeSprite(&spr_card_p, 85+cardSelectedXposOffset+posXGlitch, 5-bootYOffset+posYGlitch, 0x00, false);
                    if(allowUpdateSecondScreen)
                    {
                        loadCardSprite(paper, colorPalette_paper, card_p[0].index, pio_2, sm_2, contrast_2);
                        allowUpdateSecondScreen = false; //disable reduce cpu usage
                    }
                        
                }
                else if(card_p[0].type == SCISSORS_TYPE)
                {
                    spr_card_s.index = card_p[0].index;
                    //placeSprite(&spr_card_s, 85, 0, dispArea, 0x00, true);
                    loadCardSprite_small(scissors, card_p[0].index, bufferCard_s);
                    loadSprite(&spr_card_s, bufferCard_s, 2120, colorPalette_scissors, 40, 53);
                    placeSprite(&spr_card_s, 85+cardSelectedXposOffset+posXGlitch, 5-bootYOffset+posYGlitch, 0x00, false);
                    if(allowUpdateSecondScreen)
                    {
                        loadCardSprite(scissors, colorPalette_scissors, card_p[0].index, pio_2, sm_2, contrast_2);
                        allowUpdateSecondScreen = false; //disable reduce cpu usage
                    }
                        
                }
                else
                {
                    spr_card_x.index = card_p[0].index;
                    //placeSprite(&spr_card_x, 85, 0, dispArea, 0x00, true);
                    loadCardSprite_small(special, card_p[0].index, bufferCard_x);
                    loadSprite(&spr_card_x, bufferCard_x, 2120, colorPalette_special, 40, 53);
                    placeSprite(&spr_card_x, 85+cardSelectedXposOffset+posXGlitch, 5-bootYOffset+posYGlitch, 0x00, false);
                    if(allowUpdateSecondScreen)
                    {
                        loadCardSprite(special, colorPalette_special, card_p[0].index, pio_2, sm_2, contrast_2);
                        allowUpdateSecondScreen = false; //disable reduce cpu usage
                    }
                        
                }
            }
            else if(selectedCard == 1)
            {

                drawBox(71+cardSelectedXposOffset+posXGlitch, 3+posYGlitch, 44, 57, RGB565_WHITE);
                //placeSprite(&spr_card_r, 73, 5, dispArea, 0x00, false);
                //loadCardSprite(rock, colorPalette_rock, 3, pio_2, sm_2);

                /////////////////////////////////////////////////////////////
                //load card

                if(card_p[1].type == ROCK_TYPE)
                {
                    spr_card_r.index = card_p[1].index;
                    //placeSprite(&spr_card_r, 85, 0, dispArea, 0x00, true);
                    loadCardSprite_small(rock, card_p[1].index, bufferCard_r);
                    loadSprite(&spr_card_r, bufferCard_r, 2120, colorPalette_rock, 40, 53);
                    placeSprite(&spr_card_r, 73+cardSelectedXposOffset+posXGlitch, 5-bootYOffset+posYGlitch, 0x00, false);
                    if(allowUpdateSecondScreen)
                    {
                        loadCardSprite(rock, colorPalette_rock, card_p[1].index, pio_2, sm_2, contrast_2);
                        allowUpdateSecondScreen = false; //disable reduce cpu usage
                    }
                }
                else if(card_p[1].type == PAPER_TYPE)
                {
                    spr_card_p.index = card_p[1].index;
                    //placeSprite(&spr_card_p, 85, 0, dispArea, 0x00, true);
                    loadCardSprite_small(paper, card_p[1].index, bufferCard_p);
                    loadSprite(&spr_card_p, bufferCard_p, 2120, colorPalette_paper, 40, 53);
                    placeSprite(&spr_card_p, 73+cardSelectedXposOffset+posXGlitch, 5-bootYOffset+posYGlitch, 0x00, false);
                    if(allowUpdateSecondScreen)
                    {
                        loadCardSprite(paper, colorPalette_paper, card_p[1].index, pio_2, sm_2, contrast_2);
                        allowUpdateSecondScreen = false; //disable reduce cpu usage
                    }
                }
                else if(card_p[1].type == SCISSORS_TYPE)
                {
                    spr_card_s.index = card_p[1].index;
                    //placeSprite(&spr_card_s, 85, 0, dispArea, 0x00, true);
                    loadCardSprite_small(scissors, card_p[1].index, bufferCard_s);
                    loadSprite(&spr_card_s, bufferCard_s, 2120, colorPalette_scissors, 40, 53);
                    placeSprite(&spr_card_s, 73+cardSelectedXposOffset+posXGlitch, 5-bootYOffset+posYGlitch, 0x00, false);
                    if(allowUpdateSecondScreen)
                    {
                        loadCardSprite(scissors, colorPalette_scissors, card_p[1].index, pio_2, sm_2, contrast_2);
                        allowUpdateSecondScreen = false; //disable reduce cpu usage
                    }
                }
                else
                {
                    spr_card_x.index = card_p[1].index;
                    //placeSprite(&spr_card_x, 85, 0, dispArea, 0x00, true);
                    loadCardSprite_small(special, card_p[1].index, bufferCard_x);
                    loadSprite(&spr_card_x, bufferCard_x, 2120, colorPalette_special, 40, 53);
                    placeSprite(&spr_card_x, 73+cardSelectedXposOffset+posXGlitch, 5-bootYOffset+posYGlitch, 0x00, false);
                    if(allowUpdateSecondScreen)
                    {
                        loadCardSprite(special, colorPalette_special, card_p[1].index, pio_2, sm_2, contrast_2);
                        allowUpdateSecondScreen = false; //disable reduce cpu usage
                    }
                }
            }
            else
            {

                drawBox(58+cardSelectedXposOffset+posXGlitch, 3-bootYOffset+posYGlitch, 44, 57, RGB565_WHITE);
                //placeSprite(&spr_card_p, 60, 5, dispArea, 0x00, false);
                //loadCardSprite(paper, colorPalette_paper, 3, pio_2, sm_2);

                /////////////////////////////////////////////////////////////
                //load card

                if(card_p[2].type == ROCK_TYPE)
                {
                    spr_card_r.index = card_p[2].index;
                    //placeSprite(&spr_card_r, 85, 0, dispArea, 0x00, true);
                    loadCardSprite_small(rock, card_p[2].index, bufferCard_r);
                    loadSprite(&spr_card_r, bufferCard_r, 2120, colorPalette_rock, 40, 53);
                    placeSprite(&spr_card_r, 60+cardSelectedXposOffset+posXGlitch, 5-bootYOffset+posYGlitch, 0x00, false);
                    if(allowUpdateSecondScreen)
                    {
                        loadCardSprite(rock, colorPalette_rock, card_p[2].index, pio_2, sm_2, contrast_2);
                        allowUpdateSecondScreen = false; //disable reduce cpu usage
                    }
                        
                }
                else if(card_p[2].type == PAPER_TYPE)
                {
                    spr_card_p.index = card_p[2].index;
                    //placeSprite(&spr_card_p, 85, 0, dispArea, 0x00, true);
                    loadCardSprite_small(paper, card_p[2].index, bufferCard_p);
                    loadSprite(&spr_card_p, bufferCard_p, 2120, colorPalette_paper, 40, 53);
                    placeSprite(&spr_card_p, 60+cardSelectedXposOffset+posXGlitch, 5-bootYOffset+posYGlitch, 0x00, false);
                    if(allowUpdateSecondScreen)
                    {
                        loadCardSprite(paper, colorPalette_paper, card_p[2].index, pio_2, sm_2, contrast_2);
                        allowUpdateSecondScreen = false; //disable reduce cpu usage
                    }
                        
                }
                else if(card_p[2].type == SCISSORS_TYPE)
                {
                    spr_card_s.index = card_p[2].index;
                    //placeSprite(&spr_card_s, 85, 0, dispArea, 0x00, true);
                    loadCardSprite_small(scissors, card_p[2].index, bufferCard_s);
                    loadSprite(&spr_card_s, bufferCard_s, 2120, colorPalette_scissors, 40, 53);
                    placeSprite(&spr_card_s, 60+cardSelectedXposOffset+posXGlitch, 5-bootYOffset+posYGlitch, 0x00, false);
                    if(allowUpdateSecondScreen)
                    {
                        loadCardSprite(scissors, colorPalette_scissors, card_p[2].index, pio_2, sm_2, contrast_2);
                        allowUpdateSecondScreen = false; //disable reduce cpu usage
                    }
                        
                }
                else
                {
                    spr_card_x.index = card_p[2].index;
                    //placeSprite(&spr_card_x, 85, 0, dispArea, 0x00, true);
                    loadCardSprite_small(special, card_p[2].index, bufferCard_x);
                    loadSprite(&spr_card_x, bufferCard_x, 2120, colorPalette_special, 40, 53);
                    placeSprite(&spr_card_x, 60+cardSelectedXposOffset + posXGlitch, 5-bootYOffset + posYGlitch, 0x00, false);
                    if(allowUpdateSecondScreen)
                    {
                        loadCardSprite(special, colorPalette_special, card_p[2].index, pio_2, sm_2, contrast_2);
                        allowUpdateSecondScreen = false; //disable reduce cpu usage
                    }
                        
                }
            }
            

            ///////////////////////////////////////////
            //Display choosen card

            //player side
            drawBox(48 + cards_fight_delta_p+posXGlitch, 38+posYGlitch, 42, 57, RGB565_BLACK);
            if(choosen_card_player.type == ROCK_TYPE)
            {
                spr_card_r.index = choosen_card_player.index;
                loadCardSprite_small(rock, choosen_card_player.index, bufferCard_r);
                loadSprite(&spr_card_r, bufferCard_r, 2120, colorPalette_rock, 40, 53);
                placeSprite(&spr_card_r, 50 + cards_fight_delta_p + posXGlitch, 40 + posYGlitch, 0x00, false);
            }
            else if(choosen_card_player.type == PAPER_TYPE)
            {
                spr_card_p.index = choosen_card_player.index;
                loadCardSprite_small(paper, choosen_card_player.index, bufferCard_p);
                loadSprite(&spr_card_p, bufferCard_p, 2120, colorPalette_paper, 40, 53);
                placeSprite(&spr_card_p, 50 + cards_fight_delta_p + posXGlitch, 40 + posYGlitch, 0x00, false);
            }
            else if(choosen_card_player.type == SCISSORS_TYPE)
            {
                    //printf("DEBUG SPRITE : %d %d", spr_card_s.index, spr_card_s.winIncrement)
                spr_card_s.index = choosen_card_player.index;
                loadCardSprite_small(scissors, choosen_card_player.index, bufferCard_s);
                loadSprite(&spr_card_s, bufferCard_s, 2120, colorPalette_scissors, 40, 53);
                placeSprite(&spr_card_s, 50 + cards_fight_delta_p + posXGlitch, 40 + posYGlitch, 0x00, false);
            }
            else
            {
                spr_card_x.index = choosen_card_player.index;
                loadCardSprite_small(special, choosen_card_player.index, bufferCard_x);
                loadSprite(&spr_card_x, bufferCard_x, 2120, colorPalette_special, 40, 53);
                placeSprite(&spr_card_x, 50 + cards_fight_delta_p + posXGlitch, 40 + posYGlitch, 0x00, false);
            }

            //bot side
            drawBox(39 - cards_fight_delta_b+posXGlitch, 39+posYGlitch, 42, 57, RGB565_BLACK);
            if(choosen_card_bot.type == ROCK_TYPE)
            {
                choosen_card_bot.index = 0;
                spr_card_r.index = choosen_card_bot.index;
                loadCardSprite_small(rock, choosen_card_bot.index, bufferCard_r);
                loadSprite(&spr_card_r, bufferCard_r, 2120, colorPalette_rock, 40, 53);
                placeSprite(&spr_card_r, 40 - cards_fight_delta_b + posXGlitch, 40 + posYGlitch, 0x00, false);
            }
            else if(choosen_card_bot.type == PAPER_TYPE)
            {
                choosen_card_bot.index = 2;
                spr_card_p.index = choosen_card_bot.index;
                loadCardSprite_small(paper, choosen_card_bot.index, bufferCard_p);
                loadSprite(&spr_card_p, bufferCard_p, 2120, colorPalette_paper, 40, 53);
                placeSprite(&spr_card_p, 40 - cards_fight_delta_b + posXGlitch, 40+posYGlitch, 0x00, false);
            }
            else// if(choosen_card_bot.type == SCISSORS_TYPE)
            {
                choosen_card_bot.index = 2;
                spr_card_s.index = choosen_card_bot.index;
                loadCardSprite_small(scissors, choosen_card_bot.index, bufferCard_s);
                loadSprite(&spr_card_s, bufferCard_s, 2120, colorPalette_scissors, 40, 53);
                placeSprite(&spr_card_s, 40 - cards_fight_delta_b + posXGlitch, 40+posYGlitch, 0x00, false);
            }


            
            ///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
            

            

            ////////////////////////////////////////////
            //draw player hp
            //background
            if(sendAction <= 7)
            {
                drawBox(ennemyHp+5+posXGlitch, 120+posYGlitch, 125 - (ennemyHp+5), 2, RGB565_RED); //max 120
                drawBox(5+posXGlitch, 5+posYGlitch, 35, 2, RGB565_BLACK);
                //foreground
                drawBox(5+posXGlitch, 5+posYGlitch, playerHp, 2, RGB565_GREEN); //max 35
            }
            
            ////////////////////////////////////////////

            ////////////////////////////////////////////
            //Text section
                    
            ////////////////////////////////////
            //If numberYposOffset reset to 0 -> display all text (means cards got collision)
            if(numberYposOffset < 10)
            {
                
                
                
                
                if(numberYposOffset == 0)
                {
                    spr_number.index = 6;
                }
                else
                {
                    spr_number.index = 14;
                }


                if(roundState == LOSE_TYPE)
                {
                    spr_text.index = 0;
                }
                else if(roundState == TIE_TYPE)
                {
                    spr_text.index = 1;
                }
                else // WIN_TYPE
                {
                    spr_text.index = 2;
                }




                if(dmgDeal != NO_DMG_GET)
                {

                    if(numberYposOffset == 0)
                    {
                        spr_number.index = (dmgDeal == DMG_15_TYPE) ? 1 : (dmgDeal == DMG_25_TYPE) ? 2 : (dmgDeal == DMG_45_TYPE) ? 3 : (dmgDeal == DMG_50_TYPE) ? 4 : (dmgDeal == DMG_75_TYPE) ? 5 : 6; 
                        if(dmgTake == DMG_15_TYPE)
                        {
                            ennemyHp_conv -=1.5;
                        }
                        else if(dmgTake == DMG_25_TYPE)
                        {
                            ennemyHp_conv -= 2.5;
                        }
                        else if(dmgTake == DMG_45_TYPE)
                        {
                            ennemyHp_conv -= 4.5;
                        }
                        else if(dmgTake == DMG_50_TYPE)
                        {
                            ennemyHp_conv -= 5;
                        }
                        else if(dmgTake == DMG_75_TYPE)
                        {
                            ennemyHp_conv -= 7.5;
                        }
                        else// if(dmgTake == DMG_100_TYPE)
                        {
                            ennemyHp_conv -= 10.0;
                        }
                        if(ennemyHp_conv < 0.0)
                            ennemyHp_conv = 0.0;
                    }
                    else
                    {
                        spr_number.index = (dmgDeal == DMG_15_TYPE) ? 8 : (dmgDeal == DMG_25_TYPE) ? 9 : (dmgDeal == DMG_45_TYPE) ? 10 : (dmgDeal == DMG_50_TYPE) ? 12 : (dmgDeal == DMG_75_TYPE) ? 13 : 14;
                    }
                    
                    placeSprite(&spr_number, 90+posXGlitch, 90 + (numberYposOffset < 5 ? numberYposOffset : 5)+posYGlitch, 0x05, false); //0x04 for number !
                }

                if(dmgTake != NO_DMG_GET)
                {

                    if(numberYposOffset == 0)
                    {
                        spr_number.index = (dmgTake == DMG_15_TYPE) ? 1 : (dmgTake == DMG_25_TYPE) ? 2 : (dmgTake == DMG_45_TYPE) ? 3 : (dmgTake == DMG_50_TYPE) ? 4 : (dmgTake == DMG_75_TYPE) ? 5 : (dmgTake == HEAL_50_TYPE) ? 4 : 0; 
                        if(dmgTake == DMG_15_TYPE)
                        {
                            playerHp_conv -=1.5;
                        }
                        else if(dmgTake == DMG_25_TYPE)
                        {
                            playerHp_conv -= 2.5;
                        }
                        else if(dmgTake == DMG_45_TYPE)
                        {
                            playerHp_conv -= 4.5;
                        }
                        else if(dmgTake == DMG_50_TYPE)
                        {
                            playerHp_conv -= 5.0;
                        }
                        else if(dmgTake == DMG_75_TYPE) 
                        {
                            playerHp_conv -= 7.5;
                        }
                        else //if(dmgTake == HEAL_50_TYPE)
                        {
                            playerHp_conv += 5.0;
                        }
                        if(playerHp_conv > 50.0)
                            playerHp_conv = 50.0;
                        
                        if(playerHp_conv < 0)
                            playerHp_conv = 0.0;
                    }
                    else
                    {
                        spr_number.index = (dmgTake == DMG_15_TYPE) ? 8 : (dmgTake == DMG_25_TYPE) ? 9 : (dmgTake == DMG_45_TYPE) ? 10 : (dmgTake == DMG_50_TYPE) ? 12 : (dmgTake == DMG_75_TYPE) ? 13 : (dmgTake == HEAL_50_TYPE) ? 11 : 7;
                    }
                    
                    placeSprite(&spr_number, 5 + posXGlitch, 20 + (numberYposOffset < 5 ? numberYposOffset : 5) + posYGlitch, 0x05, false); //0x04 for number !
                }

                placeSprite(&spr_text, 35+posXGlitch, 90+posYGlitch, 0x05, false);
                numberYposOffset++;
            }
            
            

        }
        else //menu action
        {
            if(selectedCard == 0)
            {
                loadCardSprite(instructions, colorPalette_instructions, 1, pio_2, sm_2, contrast_2);
                spr_menu.index = 1;
                if(sendAction == 1)
                {
                    //launch game
                    contrast = 1.0;
                    sendAction = 2;
                }
            }
            else if(selectedCard == 1)
            {
                loadCardSprite(instructions, colorPalette_instructions, 0, pio_2, sm_2, contrast_2);
                spr_menu.index = 0;
                if(sendAction == 1)
                {
                    sendAction = 0;
                }
            }
            else //no overflow...
            {
                selectedCard = 1;
            }

            
            /*if(sendAction == 0)
            {
                placeSprite(&spr_menu, 0, 0, 0x00, false);
            }*/
            placeSprite(&spr_menu, posXGlitch, posYGlitch, 0x00, false);
            if(sendAction == 2)
            {
                contrast+=2.0;
                contrast_2+=2.0;
                volumeMenu-=0.05;
                if(volumeMenu <= 0.0)
                {
                    volumeMenu = 0.0;
                }
                if(contrast >= 70.0)
                {
                    contrast = 69.0;
                    contrast_2 = 69.0;
                    sendAction = 3;
                }
            }

            if(sendAction == 3)
            {
                contrast-=2.0;
                contrast_2-=2.0;
                if(contrast <= 51.0) //method to decrement fast on put value on 1.0 at start
                {
                    sendAction = 0;
                    menu = 0; //start game
                    playOst(ost2, sizeof(ost2));
                    volumeMenu = 1.0;
                }
            }

            layerContrast(contrast);
            



        }
        
        displayScale(setScale+1 + zoomGlitch);

        //section bot loose


        if(sendAction == 8)
        {
            bootYOffset+=5;
            if(bootYOffset > 120)
            {
                bootYOffset = 120;
            }
            spr_bot_offsetY++;
            if(spr_bot_offsetY > 50)
            {
                sendAction = 9;
            }
            //shake the screen
            screen_shakingX = (rand() % 10) - 5;
            screen_shakingY = (rand() % 10) - 5;
        }
        if(sendAction == 9)
        {
            volumeMenu-=0.05;
            if(volumeMenu <= 0.0)
            {
                volumeMenu = 0.0;
            }
            contrast += 2.0;
            contrast_2+=2.0;
            allowUpdateSecondScreen = true;
            if(contrast > 100.0)
            {
                sendAction = 15; //end the game
            }
            //shake the screen
            screen_shakingX = (rand() % 10) - 5;
            screen_shakingY = (rand() % 10) - 5;
        }

        if(sendAction == 10)
        {
            setScale+=2;
            if(setScale >= 80)
            {
                zoom_rand = 40;
                playSound(boom, sizeof(boom));
                sendAction = 11;
            }
        }

        if(sendAction == 11)
        {
            zoom_rand--;
            if(zoom_rand <= 0)
            {
                zoom_rand = 0;
                sendAction = 12;
            }
            setScale = (rand() % zoom_rand) + 80;
        }

        if(sendAction == 12)
        {
            setScale = 80;
            volumeMenu-=0.05;
            if(volumeMenu <= 0.0)
            {
                volumeMenu = 0.0;
            }
            contrast += 2.0;
            contrast_2+=2.0;
            allowUpdateSecondScreen = true;
            if(contrast > 100.0)
            {
                sendAction = 15; //end the game
            }
        }

        if(sendAction == 13)
        {
            if(contrast > 0)
            {
                contrast -=0.1;
            }
            else
            {
                sendAction = 14;
            }
        }

        if(sendAction == 14)
        {
            clearLayers();
            contrast+=0.01;
            if(contrast >= 1.0)
            {
                volumeMenu-=0.01;
                if(volumeMenu <= 0.0)
                {
                    volumeMenu = 0.0;
                }
                contrast+=1.0;
                contrast_2+=2.0;
                allowUpdateSecondScreen = true;
                screen_shakingX = (rand() % ((int)(contrast)))-((int)(contrast*0.5));
                screen_shakingY = (rand() % ((int)(contrast)))-((int)(contrast*0.5));
                if(screen_shakingX > 30)
                {
                    screen_shakingX = 30;
                }
                if(screen_shakingX < -30)
                {
                    screen_shakingX = -30;
                }
                if(screen_shakingY > 30)
                {
                    screen_shakingY = 30;
                }
                if(screen_shakingY < -30)
                {
                    screen_shakingY = -30;
                }
                printf("DEBUG SEQUENCE : %f %d %d\n", contrast, screen_shakingX, screen_shakingY);
            }
            if(contrast > 100.0)
            {
                sendAction = 15;
                volumeMenu = 0.0;
            }
            placeSprite(&spr_all_knowing, (screen_shakingX*2)+posXGlitch, (screen_shakingY*2)+posYGlitch, 0x00, false);
            layerContrast(contrast);
        }


        //special spection -> All knowing !
        if(card_p[0].index == 6 && card_p[1].index == 6 && card_p[2].index == 6) //check index = 6 -> last card
        {
            if(card_p[0].type != card_p[1].type && card_p[0].type != card_p[2].type && card_p[1].type != card_p[2].type) //check if we have ROCK PAPER SCISSORS
            {
                all_knowing_trig = true;
            }
        }




        //Display on main screen
        for (int y = 0; y < SCREEN_HEIGHT; ++y) {
            for (int x = 0; x < SCREEN_WIDTH; ++x) {
                uint16_t colour = dispArea[y][x]; 
                k++;               
                st7789_lcd_put(pio_1, sm_1, colour >> 8);
                st7789_lcd_put(pio_1, sm_1, colour & 0xff);
            }
        }
    }
}
