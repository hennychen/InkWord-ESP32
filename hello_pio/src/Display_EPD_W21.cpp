#include "Display_EPD_W21_spi.h"
#include "Display_EPD_W21.h"
uint8_t PARTIAL_2IN13[159] =
{
	0x40, 0x00, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x80, 0x00, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x40, 0x00, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x80, 0x00, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x1, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,//TP[0A]=1帧
	0x00, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x0, 0x0, 0x0,
	0x22, 0x17, 0x41, 0x00, 0x32, 0x36,
};

/* 最后一笔用LUT: TP[0A]=3帧(索引60=0x03), 其余与PARTIAL_2IN13相同 */
uint8_t PARTIAL_2IN13_LAST[159] =
{
	0x40, 0x00, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x80, 0x00, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x40, 0x00, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x80, 0x00, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x1F, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,//TP[0A]=3帧
	0x00, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x0, 0x0, 0x0,
	0x22, 0x17, 0x41, 0x00, 0x32, 0x36,
};

//Delay Functions
void delay_xms(unsigned int xms)
{
  delay(xms);
}

////////////////////////////////////E-paper demo//////////////////////////////////////////////////////////
//Busy function
void Epaper_READBUSY(void)
{ 
  while(1)
  {	 //=1 BUSY
     if(isEPD_W21_BUSY==0) break;
  }  
}
static void EPD_PRT_LUT(uint8_t *lut)
{       
	uint8_t count;
	EPD_W21_WriteCMD(0x32);
	for(count=0; count<159; count++) 
		EPD_W21_WriteDATA(lut[count]); 
	EPD_W21_WriteCMD(0x3f);
	EPD_W21_WriteDATA(*(lut+153));
	EPD_W21_WriteCMD(0x03);	// gate voltage
	EPD_W21_WriteDATA(*(lut+154));
	EPD_W21_WriteCMD(0x04);	// source voltage
	EPD_W21_WriteDATA(*(lut+155));	// VSH
	EPD_W21_WriteDATA(*(lut+156));	// VSH2
	EPD_W21_WriteDATA(*(lut+157));	// VSL
	EPD_W21_WriteCMD(0x2c);		// VCOM
	EPD_W21_WriteDATA(*(lut+158));
}
//Full screen update initialization
void EPD_HW_Init(void)
{
	EPD_W21_RST_0;  // Module reset   
	delay_xms(10);//At least 10ms delay 
	EPD_W21_RST_1;
	delay_xms(10); //At least 10ms delay 
	
	Epaper_READBUSY();   
	EPD_W21_WriteCMD(0x12);  //SWRESET
	Epaper_READBUSY();   
		
	EPD_W21_WriteCMD(0x01); //Driver output control      
	EPD_W21_WriteDATA((EPD_HEIGHT-1)%256);    
	EPD_W21_WriteDATA((EPD_HEIGHT-1)/256);
	EPD_W21_WriteDATA(0x00);

	EPD_W21_WriteCMD(0x11); //data entry mode       
	EPD_W21_WriteDATA(0x01);

	EPD_W21_WriteCMD(0x44); //set Ram-X address start/end position   
	EPD_W21_WriteDATA(0x00);
	EPD_W21_WriteDATA(EPD_WIDTH/8-1);   

	EPD_W21_WriteCMD(0x45); //set Ram-Y address start/end position          
	EPD_W21_WriteDATA((EPD_HEIGHT-1)%256);    
	EPD_W21_WriteDATA((EPD_HEIGHT-1)/256);
	EPD_W21_WriteDATA(0x00);
	EPD_W21_WriteDATA(0x00); 

	EPD_W21_WriteCMD(0x3C); //BorderWavefrom
	EPD_W21_WriteDATA(0x05);	

	EPD_W21_WriteCMD(0x21); //  Display update control
	EPD_W21_WriteDATA(0x00);		
  EPD_W21_WriteDATA(0x80);	
	
  EPD_W21_WriteCMD(0x18); //Read built-in temperature sensor
	EPD_W21_WriteDATA(0x80);	

	EPD_W21_WriteCMD(0x4E);   // set RAM x address count to 0;
	EPD_W21_WriteDATA(0x00);
	EPD_W21_WriteCMD(0x4F);   // set RAM y address count to 0X199;    
	EPD_W21_WriteDATA((EPD_HEIGHT-1)%256);    
	EPD_W21_WriteDATA((EPD_HEIGHT-1)/256);
}
//Fast full screen update initialization
void EPD_HW_Init_Fast(void)		// 1.6s 
{
	EPD_W21_RST_0;  // Module reset   
	delay_xms(10);//At least 10ms delay 
	EPD_W21_RST_1;
	delay_xms(10); //At least 10ms delay   
	
	EPD_W21_WriteCMD(0x12);  //SWRESET
	Epaper_READBUSY();   
 	
  EPD_W21_WriteCMD(0x18); //Read built-in temperature sensor
	EPD_W21_WriteDATA(0x80);	
	  	
	EPD_W21_WriteCMD(0x22); // Load temperature value
	EPD_W21_WriteDATA(0xB1);		
  EPD_W21_WriteCMD(0x20);	
  Epaper_READBUSY();   

	EPD_W21_WriteCMD(0x1A); // Write to temperature register
	EPD_W21_WriteDATA(0x5a);		// 90		
  EPD_W21_WriteDATA(0x00);	
				  	
	EPD_W21_WriteCMD(0x22); // Load temperature value
	EPD_W21_WriteDATA(0x91);		
  EPD_W21_WriteCMD(0x20);	
	Epaper_READBUSY();   
}
//////////////////////////////Display Update Function///////////////////////////////////////////////////////
//Full screen update function
void EPD_Update(void)
{   
  EPD_W21_WriteCMD(0x22); //Display Update Control
  EPD_W21_WriteDATA(0xF7);   
  EPD_W21_WriteCMD(0x20); //Activate Display Update Sequence
  Epaper_READBUSY();   

}
//Fast full screen  update function
void EPD_Update_Fast(void)
{   
  EPD_W21_WriteCMD(0x22); //Display Update Control 
	EPD_W21_WriteDATA(0xC7);   
  EPD_W21_WriteCMD(0x20); //Activate Display Update Sequence
  Epaper_READBUSY();   

}
// Full screen  update function (Basemap)
void EPD_Update_BaseMap(void)
{   
  EPD_W21_WriteCMD(0x22); //Display Update Control
  EPD_W21_WriteDATA(0xF4);   
  EPD_W21_WriteCMD(0x20); //Activate Display Update Sequence
  Epaper_READBUSY();   

}
//Partial update function
void EPD_Part_Update(void)
{
	EPD_W21_WriteCMD(0x22); //Display Update Control
	EPD_W21_WriteDATA(0x1C);   
	EPD_W21_WriteCMD(0x20); //Activate Display Update Sequence
	Epaper_READBUSY(); 			
}
//////////////////////////////Display Data Transfer Function////////////////////////////////////////////
//Full screen update display function
void EPD_WhiteScreen_ALL(const unsigned char* datasBW,const unsigned char* datasRW)
{
    unsigned int i;
	  //Write Data
		EPD_W21_WriteCMD(0x24);	       //Transfer old data
	  for(i=0;i<EPD_ARRAY;i++)	  
    {	
	    EPD_W21_WriteDATA(datasBW[i]);  //Transfer the actual displayed data
    }	
		EPD_W21_WriteCMD(0x26);		     //Transfer new data
	  for(i=0;i<EPD_ARRAY;i++)	     
	  {
	    EPD_W21_WriteDATA(~datasRW[i]);  //Transfer the actual displayed data
	  }
    EPD_Update();	    

}
//Fast full screen update display function
void EPD_WhiteScreen_ALL_Fast(const unsigned char* datasBW,const unsigned char* datasRW)
{
    unsigned int i;
	  //Write Data
		EPD_W21_WriteCMD(0x24);	       //Transfer old data
	  for(i=0;i<EPD_ARRAY;i++)	  
    {	
	    EPD_W21_WriteDATA(datasBW[i]);  //Transfer the actual displayed data
    }	
		EPD_W21_WriteCMD(0x26);		     //Transfer new data
	  for(i=0;i<EPD_ARRAY;i++)	     
	  {
	    EPD_W21_WriteDATA(~datasRW[i]);  //Transfer the actual displayed data
	  }
    EPD_Update_Fast();	    

}
//Clear screen display
void EPD_WhiteScreen_White(void)
{
    unsigned int i;
	  //Write Data
		EPD_W21_WriteCMD(0x24);	       //Transfer old data
	  for(i=0;i<EPD_ARRAY;i++)	  
    {	
	    EPD_W21_WriteDATA(0xFF); 
    }
		EPD_W21_WriteCMD(0x26);		     //Transfer new data
	  for(i=0;i<EPD_ARRAY;i++)	     
	  {
	    EPD_W21_WriteDATA(0x00);  //Transfer the actual displayed data
	  }
    EPD_Update();	
	
}
//Clear screen display
void EPD_WhiteScreen_White_Fast(void)
{
    unsigned int i;
	
		EPD_W21_WriteCMD(0x3c);  // set border
		EPD_W21_WriteDATA(0x01);     // 0x00=black £» 0x01=white £» 0x80=VCOM £» 0xc0=HiZ£¨default£©	 	
	  //Write Data
		EPD_W21_WriteCMD(0x24);	       //Transfer old data
	  for(i=0;i<EPD_ARRAY;i++)	  
    {	
	    EPD_W21_WriteDATA(0xFF); 
    }
		EPD_W21_WriteCMD(0x26);		     //Transfer new data
	  for(i=0;i<EPD_ARRAY;i++)	     
	  {
	    EPD_W21_WriteDATA(0x00);  //Transfer the actual displayed data
	  }
    EPD_Update_Fast();	

		EPD_W21_WriteCMD(0x26);		     //Transfer new data
	  for(i=0;i<EPD_ARRAY;i++)	     
	  {
	    EPD_W21_WriteDATA(0xFF);  //Transfer the actual displayed data
	  }		
}
//Display all black
void EPD_WhiteScreen_Black(void)
{
	unsigned int i;
	//Write Data
	EPD_W21_WriteCMD(0x24);	       //Transfer old data
	for(i=0;i<EPD_ARRAY;i++)	  
	{	
		EPD_W21_WriteDATA(0xFF); 
	}
	EPD_W21_WriteCMD(0x26);		     //Transfer new data
	for(i=0;i<EPD_ARRAY;i++)	     
	{
		EPD_W21_WriteDATA(0xFF);  //Transfer the actual displayed data
	}
	EPD_Update();	
}

//Partial update of background display, this function is necessary, please do not delete it!!!
void EPD_SetRAMValue_BaseMap(const unsigned char* datasBW,const unsigned char* datasRW)
{
    unsigned int i;
	  unsigned char a;
	  //Write Data
		EPD_W21_WriteCMD(0x24);	       //Transfer old data
	  for(i=0;i<EPD_ARRAY;i++)	  
    {	
	    EPD_W21_WriteDATA(datasBW[i]);  //Transfer the actual displayed data
    }	
		EPD_W21_WriteCMD(0x26);		     //Transfer new data
	  for(i=0;i<EPD_ARRAY;i++)	     
	  {
	    EPD_W21_WriteDATA(~datasRW[i]);  //Transfer the actual displayed data
	  }
    EPD_Update_BaseMap();	  	 
	  Epaper_READBUSY();   
		
	  //update
		EPD_W21_WriteCMD(0x24);	       //Transfer old data
	  for(i=0;i<EPD_ARRAY;i++)	  
    {	
			a=datasBW[i]&datasRW[i];
	    EPD_W21_WriteDATA(a);  //Transfer the actual displayed data
    }	
		EPD_W21_WriteCMD(0x26);		     //Transfer new data
	  for(i=0;i<EPD_ARRAY;i++)	  
    {	
			a=datasBW[i]&datasRW[i];
	    EPD_W21_WriteDATA(a);  //Transfer the actual displayed data
    }	
		
		//////////////////////////////////////////////
}
//Partial update display
void EPD_Dis_Part(unsigned int x_start,unsigned int y_start,const unsigned char * datas,unsigned int PART_COLUMN,unsigned int PART_LINE)
{
	unsigned int i;  
	unsigned int x_end,y_end;
	
	x_start=x_start/8; //x address start
	x_end=x_start+PART_LINE/8-1; //x address end
	y_start=y_start; //Y address start
	y_end=y_start+PART_COLUMN-1; //Y address end

	EPD_W21_WriteCMD(0x44);       // set RAM x address start/end
	EPD_W21_WriteDATA(x_start);  //x address start
	EPD_W21_WriteDATA(x_end);   //y address end   
	EPD_W21_WriteCMD(0x45);    // set RAM y address start/end
	EPD_W21_WriteDATA(y_start%256);  //y address start2 
	EPD_W21_WriteDATA(y_start/256); //y address start1 
	EPD_W21_WriteDATA(y_end%256);  //y address end2 
	EPD_W21_WriteDATA(y_end/256); //y address end1   

	EPD_W21_WriteCMD(0x4E);        // set RAM x address count to 0;
	EPD_W21_WriteDATA(x_start);   //x start address
	EPD_W21_WriteCMD(0x4F);      // set RAM y address count to 0X127;    
	EPD_W21_WriteDATA(y_start%256);//y address start2
	EPD_W21_WriteDATA(y_start/256);//y address start1
		EPD_PRT_LUT(PARTIAL_2IN13);
	
	 EPD_W21_WriteCMD(0x24);   //Write Black and White image to RAM
   for(i=0;i<PART_COLUMN*PART_LINE/8;i++)
   {                         
     EPD_W21_WriteDATA(datas[i]);
   } 
	 EPD_Part_Update();

}


//全屏局部刷新: Y计数器从295递减, 覆盖完整128x296帧缓存
void EPD_Full_PartUpdate(const unsigned char* datas)
{
	unsigned int i;

	EPD_W21_WriteCMD(0x44);       // set RAM x address start/end
	EPD_W21_WriteDATA(0x00);      // x start = 0
	EPD_W21_WriteDATA(EPD_WIDTH/8-1); // x end = 15
	EPD_W21_WriteCMD(0x45);       // set RAM y address start/end
	EPD_W21_WriteDATA((EPD_HEIGHT-1)%256); // y start = 295
	EPD_W21_WriteDATA((EPD_HEIGHT-1)/256);
	EPD_W21_WriteDATA(0x00);      // y end = 0
	EPD_W21_WriteDATA(0x00);

	EPD_W21_WriteCMD(0x4E);       // set RAM x address count
	EPD_W21_WriteDATA(0x00);
	EPD_W21_WriteCMD(0x4F);       // set RAM y address count = 295
	EPD_W21_WriteDATA((EPD_HEIGHT-1)%256);
	EPD_W21_WriteDATA((EPD_HEIGHT-1)/256);

	EPD_PRT_LUT(PARTIAL_2IN13);   // 加载局刷LUT和电压

	EPD_W21_WriteCMD(0x24);       // 写新帧到RAM
	for(i=0;i<EPD_ARRAY;i++)
	{
		EPD_W21_WriteDATA(datas[i]);
	}
	EPD_Part_Update();
}


//窗口局部刷新: px_start/px_width为竖排X(128维,须8对齐), py_start/py_height为竖排Y(296维,可任意)
//Y递减模式: Y start=较大值, Y end=较小值, 计数器从大到小
void EPD_Window_PartUpdate(unsigned int px_start, unsigned int py_start, const unsigned char* datas, unsigned int px_width, unsigned int py_height, const uint8_t* lut)
{
	unsigned int i;
	unsigned int x_start_byte = px_start / 8;
	unsigned int x_end_byte = x_start_byte + px_width / 8 - 1;
	unsigned int y_end = py_start + py_height - 1; // 较大的Y值(递减起点)

	EPD_W21_WriteCMD(0x44);       // set RAM x address start/end
	EPD_W21_WriteDATA(x_start_byte);
	EPD_W21_WriteDATA(x_end_byte);
	EPD_W21_WriteCMD(0x45);       // set RAM y address start/end
	EPD_W21_WriteDATA(y_end % 256);    // Y start = 较大值
	EPD_W21_WriteDATA(y_end / 256);
	EPD_W21_WriteDATA(py_start % 256); // Y end = 较小值
	EPD_W21_WriteDATA(py_start / 256);

	EPD_W21_WriteCMD(0x4E);       // set RAM x address count
	EPD_W21_WriteDATA(x_start_byte);
	EPD_W21_WriteCMD(0x4F);       // set RAM y address count = 较大值(递减)
	EPD_W21_WriteDATA(y_end % 256);
	EPD_W21_WriteDATA(y_end / 256);

	if (lut != NULL) {
		EPD_PRT_LUT((uint8_t*)lut);
	}

	EPD_W21_WriteCMD(0x24);       // 写新帧到RAM
	for(i=0; i < py_height * px_width / 8; i++)
	{
		EPD_W21_WriteDATA(datas[i]);
	}
	EPD_Part_Update();
}

//Deep sleep function
void EPD_DeepSleep(void)
{  	
  EPD_W21_WriteCMD(0x10); //Enter deep sleep
  EPD_W21_WriteDATA(0x01); 
  delay_xms(100);
}

//Partial update write address and data
void EPD_Dis_Part_RAM(unsigned int x_start,unsigned int y_start,const unsigned char * datas,unsigned int PART_COLUMN,unsigned int PART_LINE)
{
	unsigned int i;  
	unsigned int x_end,y_end;
	
	x_start=x_start/8; //x address start
	x_end=x_start+PART_LINE/8-1; //x address end
	
	y_start=y_start-1; //Y address start
	y_end=y_start+PART_COLUMN-1; //Y address end
		EPD_PRT_LUT(PARTIAL_2IN13);
	EPD_W21_WriteCMD(0x44);       // set RAM x address start/end
	EPD_W21_WriteDATA(x_start);  //x address start
	EPD_W21_WriteDATA(x_end);   //y address end   
	EPD_W21_WriteCMD(0x45);     // set RAM y address start/end
	EPD_W21_WriteDATA(y_start%256);  //y address start2 
	EPD_W21_WriteDATA(y_start/256); //y address start1 
	EPD_W21_WriteDATA(y_end%256);  //y address end2 
	EPD_W21_WriteDATA(y_end/256); //y address end1   

	EPD_W21_WriteCMD(0x4E);   // set RAM x address count to 0;
	EPD_W21_WriteDATA(x_start);   //x start address
	EPD_W21_WriteCMD(0x4F);   // set RAM y address count to 0X127;    
	EPD_W21_WriteDATA(y_start%256); //y address start2
	EPD_W21_WriteDATA(y_start/256); //y address start1
		
	EPD_W21_WriteCMD(0x24);   //Write Black and White image to RAM
  for(i=0;i<PART_COLUMN*PART_LINE/8;i++)
   {                         
     EPD_W21_WriteDATA(datas[i]);
   } 
}
//Num display
void EPD_Dis_Part_Num(unsigned int x_startA,unsigned int y_startA,const unsigned char * datasA,
	                       unsigned int x_startB,unsigned int y_startB,const unsigned char * datasB,
												 unsigned int x_startC,unsigned int y_startC,const unsigned char * datasC,
											   unsigned int x_startD,unsigned int y_startD,const unsigned char * datasD,
												 unsigned int PART_COLUMN,unsigned int PART_LINE
	                      )
{
	EPD_Dis_Part_RAM(x_startA,y_startA,datasA,PART_COLUMN,PART_LINE);
	EPD_Dis_Part_RAM(x_startB,y_startB,datasB,PART_COLUMN,PART_LINE);
	EPD_Dis_Part_RAM(x_startC,y_startC,datasC,PART_COLUMN,PART_LINE);
	EPD_Dis_Part_RAM(x_startD,y_startD,datasD,PART_COLUMN,PART_LINE);
	EPD_Part_Update();
}												 




////////////////////////////////Other newly added functions////////////////////////////////////////////
//Display rotation 180 degrees initialization
void EPD_HW_Init_180(void)
{
	EPD_W21_RST_0;  // Module reset   
	delay_xms(10); //At least 10ms delay 
	EPD_W21_RST_1;
	delay_xms(10); //At least 10ms delay 
	
	Epaper_READBUSY();   
	EPD_W21_WriteCMD(0x12);  //SWRESET
	Epaper_READBUSY();   
	
	EPD_W21_WriteCMD(0x3C); //BorderWavefrom
	EPD_W21_WriteDATA(0x05);
	
	EPD_W21_WriteCMD(0x01); //Driver output control      
	EPD_W21_WriteDATA((EPD_HEIGHT-1)%256);    
	EPD_W21_WriteDATA((EPD_HEIGHT-1)/256);
	EPD_W21_WriteDATA(0x00); 

	EPD_W21_WriteCMD(0x11); //data entry mode       
	EPD_W21_WriteDATA(0x02);

	EPD_W21_WriteCMD(0x44); //set Ram-X address start/end position   
	EPD_W21_WriteDATA(EPD_WIDTH/8-1);    
	EPD_W21_WriteDATA(0x00);  

	EPD_W21_WriteCMD(0x45); //set Ram-Y address start/end position          
	EPD_W21_WriteDATA(0x00);
	EPD_W21_WriteDATA(0x00); 
  EPD_W21_WriteDATA((EPD_HEIGHT-1)%256);  
	EPD_W21_WriteDATA((EPD_HEIGHT-1)/256);

  EPD_W21_WriteCMD(0x18); //Read built-in temperature sensor
	EPD_W21_WriteDATA(0x80);	

	EPD_W21_WriteCMD(0x4E);   // set RAM x address count to 0;
	EPD_W21_WriteDATA(EPD_WIDTH/8-1);    
	EPD_W21_WriteCMD(0x4F);   // set RAM y address count to 0X199;    
	EPD_W21_WriteDATA(0x00);
	EPD_W21_WriteDATA(0x00);
  Epaper_READBUSY();
}
// GUI initialization
void EPD_HW_Init_GUI(void)
{
	EPD_W21_RST_0;  // Module reset   
	delay_xms(10); //At least 10ms delay 
	EPD_W21_RST_1;
	delay_xms(10); //At least 10ms delay 
	
	Epaper_READBUSY();   
	EPD_W21_WriteCMD(0x12);  //SWRESET
	Epaper_READBUSY();   
		
	EPD_W21_WriteCMD(0x01); //Driver output control      
	EPD_W21_WriteDATA((EPD_HEIGHT+46-1)%256);    
	EPD_W21_WriteDATA((EPD_HEIGHT+46-1)/256);
	EPD_W21_WriteDATA(0x01);//Show mirror

	EPD_W21_WriteCMD(0x11); //data entry mode       
	EPD_W21_WriteDATA(0x01);

	EPD_W21_WriteCMD(0x44); //set Ram-X address start/end position   
	EPD_W21_WriteDATA(0x00);
	EPD_W21_WriteDATA(EPD_WIDTH/8-1);   

	EPD_W21_WriteCMD(0x45); //set Ram-Y address start/end position          
	EPD_W21_WriteDATA((EPD_HEIGHT+46-1)%256);   
	EPD_W21_WriteDATA((EPD_HEIGHT+46-1)/256);
	EPD_W21_WriteDATA(0x00);
	EPD_W21_WriteDATA(0x00); 

	EPD_W21_WriteCMD(0x3C); //BorderWavefrom
	EPD_W21_WriteDATA(0x05);	
	  	
  EPD_W21_WriteCMD(0x18); //Read built-in temperature sensor
	EPD_W21_WriteDATA(0x80);	

	EPD_W21_WriteCMD(0x4E);   // set RAM x address count to 0;
	EPD_W21_WriteDATA(0x00);
	EPD_W21_WriteCMD(0x4F);   // set RAM y address count to 0X199;    
	EPD_W21_WriteDATA((EPD_HEIGHT+46-1)%256);    
	EPD_W21_WriteDATA((EPD_HEIGHT+46-1)/256);
  Epaper_READBUSY();
	
}

//GUI display
void EPD_Display(unsigned char *Image)
{
	unsigned int Width, Height,i,j;
	Width = (EPD_WIDTH % 8 == 0)? (EPD_WIDTH / 8 ): (EPD_WIDTH / 8 + 1);
	Height = EPD_HEIGHT;

	EPD_W21_WriteCMD(0x24);
	for ( j = 0; j < Height; j++) {
			for ( i = 0; i < Width; i++) {
				 EPD_W21_WriteDATA(Image[i + j * Width]);
			}
	}
	EPD_Update();		 
}




/***********************************************************
						end file
***********************************************************/

