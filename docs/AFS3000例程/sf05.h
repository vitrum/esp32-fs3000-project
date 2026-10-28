//==============================================================================
//    S E N S I R I O N   AG,  Laubisruetistr. 50, CH-8712 Staefa, Switzerland
//==============================================================================
// Project   :  SF05 Sample Code (V1.1)
// File      :  sf05.h (V1.2)
// Author    :  RFU
// Date      :  20-Aug-2015
// Controller:  STM32F100RB
// IDE       :  µVision V4.60.0.0
// Compiler  :  Armcc
// Brief     :  Sensor Layer: Definitions of commands and functions for sensor
//                            access.
//==============================================================================

#ifndef SF05_H
#define SF05_H
#include "stm32f10x.h"
//-- Includes ------------------------------------------------------------------
//#include "system.h"

//-- Defines -------------------------------------------------------------------
// Offset and scale factors from datasheet (SFM3000).
#define OFFSET_FLOW 32000.0F   // offset flow
#define SCALE_FLOW    140.0F   // scale factor flow

#if 1
//-- Enumerations --------------------------------------------------------------
// Error codes
typedef enum{
  NO_ERROR       = 0x00, // no error
  ACK_ERROR      = 0x01, // no acknowledgment error
  CHECKSUM_ERROR = 0x02  // checksum mismatch error
}etError;
#endif

typedef unsigned char   u8t;      ///< range: 0 .. 255
typedef signed char     i8t;      ///< range: -128 .. +127
                                      
typedef unsigned short  u16t;     ///< range: 0 .. 65535
typedef signed short    i16t;     ///< range: -32768 .. +32767
                                      
typedef unsigned long   u32t;     ///< range: 0 .. 4'294'967'295
typedef signed long     i32t;     ///< range: -2'147'483'648 .. +2'147'483'647
                                      
typedef float           ft;       ///< range: +-1.18E-38 .. +-3.39E+38
typedef double          dt;      ///< range:            .. +-1.79E+308

#define bool _Bool

//#define I2C_ADR 	64
#define FW_VER		1
extern unsigned short	device_status;
extern unsigned char lcdstr[30];

//-- Defines -------------------------------------------------------------------
// CRC
#define POLYNOMIAL  0x131    // P(x) = x^8 + x^5 + x^4 + 1 = 100110001

//-- Enumerations --------------------------------------------------------------
// Sensor Commands
typedef enum{
  FLOW_MEASUREMENT        = 0x1000, // command: flow measurement
  READ_SERIAL_NUMBER_HIGH = 0x31AE, // command: read serial number (bit 31:16)
  READ_SERIAL_NUMBER_LOW  = 0x31AF, // command: read serial number (bit 15:0)
  SOFT_RESET              = 0X2000  // command: soft reset
}etCommands;

//==============================================================================
void SF05_Init(void);
//==============================================================================
// Initializes the I2C bus for communication with the sensor.
//------------------------------------------------------------------------------

//==============================================================================
etError SF05_WriteCommand(unsigned char senNo,etCommands cmd);
//==============================================================================
// Writes command to the sensor.
//------------------------------------------------------------------------------
// input:  cmd          command which is to be written to the sensor
//
// return: error:       ACK_ERROR = no acknowledgment from sensor
//                      NO_ERROR  = no error

//==============================================================================
etError SF05_ReadCommandResult(unsigned char senNo,u16t *result);
//==============================================================================
// Reads command results from sensor.
//------------------------------------------------------------------------------
// input:  *result      pointer to an integer where the result will be stored
//
// return: errror:      ACK_ERROR      = no acknowledgment from sensor
//                      CHECKSUM_ERROR = checksum mismatch
//                      NO_ERROR       = no error

//==============================================================================
etError SF05_ReadCommandResultWithTimeout(unsigned char senN,u8t maxRetries, u16t *result);
//==============================================================================
// Reads command results from sensor. If an error occurs, then the read will be
// repeated after a short wait (approx. 10ms).
//------------------------------------------------------------------------------
// input:  maxRetries    maximum number of retries
//         *result       pointer to an integer where the result will be stored  
//  
// return: errror:       ACK_ERROR      = no acknowledgment from sensor
//                       CHECKSUM_ERROR = checksum mismatch
//                       NO_ERROR       = no error
//
// remark: This function is usefull for reading measurement results. If not yet
//         a new valid measurement was performed, an acknowledge error occurs
//         and the read will be automatical repeated until a valid measurement
//         could be read.

//==============================================================================
etError SF05_GetFlow(unsigned char senN,ft offset, ft scale, ft *flow);
//==============================================================================
// Gets the flow from the sensor in a predefined unit. The "flow measurement"
// command will be automatical written to the sensor, if it is not already set.
//------------------------------------------------------------------------------
// input:  offset         offset flow
//         scale          scale factor flow
//         *flow          pointer to a floating point value, where the calculated
//                        flow will be stored
// 
// return: errror:        ACK_ERROR      = no acknowledgment from sensor
//                        CHECKSUM_ERROR = checksum mismatch
//                        NO_ERROR       = no error
//
// remark: The result will be converted according to the following formula:
//         flow in predefined unit = (measurement_result - offset) / scale

//==============================================================================
etError SF05_GetSerialNumber(unsigned char senN,u32t *serialNumber);
//==============================================================================
// Gets the serial number from the sensor.
//------------------------------------------------------------------------------
// input:  *serialNumber  pointer to a 32-bit integer, where the serial number
//                        will be stored
//
// return: error:         ACK_ERROR      = no acknowledgment from sensor
//                        CHECKSUM_ERROR = checksum mismatch
//                        NO_ERROR       = no error

//==============================================================================
etError SF05_SoftReset(unsigned char senN);
//==============================================================================
// Forces a sensor reset without switching the power off and on again.
//------------------------------------------------------------------------------
// return: error:         ACK_ERROR      = no acknowledgment from sensor
//                        NO_ERROR       = no error

//==============================================================================
etError SF05_CheckCrc(u8t data[], u8t nbrOfBytes, u8t checksum);
//==============================================================================
// Calculates checksum for n bytes of data and compares it with expected
// checksum.
//------------------------------------------------------------------------------
// input:  data[]         checksum is built based on this data
//         nbrOfBytes     checksum is built for n bytes of data
//         checksum       expected checksum
//  
// return: error:         CHECKSUM_ERROR = checksum does not match
//                        NO_ERROR       = checksum matches

#endif

etError SF05_ReadData(GPIO_TypeDef* GPIOSCL, uint16_t SCL_GPIO_Pin,GPIO_TypeDef* GPIOSDA, uint16_t SDA_GPIO_Pin,u8t *buf, u8t len);
etError SF05_WriteData(GPIO_TypeDef* GPIOSCL, uint16_t SCL_GPIO_Pin,GPIO_TypeDef* GPIOSDA, uint16_t SDA_GPIO_Pin,u8t *buf, u8t len);
etError SF05_GetFlow_0(unsigned char senN,u16t *result);
etError ADP800_GetDiffPress(unsigned char senN,ft scale, ft *diffPress, ft *tempature);
void DelayMicroSeconds(u32t nbrOfUs);
etError ADP810_ReadCommandResult(unsigned char senNo,u8t byteNum,u8t *result);
etError SF05_ReadDataID(GPIO_TypeDef* GPIOSCL, uint16_t SCL_GPIO_Pin,GPIO_TypeDef* GPIOSDA, uint16_t SDA_GPIO_Pin,u8t *buf, u8t len);
etError SF05_Write32byteData(GPIO_TypeDef* GPIOSCL, uint16_t SCL_GPIO_Pin,GPIO_TypeDef* GPIOSDA, uint16_t SDA_GPIO_Pin,float *buf, u8t len);
uint8_t SF05_WriteCommand_SM(unsigned char senNo,etCommands cmd);
etError SFM3000_GetDiffPress(unsigned char senN,ft scale, ft *diffPress, ft *tempature);
void GetAFM3000Temperature(unsigned char senN, ft *temperature);
uint32_t GetAFM3000ID(unsigned char senN);
int32_t GetAFM3000TempADC(unsigned char senN);
int32_t GetAFM3000FlowADC(unsigned char senN);
float GetAFM3000KNBN(unsigned char senN);	
int32_t Get_k(unsigned char senN);