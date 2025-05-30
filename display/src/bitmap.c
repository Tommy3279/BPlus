/* Created by Abhay Dutta, 3rd June, 2024 */

#include "display.h"
const unsigned char bbd_logo [] =
{
// 'baoBapDau', 128x64px
// 'baoBapDau', 128x64px
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7f, 0x7e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7e, 0x7e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x01, 0x80, 0x00, 0x00, 0x00, 0x00, 0xfc, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x01, 0xf8, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x0f, 0x80, 0x80, 0x00, 0x00, 0x00, 0x03, 0xf8, 0x1f, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x08, 0xc0, 0x80, 0x00, 0x00, 0x00, 0x03, 0xf0, 0x0f, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x08, 0x40, 0x00, 0x00, 0x07, 0xff, 0xff, 0xe0, 0x07, 0xff, 0xff, 0xe0, 0x00, 0x00, 0x00, 0x00, 
0x08, 0x43, 0xe0, 0xf0, 0x1f, 0xff, 0xff, 0xc0, 0x03, 0xff, 0xff, 0xf8, 0x00, 0x00, 0x00, 0x00, 
0x08, 0xc4, 0x21, 0x08, 0x1f, 0xff, 0xff, 0xc0, 0x03, 0xff, 0xff, 0xf8, 0x00, 0x00, 0x00, 0x00, 
0x0f, 0x80, 0x33, 0x0c, 0x1f, 0xff, 0xff, 0x80, 0x01, 0xff, 0xff, 0xf8, 0x00, 0x00, 0x00, 0x00, 
0x08, 0x63, 0xf2, 0x04, 0x0f, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0xf0, 0x00, 0x00, 0x00, 0x00, 
0x08, 0x26, 0x32, 0x04, 0x0f, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x03, 0xf0, 0x00, 0x00, 0x00, 0x00, 
0x08, 0x24, 0x33, 0x0c, 0x07, 0xe0, 0x00, 0x07, 0xc0, 0x00, 0x07, 0xe0, 0x00, 0x00, 0x00, 0x00, 
0x08, 0x66, 0x71, 0x08, 0x03, 0xf0, 0x00, 0x3f, 0xf8, 0x00, 0x07, 0xc0, 0x00, 0x00, 0x00, 0x00, 
0x0f, 0xc3, 0xf0, 0xf0, 0x03, 0xf0, 0x00, 0x7f, 0xfc, 0x00, 0x0f, 0xc0, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x01, 0xf8, 0x00, 0xff, 0xfe, 0x00, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x01, 0xf8, 0x01, 0xff, 0xff, 0x00, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0xfc, 0x03, 0xf8, 0x3f, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x7e, 0x03, 0xf0, 0x1f, 0x80, 0x7e, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x7e, 0x03, 0xe0, 0x0f, 0x80, 0x7e, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x07, 0xc0, 0x07, 0xc0, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x1f, 0x07, 0xc0, 0x07, 0xc0, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x1f, 0x07, 0xc0, 0x07, 0xc0, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x07, 0xc0, 0x07, 0xc0, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x7e, 0x07, 0xc0, 0x07, 0xc0, 0x7e, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x7e, 0x03, 0xe0, 0x0f, 0x80, 0x7e, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x40, 0x00, 0x00, 0xfc, 0x03, 0xf0, 0x1f, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x80, 0x00, 0x00, 0xf8, 0x03, 0xf8, 0x3f, 0x80, 0x1f, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x02, 0x20, 0x00, 0x01, 0xf8, 0x01, 0xff, 0xff, 0x00, 0x1f, 0x80, 0x00, 0x18, 0x00, 0x00, 
0x0f, 0x83, 0x20, 0x00, 0x03, 0xf0, 0x00, 0xff, 0xfe, 0x00, 0x0f, 0xc1, 0xf0, 0x14, 0x00, 0x00, 
0x08, 0xc1, 0xc0, 0x00, 0x03, 0xf0, 0x00, 0x7f, 0xfc, 0x00, 0x07, 0xc1, 0x0c, 0x22, 0x00, 0x00, 
0x08, 0x40, 0x00, 0x00, 0x07, 0xe0, 0x00, 0x3f, 0xf8, 0x00, 0x07, 0xe1, 0x06, 0x00, 0x00, 0x00, 
0x08, 0x43, 0xe2, 0x70, 0x0f, 0xc0, 0x00, 0x07, 0xc0, 0x00, 0x03, 0xf1, 0x06, 0x3e, 0x31, 0x80, 
0x08, 0xc4, 0x23, 0x98, 0x0f, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0xf1, 0x02, 0x42, 0x31, 0x80, 
0x0f, 0x80, 0x33, 0x08, 0x1f, 0xff, 0xff, 0x80, 0x01, 0xff, 0xff, 0xff, 0xc2, 0x03, 0x31, 0x80, 
0x08, 0x63, 0xf3, 0x08, 0x1f, 0xff, 0xff, 0xc0, 0x03, 0xff, 0xff, 0xf9, 0x02, 0x3f, 0x31, 0x80, 
0x08, 0x26, 0x33, 0x08, 0x1f, 0xff, 0xff, 0xc0, 0x03, 0xff, 0xff, 0xf9, 0x06, 0x63, 0x31, 0x80, 
0x08, 0x24, 0x33, 0x08, 0x0f, 0xff, 0xff, 0xe0, 0x07, 0xff, 0xff, 0xf1, 0x04, 0x43, 0x31, 0x80, 
0x08, 0x66, 0x73, 0x98, 0x00, 0x00, 0x03, 0xf0, 0x0f, 0xc0, 0x00, 0x01, 0x0c, 0x67, 0x13, 0x80, 
0x0f, 0xc3, 0xf3, 0x70, 0x00, 0x00, 0x03, 0xf8, 0x1f, 0xc0, 0x00, 0x01, 0xf0, 0x3f, 0x1c, 0x80, 
0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0xf8, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0xfc, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7e, 0x7e, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7e, 0x7e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};