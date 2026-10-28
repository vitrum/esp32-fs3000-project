//==============================================================================
//    S E N S I R I O N   AG,  Laubisruetistr. 50, CH-8712 Staefa, Switzerland
//==============================================================================
// Project   :  SF05 Sample Code (V1.1)
// File      :  sf05.c (V1.2)
// Author    :  RFU
// Date      :  20-Aug-2015
// Controller:  STM32F100RB
// IDE       :  µVision V4.60.0.0
// Compiler  :  Armcc
// Brief     :  Sensor Layer: Implementation of functions for sensor access.
//==============================================================================

//-- Includes ------------------------------------------------------------------
#include "sf05.h"
#include "i2c_hal.h"
#include "stdio.h"

extern void Delay_Nms(uint32_t ms);	
unsigned short	device_status;
unsigned char lcdstr[30];
//-- Global Variables ----------------------------------------------------------
u16t currentCommand = 0x0000;


//==============================================================================
etError ADP810_ReadCommandResult(unsigned char senNo,u8t byteNum,u8t *result){   //IIC read
//==============================================================================
	etError error;    // error code
	//u8t     checksum; // checksum byte
//u8t     data[2];  // read data array
	u8t     ri;
	// read command result & checksum from sensor
	I2c_StartCondition(scl_gpio_typedef[senNo],scl_gpio_pin[senNo],sda_gpio_typedef[senNo],sda_gpio_pin[senNo]);
	error    = I2c_WriteByte(scl_gpio_typedef[senNo],scl_gpio_pin[senNo],sda_gpio_typedef[senNo],sda_gpio_pin[senNo],I2C_ADR << 1 | I2C_READ);
	for(ri=0;ri<byteNum-1;ri++)//3-1 = 2 0 1
	{
	  result[ri]  = I2c_ReadByte(scl_gpio_typedef[senNo],scl_gpio_pin[senNo],sda_gpio_typedef[senNo],sda_gpio_pin[senNo],ACK);
	}
	result[ri] = I2c_ReadByte(scl_gpio_typedef[senNo],scl_gpio_pin[senNo],sda_gpio_typedef[senNo],sda_gpio_pin[senNo],NO_ACK);
	
	I2c_StopCondition(scl_gpio_typedef[senNo],scl_gpio_pin[senNo],sda_gpio_typedef[senNo],sda_gpio_pin[senNo]);
  //result[0] result[1] result[2]


	return error;
}



//==============================================================================
etError SF05_CheckCrc(u8t data[], u8t nbrOfBytes, u8t checksum){
//==============================================================================
	u8t bit;     // bit mask
	u8t crc = 0x00; // calculated checksum
	u8t byteCtr; // byte counter

	// calculates 8-Bit checksum with given polynomial
	for(byteCtr = 0; byteCtr < nbrOfBytes; byteCtr++)
	{
		crc ^= (data[byteCtr]);
		for(bit = 8; bit > 0; --bit)
		{
			  if(crc & 0x80) 
				  crc = (crc << 1) ^ POLYNOMIAL;
			  else           
				  crc = (crc << 1);
		}
	}

	// verify checksum
	if(crc != checksum) 
		return CHECKSUM_ERROR;
	else                
		return NO_ERROR;

}




//==============================================================================
etError SFM3000_GetDiffPress(unsigned char senN,ft scale, ft *diffPress, ft *tempature){
//==============================================================================
	etError error = NO_ERROR; // error code
	u8t  result[10];           // read result from sensor
	int16_t ftmp = 0;



	DelayMicroSeconds(120);		// none

	// if no error, read command result
	if(error == NO_ERROR)
	error = ADP810_ReadCommandResult(senN,3,result);
	
	error  = SF05_CheckCrc (&result[0], 2, result[2]);
	ftmp  = (result[0]*256+result[1]);
	
	
	if(error == NO_ERROR)
	{
		*diffPress = ftmp*0.005;
	}

	return error;
}



