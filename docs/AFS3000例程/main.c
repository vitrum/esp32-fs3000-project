#include "stm32f10x.h"
#include "bsp_usart1.h"
#include "HDC1080.h"
#include "Modbus.h"
#include "LCD_144.h"
#include "key.h"
#include "sf05.h"
#include "i2c_hal.h"
#include "wdg.h"
#include "stdio.h"
u32t serialNumber; // sensor serial number
u16t flow_u16;     // measured flow value
ft   flow_ft[12];      // measured flow value
ft   dfpress_ft[12];  
ft   temp_ft[12];  
extern unsigned short disp_cnt;

    

// 软件延时
void Delay(__IO uint32_t nCount)
{
  for(; nCount != 0; nCount--);
} 

/**
  * @brief  主函数
  * @param  无
  * @retval 无
  */
uint8  cmd_buffer[CMD_MAX_SIZE];                                                     //????
static uint16 current_screen_id = 0;                                                 //????ID
static int32 progress_value = 0;                                                     //??????
static uint8 update_en = 0;                                                          //????
static int32 meter_flag = 0;                                                         //?????????
static int32 num = 0;                                                                //???????
static uint8 sec = 1;                                                                //???
static int32 curves_type = 0;                                                        //?????  0????,1????                  
static int32 second_flag=0;                                                          //?????
extern uchar update_en_flag;                                                          //????
extern uchar poweron_flag;
extern uint32 poweron_cnt;
uchar i2c_RX_Buf[20];
uchar crc8_voc;
extern uchar update_flag;
int cser;


void SysClockInit(void)
{
//配置内置振荡器 36MHz
	RCC_DeInit(); /*将外设RCC寄存器重设为缺省值 */ 
	RCC_HSICmd(ENABLE); //使能内部时钟  HSI
	while(RCC_GetFlagStatus(RCC_FLAG_HSIRDY)== RESET);//等待HSI就绪 
	 
	RCC_HCLKConfig(RCC_SYSCLK_Div1);   /*设置AHB时钟（HCLK） RCC_SYSCLK_Div1——AHB时钟 = 系统时*/  
	RCC_PCLK2Config(RCC_HCLK_Div1);   /* 设置高速AHB时钟（PCLK2)RCC_HCLK_Div1——APB2时钟 = HCLK*/     
	RCC_PCLK1Config(RCC_HCLK_Div2); /*设置低速AHB时钟（PCLK1）RCC_HCLK_Div2——APB1时钟 = HCLK / 2*/   
 
	 //下面这语句设置时钟频率  记住参考
	RCC_PLLConfig(RCC_PLLSource_HSI_Div2, RCC_PLLMul_16);/*设置PLL时钟源及倍频系数，频率为8/2*9=36Mhz*/    
	RCC_PLLCmd(ENABLE); 	 /*使能PLL *///这里使用的内部时钟所以不能被 失能 
	while(RCC_GetFlagStatus(RCC_FLAG_PLLRDY) == RESET) ; /*检查指定的RCC标志位(PLL准备好标志)设置与否   等待是否配置成功*/    
	
	RCC_SYSCLKConfig(RCC_SYSCLKSource_PLLCLK);  /*设置系统时钟（SYSCLK） */  
	  //选择哪一个做为时钟   
	  //0x00：HSI 作为系统时钟     RCC_SYSCLKSource_HSI
    //  0x04：HSE作为系统时钟 			 RCC_SYSCLKSource_HSE
    //  0x08：PLL作为系统时钟 			RCC_SYSCLKSource_PLLCLK 
	 
	while(RCC_GetSYSCLKSource() != 0x08);//需与被选择的系统时钟对应起来，RCC_SYSCLKSource_PLL

}





int main(void)
{	
	unsigned char add,cx,cy;
	uint32_t ID=0;
	int32_t flowAdc=0;
	int32_t tempAdc=0;
	int32_t slope_k;
	int32_t KNBN=0;
	etError error = NO_ERROR; // error code
	SysClockInit();
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);
	GPIO_PinRemapConfig(GPIO_Remap_PD01, ENABLE);  //启用PD0和PD1端口作为GPIO
	GPIO_PinRemapConfig(GPIO_Remap_SWJ_JTAGDisable,ENABLE); // PB3，PB4，PA15做普通IO，PA13&14用于SWD调试  
	cx = 12; cy = 113;
	TIM3_Config();
	LCD_Init(); 		
	Init_KeyPORT();
	Delay_Nms(10);
	Start_System((uint8_t)(3000));
	Delay_Nms(1000);
	LCD_Clear(BLACK);
	Start_System1((uint8_t)(3000));
	USART3_Config();
	SF05_Init();	
	InitGpioType();
	int cnt;
	float average,str[80],tempflow;
	
	while(1)
	{ 	

		error=SFM3000_GetDiffPress(SEN04, 60, &dfpress_ft[SEN04],&temp_ft[SEN04]);    //sfm3000读取函数

			str[cnt] = dfpress_ft[SEN04];
			cnt ++;
	
			if(cnt == 80)
			{
				cnt = 0;
				for(int i=0;i<50;i++)
				{
					average+=str[i];
				}
				tempflow = average/50;
				average = 0;
			
			}
}
	}

void WriteOTPA(void)
{
	uint16_t len = 0;
  uint32_t wdat = 0;
	uint16_t i = 0;
	unsigned char buf[10];
	float 	temp1_float;
  int 	temp1_int;
	uint16_t cmdty = 0;
	// I2C write
	//error = SF05_WriteData(&RX_Data[3], 6);//01 06 06 \55 5D - C7 DA C9 00
	
	for(len=0;len<95;len++) 
  {
		IWDG_Feed();//喂狗
	 // SF05_WriteData(scl_gpio_typedef[0],scl_gpio_pin[0],sda_gpio_typedef[0],sda_gpio_pin[0],&wotpbuf[len][0], 6);
		//wdat = (wotpbuf[len][2]<<24)|(wotpbuf[len][3]<<16)|(wotpbuf[len][4]<<8)|(wotpbuf[len][5]);
		printf("write 0x%x \r\n",wdat);			
		Delay_Nms(500);
		
		cmdty = 0xD5;//D5nn
		cmdty = cmdty<<8;
		cmdty = cmdty|i;
		SF05_WriteCommand(0,cmdty);
		Delay_Nms(100);
		SF05_ReadData(scl_gpio_typedef[0],scl_gpio_pin[0],sda_gpio_typedef[0],sda_gpio_pin[0],&buf[2], 4);	
		temp1_int = ((buf[2]<<24) + (buf[3]<<16) + (buf[4]<<8) + (buf[5]));
		temp1_float = convert_float_data(temp1_int);
		printf("temp1_float:0x%x,%3.1f,\r\n", cmdty,temp1_float);	
	}
								

}

void ReadADC(void)
{
	uint16_t i = 0;
	unsigned char buf[10];

  int 	temp1_int;
  etError error = NO_ERROR; 

	SF05_WriteCommand(i,0X4a00);	
	Delay_Nms(20);
	error=SF05_ReadData(scl_gpio_typedef[0],scl_gpio_pin[0],sda_gpio_typedef[0],sda_gpio_pin[0],&buf[2+0*4], 4);
if(error==NO_ERROR)
{
	temp1_int = (int)(((buf[2]<<24) + (buf[3]<<16) + (buf[4]<<8) + (buf[5])));

	printf("  %d  \r\n", temp1_int);	
}else
{
	printf("  error  \r\n");
}

Delay_Nms(20);
					

}
void ReadOTPA(void)
{
	uint16_t i = 0;
	unsigned char buf[10];
	float 	temp1_float;
  int 	temp1_int;
	uint16_t cmdty = 0;
	for(i=0;i<94;i++)
	{
		IWDG_Feed();//喂狗
		cmdty = 0xD5;//D5nn
		cmdty = cmdty<<8;
		cmdty = cmdty|i;
		SF05_WriteCommand(0,cmdty);
		Delay_Nms(100);
		SF05_ReadData(scl_gpio_typedef[0],scl_gpio_pin[0],sda_gpio_typedef[0],sda_gpio_pin[0],&buf[2], 4);	
		temp1_int = ((buf[2]<<24) + (buf[3]<<16) + (buf[4]<<8) + (buf[5]));
		temp1_float = convert_float_data(temp1_int);
		printf("temp1_float:0x%x,%3.1f,\r\n", cmdty,temp1_float);	
		//printf("temp1_float:%3.1f \r\n", temp1_float);	
	}
	Delay_Nms(20000);
}

void ReadADP800ID(void)
{
	unsigned char buf[10];
	etError error = NO_ERROR; 
	int 	temp1_int;
	SF05_WriteCommand(0,0x31AE);	
	Delay_Nms(100);
	error = SF05_ReadDataID(scl_gpio_typedef[0],scl_gpio_pin[0],sda_gpio_typedef[0],sda_gpio_pin[0],&buf[0], 2);
	temp1_int = ((buf[0]<<24) + (buf[1]<<16) + (buf[2]<<8) + (buf[3]));
  if(error!=NO_ERROR)//如果是错误值，返回FF FF FF FF
	{
		 memset(&buf[0],0xff,4);
	}
 			
	sprintf(lcdstr,"1#ID:%d   ",temp1_int);
  LCD_WriteLineASCII_Std(0, 8, 0,lcdstr);
}