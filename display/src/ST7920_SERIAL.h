/* Created by Abhay Dutta, 3rd June, 2024 */

#ifndef ST7920_SERIAL_H_
#define ST7920_SERIAL_H_


#include "display.h"


#define SCLK_PIN 36 	//E Pin in Display
#define CS_PIN 39  		//RS Pin in Display
#define SID_PIN 35 		//R/W Pin in Display
#define RST_PIN 38 		//RST Pin in Display


#define GPIO_PIN_SET 1
#define GPIO_PIN_RESET 0

extern uint8_t image[(128 * 64)/8];

extern uint8_t disp1color_buff[(128 * 64)/8];




// A replacement for SPI_TRANSMIT
void SendByteSPI(uint8_t byte);


// Send the command to the LCD
void ST7920_SendCmd (uint8_t cmd);


// send the data to the LCD
void ST7920_SendData (uint8_t data);


/* send the string to the LCD
 * 'row' = starting ROW for the string (from 0 to 3)
 * 'col' = starting COL for the string (from 0 to 7)
 */
void ST7920_SendString(int row, int col, char* string);


/* ENABLE or DISABLE the graphic mode
 * enable =1 --> graphic mode enabled
 */
void ST7920_GraphicMode (int enable);


// clear screen in any mode
void ST7920_Clear();


// Draw bitmap on the display
void ST7920_DrawBitmap(const unsigned char* graphic);



// Update the display with the selected graphics
void ST7920_Update(void);



// Initialize the display
void ST7920_Init (void);


/* Common functions used
 * in other LCDs also
 */


// Set a pixel on the display
void SetPixel(uint8_t x, uint8_t y);



// draw line from (X0, Y0) to (X1, Y1)
void DrawLine(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1);


// draw rectangle from (X,Y) w- width, h- height
void DrawRectangle(uint16_t x, uint16_t y, uint16_t w, uint16_t h);



// draw filled rectangle
void DrawFilledRectangle(uint16_t x, uint16_t y, uint16_t w, uint16_t h);



// draw circle with centre (X0, Y0) and radius= radius
void DrawCircle(uint8_t x0, uint8_t y0, uint8_t radius);



// Draw Filled Circle with centre (X0, Y0) and radius= r

void DrawFilledCircle(int16_t x0, int16_t y0, int16_t r);



// Draw Traingle with coordimates (x1, y1), (x2, y2), (x3, y3)
void DrawTriangle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t x3, uint16_t y3);



// Draw Filled Traingle with coordimates (x1, y1), (x2, y2), (x3, y3)
void DrawFilledTriangle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t x3, uint16_t y3);

//for gpio initialization
void pin_init();

//for displaying different fonts
#if 1

void ST7920_Ext_SetGDRAMAddr(uint8_t VertAddr, uint8_t HorizAddr);
uint8_t ST7920_GetHorizontalByte(uint8_t *pBuff, uint8_t Row, uint8_t Col);
void ST7920_DisplayFullUpdate(uint8_t *pBuff, uint16_t BuffLen);
void disp1color_UpdateFromBuff(void);
void disp1color_DrawPixel(int16_t X, int16_t Y, uint8_t State);
uint8_t disp1color_DrawChar(int16_t X, int16_t Y, uint8_t FontID, uint8_t Char);
void disp1color_DrawString(int16_t X, int16_t Y, uint8_t FontID, uint8_t *Str);
void disp1color_printf(int16_t X, int16_t Y, uint8_t FontID, const char *args, ...);
void disp1color_clearBuff(void);
#endif


#endif

