#ifndef _DISPLAY_EPD_W21_SPI_
#define _DISPLAY_EPD_W21_SPI_
#include "Arduino.h"

//IO settings
#define waveshare 1 //微雪驱动板
#define goodisplay 0 //佳显驱动板
#define epdESP32C3 2 //墨水屏小店ESP32C3驱动板
#define ESP32C3 4 //墨鱼定义
#define S3 3 //ESP32S3驱动板

#define driveboard 1  //InkWord S3 板 EVK011 接线（SCK=7/MOSI=8/DC=9/CS=10/BUSY=12/RST=13）

#if driveboard == waveshare//InkWord EVK011 J2 排针实测接线（S3）
const int BUSY_Pin = 12;    /* J2 pin9 */
const int RES_Pin = 13;     /* J2 pin8 */
const int DC_Pin = 9;       /* J2 pin7 */
const int CS_Pin = 10;      /* J2 pin6 */
const int SCK_Pin = 7;      /* J2 pin3 */
const int SDI_Pin = 8;      /* J2 pin5 */

#elif driveboard == S3//ESP32S3驱动板
const int BUSY_Pin = 4; 
const int RES_Pin = 11; 
const int DC_Pin = 10; 
const int CS_Pin = 1; 
const int SCK_Pin = 13; 
const int SDI_Pin = 7; 

#elif driveboard == epdESP32C3//墨水屏小店ESP32C3驱动板
const int BUSY_Pin = 8; 
const int RES_Pin = 5; 
const int DC_Pin = 4; 
const int CS_Pin = 3; 
const int SCK_Pin = 6; 
const int SDI_Pin = 7; 

#elif driveboard == ESP32C3//墨水屏小店ESP32C3驱动板
const int BUSY_Pin = 10; 
const int RES_Pin = 2; 
const int DC_Pin = 1; 
const int CS_Pin = 7; 
const int SCK_Pin = 4; 
const int SDI_Pin = 6; 

#elif driveboard == goodisplay//佳显驱动板
const int BUSY_Pin = A14; 
const int RES_Pin = A15; 
const int DC_Pin = A16; 
const int CS_Pin = A17; 
const int SCK_Pin = A18; 
const int SDI_Pin = A19; 
#endif

#define EPD_W21_CS_0 digitalWrite(CS_Pin,LOW)
#define EPD_W21_CS_1 digitalWrite(CS_Pin,HIGH)
#define EPD_W21_DC_0  digitalWrite(DC_Pin,LOW)
#define EPD_W21_DC_1  digitalWrite(DC_Pin,HIGH)
#define EPD_W21_RST_0 digitalWrite(RES_Pin,LOW)
#define EPD_W21_RST_1 digitalWrite(RES_Pin,HIGH)
#define isEPD_W21_BUSY digitalRead(BUSY_Pin)

void SPI_Write(unsigned char value);
void EPD_W21_WriteDATA(unsigned char datas);
void EPD_W21_WriteCMD(unsigned char command);


#endif 
