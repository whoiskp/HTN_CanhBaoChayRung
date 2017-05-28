/*
 * main.c
 * @author: Khoa Pham
 * @email: kpwhois@gmail.com
 *
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "inc/hw_ints.h"
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "inc/hw_gpio.h"
#include "inc/hw_uart.h"
#include "driverlib/gpio.h"
#include "driverlib/pin_map.h"
#include "driverlib/sysctl.h"
#include "driverlib/timer.h"
#include "driverlib/uart.h"
#include "driverlib/interrupt.h"
#include "driverlib/eeprom.h"
#include "driverlib/adc.h"

// Create macro

#define DHT_PERIPH 			SYSCTL_PERIPH_GPIOD
#define DHT_PORT			GPIO_PORTD_BASE
#define DHT_PIN				GPIO_PIN_0

#define BLT_TX				PB1        	// transport
#define BLT_RX				PB0			// recieve

#define MY_UART_COM 		UART0_BASE 	// use for test with computer
#define MY_UART_BLT 		UART1_BASE 	// use for bluetooth

#define TEST_WITH_COM		1

// LED and LOA PE1
#define WARN_PERIPH			SYSCTL_PERIPH_GPIOE
#define WARN_PORT			GPIO_PORTE_BASE
#define WARN_PIN			GPIO_PIN_1

#define KNQ_MODE_HOUR 		1			// Che do ghi data theo thoi gian tinh theo hour
#define KNQ_MODE_TEMP 		2			// Che do ghi data theo muc chenh lech nhiet do
#define KNQ_VALUE_DEFAULT	5			// Gia tri default chenh lech (5 hour or 5oC) tuy vao Mode
#define KNQ_EEPROM_ADR_VLUE	0x08		// Adress start in EEPROM for save data

#define REQUEST_GETALL_DATA 600
#define REQUEST_SET_TIME	601
#define REQUEST_SET_MODE	602
#define REQUEST_GET_DATA	603

#define ERROR_NOT_FOUND		404
#define ERROR_RES_TIMEOUT	408
#define ERROR_CONFLICT		409 		// checksum != iTempDHT + iHumiDHT

#define STATUS_SUCCESS		200
#define STATUS_ACCEPTED		202

#define data GPIOPinRead(DHT_PORT,DHT_PIN)

// Global variable

uint32_t pui32Data_Clock[2]; // Chua 2 word dau tien mang thong tin clock trong EEPROM
uint32_t pui32Read_Clock[2]; // Chua thong thong tin Clock duoc doc ra tu EEPROM
uint32_t pui32Data_Save[3];				// Chua du lieu ghi vao EEPROM
uint32_t iCountAdrEEPROMTempHumi; // default = 0x8, set = multil 4 ; 0x0 - 0x07 : set time

int iKNQ_Status;						// status cho giao thuc ket noi "KNQ"
int iCountTime_ms;						// Bien dem thoi gian MCU theo ms
int iMode_Module, iMode_Value;// Cac che do cai dat cho module nay, va gia tri cua no.

unsigned int iTempDHT, iHumiDHT, iTempTivaC;
unsigned int iPreTempDHT, iPreHumiDHT, iPreTempTivaC;

char* sTimeMark = "000000";				// the hour:min:sec can be 000000

/**
 * iYears 	: 2 so cuoi cua nam. 20xx
 * iDays	: so ngay trong nam do (max 365);
 * iHours	: so gio hien tai
 * iMins	: so phut hien tai
 * iSec		: so giay hien tai
 */
int iYears, iDays, iHours, iMins, iSec;

// Var for ADC TivaC
uint32_t ui32ADC0Value[4];
uint32_t ui32TempAvg;

// Var for DHT
uint32_t testDirModeDHT; 	// this var for testing
uint32_t testValuePinDHT;	// this var for testing

unsigned int check, error;
uint8_t buffer[5] = { 0, 0, 0, 0, 0 };
uint8_t ii, i;

/**
 * Giao tiep voi UART
 */
static char resBuff[100];

// Function
static Reset(char *pbuff) {
	while (*pbuff != 0x00) {
		*pbuff = 0;
		pbuff++;
	}
}

static void UARTGetBuffer(uint32_t uart_base, char *pbuff) {
	char c;
	int n = 0;
	if (n == 0)
		Reset(pbuff);
	while (UARTCharsAvail(uart_base)) {
		c = UARTCharGetNonBlocking(uart_base);
		*(pbuff + n) = c;
		n++;
	}
}

void writeStringToUART(uint32_t uart_base, char* str) {
	int i;
	for (i = 0; i < strlen(str); i++)
		UARTCharPut(uart_base, str[i]);
}

void writeIntToUART(uint32_t uart_base, int iNum) {
	char cBuffer[10];
	int i = 0;
	if(iNum == 0) writeStringToUART(uart_base, "00");
	while (iNum - 1 >= 0) {
		cBuffer[i++] = iNum % 10 + 0x30;
		iNum /= 10;
	}
	for (i = i - 1; i >= 0; i--) {
		UARTCharPut(uart_base, cBuffer[i]);
	}
}

// it use to write data time mark to UART cz' number 0 can not write correct by function writeIntToUART
// it's as needed.
// This function write a number with mark, Default mark hour:min:sec is "000000"

void writeDataMarkToUART(uint32_t uart_base, int iNum, char* sMark){

	int iDigit = 0;
	int iIndexStr = strlen(sMark) - 1;

	/**
	 * check iIndexStr >= 0
	 * if can be
	 * 		while(iNum % 10 < 10 && iIndexStr >=0)
	 * 			to exit if out of sMark length :D
	 */

	while(iNum % 10 < 10){
		iDigit = iNum % 10;
		sMark[iIndexStr] += iDigit; // example: "0" + 9 = "9"; => "000009"
		iIndexStr--;
		iNum /= 10;					// get next Digit
	}

	// if it not change send "000000" as output :)) =>> this bug is resolve :D
	writeStringToUART(uart_base, sMark);
}

// ---------------------- End UART Communication ----------------------

// ---------------------- Controller ----------------------------------
// Trigger when the tempture DHT > 40oC or tempTivaC > 100oC
void warningFire() {
	// LEd Sang Chuong keu
	GPIOPinWrite(WARN_PORT, WARN_PIN, WARN_PIN);

	// Gui ve Bluetooth to, Do Am
	if (TEST_WITH_COM) {
		writeIntToUART(MY_UART_COM, iTempDHT);
		writeIntToUART(MY_UART_COM, iHumiDHT);
		writeIntToUART(MY_UART_COM, iTempTivaC);

		// 3 last char in respone => transport complete
		writeStringToUART(MY_UART_COM, "KNQ");
	} else {
		writeIntToUART(MY_UART_BLT, iTempDHT);
		writeIntToUART(MY_UART_BLT, iHumiDHT);
		writeIntToUART(MY_UART_BLT, iTempTivaC);

		// 3 last char in respone => transport complete
		writeStringToUART(MY_UART_BLT, "KNQ");
	}
}

// Add data to EEPROM
void addData() {
	// word 1 chua thong tin nam ngay
	pui32Data_Save[0] = iYears * 1000 + iDays;

	// word 2 chua thong tin gio phut giay
	pui32Data_Save[1] = iHours * 10000 + iMins * 100 + iSec;

	// word 3 chua thong tin nhiet do DHT 0 - 50, do am DHT 0 - 95, To cua TivaC 22.5 - 337.5
	// 3539022
	pui32Data_Save[2] = iTempDHT * 100000 + iHumiDHT * 1000 + iTempTivaC;

	// neu vuot qua EEPROM thi reset lai vi tri 0x08
	// EEPROM co 512 word 32bit
	if (iCountAdrEEPROMTempHumi == 512 * 4) {
		iCountAdrEEPROMTempHumi = KNQ_EEPROM_ADR_VLUE;
	}
	EEPROMProgram(pui32Data_Save, iCountAdrEEPROMTempHumi,
			sizeof(pui32Data_Save)); // write data to ROM

	iCountAdrEEPROMTempHumi += sizeof(pui32Data_Save);

	// Gan cac gia tri nhiet do, do am da luu thanh Pre go to getData to know why?
	iPreTempDHT = iTempDHT;
	iPreHumiDHT = iHumiDHT;
	iPreTempTivaC = iTempTivaC;
}

// Lay thoi gian trong EEPROM ra cac bien chay trong chuong trinh
void getTime() {
	EEPROMRead(pui32Read_Clock, 0x0, sizeof(pui32Read_Clock));

	if (TEST_WITH_COM) {
		writeStringToUART(MY_UART_COM, "\n Test time: ");
		writeIntToUART(MY_UART_COM, pui32Read_Clock[0]);

		writeStringToUART(MY_UART_COM, "\n Clock time: ");
		writeIntToUART(MY_UART_COM, pui32Read_Clock[1]);

	} else {
		writeStringToUART(MY_UART_BLT, "\n Test time: ");
		writeIntToUART(MY_UART_BLT, pui32Read_Clock[0]);

		writeStringToUART(MY_UART_BLT, "\n Clock time: ");
		writeIntToUART(MY_UART_BLT, pui32Read_Clock[1]);
	}

	// 19364
	iYears = pui32Read_Clock[0] / 1000;
	iDays = pui32Read_Clock[0] % 1000;

	// 235959
	iHours = pui32Read_Clock[1] / 10000;
	iSec = pui32Read_Clock[1] % 100;
	iMins = (pui32Read_Clock[1] - (10000 * iHours + iSec)) / 100;

	if (TEST_WITH_COM) {
		writeStringToUART(MY_UART_COM, "\n iYears: ");
		writeIntToUART(MY_UART_COM, iYears);

		writeStringToUART(MY_UART_COM, "\n iDays: ");
		writeIntToUART(MY_UART_COM, iDays);

		writeStringToUART(MY_UART_COM, "\n iHours: ");
		writeIntToUART(MY_UART_COM, iHours);

		writeStringToUART(MY_UART_COM, "\n iMins: ");
		writeIntToUART(MY_UART_COM, iMins);

		writeStringToUART(MY_UART_COM, "\n iSec: ");
		writeIntToUART(MY_UART_COM, iSec);

	} else {
		writeStringToUART(MY_UART_BLT, "\n iYears: ");
		writeIntToUART(MY_UART_BLT, iYears);

		writeStringToUART(MY_UART_BLT, "\n iDays: ");
		writeIntToUART(MY_UART_BLT, iDays);

		writeStringToUART(MY_UART_BLT, "\n iHours: ");
		writeIntToUART(MY_UART_BLT, iHours);

		writeStringToUART(MY_UART_BLT, "\n iMins: ");
		writeIntToUART(MY_UART_BLT, iMins);

		writeStringToUART(MY_UART_BLT, "\n iSec: ");
		writeIntToUART(MY_UART_BLT, iSec);

	}
}

// write time config to EEPROM
void configTime(uint32_t i32YearDay, uint32_t i32HourMinSec) {

	pui32Data_Clock[0] = i32YearDay; // 19364 = year: 2019 day: 364
	pui32Data_Clock[1] = i32HourMinSec; // 235959 = hour: 23 min 59 sec 59

	if (TEST_WITH_COM) {
		writeStringToUART(MY_UART_COM, "\n Set time: ");
	} else {
		writeStringToUART(MY_UART_BLT, "\n Set time: ");
	}

	EEPROMProgram(pui32Data_Clock, 0x0, sizeof(pui32Data_Clock)); // write data to ROM

	// get time data config to change Global variable;
	getTime();
}

void getTempFromADC() {
	// Xoa ngat ADC
	ADCIntClear(ADC0_BASE, 1);
	// Cho phep biet doi ADC kenh 0, chuoi lay mau 1
	ADCProcessorTrigger(ADC0_BASE, 1);
	// Cho trang thai ngat bien doi xong
	while (!ADCIntStatus(ADC0_BASE, 1, false)) {
	}
	// Lay ket qua tu ADC0 luu vao cho bien ui32ADC0Value
	ADCSequenceDataGet(ADC0_BASE, 1, ui32ADC0Value);
	// Doc ra tu mang va xu li
	ui32TempAvg = (ui32ADC0Value[0] + ui32ADC0Value[1] + ui32ADC0Value[2]
			+ ui32ADC0Value[3] + 2) / 4;
	iTempTivaC = (1475 - ((2475 * ui32TempAvg)) / 4096) / 10;
}

void getTempHumiFromDHT11(void) {

	if (TEST_WITH_COM) {
		writeStringToUART(MY_UART_COM, "\n Start DHT11...");
	} else {
		writeStringToUART(MY_UART_BLT, "\n Start DHT11...");
	}

	SysCtlDelay(SysCtlClockGet() / 3); // Delay 1s

	GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_1, GPIO_PIN_1); // Sang led Red

	GPIODirModeSet(DHT_PORT, DHT_PIN, GPIO_DIR_MODE_OUT);
	GPIOPinTypeGPIOOutput(DHT_PORT, DHT_PIN);

	GPIOPinWrite(DHT_PORT, DHT_PIN, DHT_PIN);
	testValuePinDHT = GPIOPinRead(DHT_PORT, DHT_PIN);
	SysCtlDelay(SysCtlClockGet() / 10000000);

	GPIOPinWrite(DHT_PORT, DHT_PIN, 0);
	testValuePinDHT = GPIOPinRead(DHT_PORT, DHT_PIN);
	SysCtlDelay(SysCtlClockGet() * 25 / 3000); // delay 25ms

	GPIOPinWrite(DHT_PORT, DHT_PIN, DHT_PIN);
	testValuePinDHT = GPIOPinRead(DHT_PORT, DHT_PIN);
//	SysCtlDelay(SysCtlClockGet() * 40 / 3000000); // delay 40us
	testValuePinDHT = GPIOPinRead(DHT_PORT, DHT_PIN);

	GPIODirModeSet(DHT_PORT, DHT_PIN, GPIO_DIR_MODE_IN);
	GPIOPinTypeGPIOInput(DHT_PORT, DHT_PIN);
	testValuePinDHT = GPIOPinRead(DHT_PORT, DHT_PIN);

	SysCtlDelay(SysCtlClockGet() * 60 / 3000000); // delay 60us

	testValuePinDHT = GPIOPinRead(DHT_PORT, DHT_PIN);

	if (testValuePinDHT != 0x0) {
		iKNQ_Status = ERROR_NOT_FOUND;
		return;
	} else {
//		writeStringToUART(MY_UART_BLT, "\n Giao tiep duoc");
		// tat led red, sang led green
		GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_1 | GPIO_PIN_2 | GPIO_INT_PIN_3,
		GPIO_PIN_2); // Sang led Green
	}

	while (!data)
		;											//Doi DaTa len 1
	SysCtlDelay(SysCtlClockGet() * 60 / 3000000);
	if (!data) {
		iKNQ_Status = ERROR_NOT_FOUND;
		return;
	} else
		while (data)
			;	 										//Doi Data ve 0
	for (i = 0; i < 5; i++) {
		for (ii = 0; ii < 8; ii++) {
			while ((!data))
				;											//Doi Data len 1
			SysCtlDelay(SysCtlClockGet() * 50 / 3000000);
			if (data) {
				buffer[i] |= (1 << (7 - ii));
				while ((data))
					;										//Doi Data xuong 0
			}
		}
	}

	iHumiDHT = buffer[0];
//	doamtp = buffer[1];
	iTempDHT = buffer[2];

//	nhietdotp = buffer[3];
	check = buffer[4]; //
	if (check != iHumiDHT + iTempDHT)
		iKNQ_Status = ERROR_CONFLICT;
	iKNQ_Status = STATUS_SUCCESS;

//	int x1, x2, x3, y1, y2, y3;
////	x1 = nhietdo / 100 + 0x30;
//	x2 = nhietdo % 100 / 10 + 0x30;
//	x3 = nhietdo % 10 + 0x30;
//
////	y1 = doam / 100 + 0x30;
//	y2 = doam % 100 / 10 + 0x30;
//	y3 = doam % 10 + 0x30;
//
//	writeStringToUART(MY_UART_BLT, "\n Temperature = ");
////	writeStringToUART(nhietdo);
////	UARTCharPut(MY_UART_BASE, x1);
////	UARTCharPut(MY_UART_BLT, x2);
////	UARTCharPut(MY_UART_BLT, x3);
////	writeStringToUART(MY_UART_COM, "oC");
//	UARTCharPut(MY_UART_BLT, x2);
//	UARTCharPut(MY_UART_BLT, x3);
//	writeStringToUART(MY_UART_BLT, "oC");
//
//	writeStringToUART(MY_UART_BLT, "\n Humidity= ");
////	UARTCharPut(MY_UART_COM, y1);
////	UARTCharPut(MY_UART_COM, y2);
////	UARTCharPut(MY_UART_COM, y3);
////	UARTCharPut(MY_UART_COM, '%');
//	UARTCharPut(MY_UART_BLT, y2);
//	UARTCharPut(MY_UART_BLT, y3);
//	UARTCharPut(MY_UART_BLT, '%');

}

/**
 * Lay thong tin cho DHT11 va To TivaC
 */
void getData() {

	getTempHumiFromDHT11();
	getTempFromADC();

	//	Handler Mode
	// check fire
	if (iTempDHT > 40 || iTempTivaC > 100) {
		warningFire();
	} else {
		// Tat led and den
		GPIOPinWrite(WARN_PORT, WARN_PIN, 0);
	}

	// Che do ghi data theo chenh lech temp
	if (iMode_Module == KNQ_MODE_TEMP) {
		if ((abs(iTempDHT - iPreTempDHT) == iMode_Value)
				|| (abs(iTempTivaC - iPreTempTivaC) == iMode_Value)) {
			addData();
		}
	}
	// Che do ghi data theo gio
	if ((iMode_Module == KNQ_MODE_HOUR) && (iHours % iMode_Value == 0)) {
		addData();
	}

	// test for com
	addData();
}

// Midleware xử lý resquest from client - Android (resquest in buff[100]);
// Exp: GetAllData : 					600
// 		SetMode save data by temp: 		60225 			-> mode 2 value = 5
//		SetTime 22/05/2017 17:36:40 : 	60117141173640	-> year: 17, date: 141, hour: 17, mins: 36, sec: 40
//		Get a Data Current:				603

// code: 600 - send all data to Client
void sendAllData() {

	// doc data tu EEPROM 0x08 den vi tri luu hien tai
	uint32_t i32ReadData_tmp[3];

	uint32_t i32CountAds = KNQ_EEPROM_ADR_VLUE;

	for (; i32CountAds <= iCountAdrEEPROMTempHumi; i32CountAds +=
			sizeof(i32ReadData_tmp)) {
		EEPROMRead(i32ReadData_tmp, i32CountAds, sizeof(i32ReadData_tmp));

		if (TEST_WITH_COM) {

			// Gui qua com de test
			writeIntToUART(MY_UART_COM, i32ReadData_tmp[0]);

			// send time hour:min:sec = 000000
			writeDataMarkToUART(MY_UART_COM, i32ReadData_tmp[1], sTimeMark);

			writeIntToUART(MY_UART_COM, i32ReadData_tmp[2]);

		}
	}

	// Cau hinh lai vi tri ghi du lieu la 0x08 <=> Xoa phan trong EEPROM chua data, ghi lai cac gia tri moi
	iCountAdrEEPROMTempHumi = KNQ_EEPROM_ADR_VLUE;
	iKNQ_Status = STATUS_SUCCESS;

	if (TEST_WITH_COM) {
		// Send request reply
		// 3 last char in respone => transport complete
		writeStringToUART(MY_UART_COM, "KNQ");

		// Gui Status code to Client;
		writeIntToUART(MY_UART_COM, iKNQ_Status);
	} else {
		// Send request reply
		// 3 last char in respone => transport complete
		writeStringToUART(MY_UART_BLT, "KNQ");

		// Gui Status code to Client;
		writeIntToUART(MY_UART_BLT, iKNQ_Status);

	}

}

// code: 601- update time in sever - res: 60119364235959
void setTime() {
	// 19364 = year: 2019 day: 364
	pui32Data_Clock[0] = resBuff[3] * 10000 + resBuff[4] * 1000
			+ resBuff[5] * 100 + resBuff[6] * 10 + resBuff[7];

	// 235959 = hour: 23 min 59 sec 59
	pui32Data_Clock[1] = resBuff[8] * 100000 + resBuff[9] * 10000
			+ resBuff[10] * 1000 + resBuff[11] * 100 + resBuff[12] * 10
			+ resBuff[13];

	writeStringToUART(MY_UART_COM, "\n Set time: ");

	EEPROMProgram(pui32Data_Clock, 0x0, sizeof(pui32Data_Clock)); // write data to ROM

	iKNQ_Status = STATUS_SUCCESS;
	if (TEST_WITH_COM) {
		// Send request reply
		// 3 last char in respone => transport complete
		writeStringToUART(MY_UART_COM, "KNQ");

		// Gui Status code to Client;
		writeIntToUART(MY_UART_COM, iKNQ_Status);
	} else {
		// Send request reply
		// 3 last char in respone => transport complete
		writeStringToUART(MY_UART_BLT, "KNQ");

		// Gui Status code to Client;
		iKNQ_Status = STATUS_SUCCESS;
		writeIntToUART(MY_UART_BLT, iKNQ_Status);
	}

	// get time data after to change Global variable;
	getTime();
}

// code: 602 - update mode in server
void setMode() {
	iMode_Module = resBuff[3];
	iMode_Value = resBuff[4];

	// Send request reply
	// 3 last char in respone => transport complete
	writeStringToUART(MY_UART_BLT, "KNQ");

	// Gui Status code to Client;
	iKNQ_Status = STATUS_SUCCESS;
	writeIntToUART(MY_UART_BLT, iKNQ_Status);
}

// code: 603 - Send only a data to Client
void sendAData() {
	if (TEST_WITH_COM) {
		writeStringToUART(MY_UART_COM, "\n Tmp DHT: ");
		writeIntToUART(MY_UART_COM, iPreTempDHT);
		writeStringToUART(MY_UART_COM, "\n Humi DHT: ");
		writeIntToUART(MY_UART_COM, iPreHumiDHT);
		writeStringToUART(MY_UART_COM, "\n Tmp TivaC: ");
		writeIntToUART(MY_UART_COM, iPreTempTivaC);

//		// Send request reply
//		// 3 last char in respone => transport complete
//		writeStringToUART(MY_UART_COM, "KNQ");
//
//		// Gui Status code to Client;
//		// status nay phu thuoc vao viec doc data tu DHT11
//		// Co the la NOT_FOUND DHT or CONFLICT voi checksum
//		writeIntToUART(MY_UART_COM, iKNQ_Status);

	} else {
		writeIntToUART(MY_UART_BLT, iPreTempDHT);
		writeIntToUART(MY_UART_BLT, iPreHumiDHT);
		writeIntToUART(MY_UART_BLT, iPreTempTivaC);

		// Send request reply
		// 3 last char in respone => transport complete
		writeStringToUART(MY_UART_BLT, "KNQ");

		// Gui Status code to Client;
		// status nay phu thuoc vao viec doc data tu DHT11
		// Co the la NOT_FOUND DHT or CONFLICT voi checksum
		writeIntToUART(MY_UART_BLT, iKNQ_Status);
	}

}

void midlewareHandleRes() {
	int iResquest = 0;
	// covert char recieve in resquest to integer;
	iResquest = (resBuff[0] - 0x30) * 100 + (resBuff[1] - 0x30) * 10
			+ (resBuff[2] - 0x30);
	if (iResquest == REQUEST_GETALL_DATA) {
		sendAllData();
		return;
	}
	if (iResquest == REQUEST_SET_MODE) {
		setMode();
		return;
	}
	if (iResquest == REQUEST_SET_TIME) {
		setTime();
		return;
	}
	if (iResquest == REQUEST_GET_DATA) {
		sendAData();
	}
}

// Handler interrupt UART

void UARTIntHandler(void) {
	UARTIntClear(UART1_BASE, UARTIntStatus(UART1_BASE, true)); //clear the asserted interrupts

	// read request from client and save it into resBuff
	UARTGetBuffer(MY_UART_BLT, &resBuff[0]);

	// xu li request
	midlewareHandleRes();
}

// Handler interrupt Timer - Interrupt every 100ms
void Timer0IntHandler(void) {
	// Clear the timer interrupt
	TimerIntClear(TIMER0_BASE, TIMER_TIMA_TIMEOUT);

//	if (TEST_WITH_COM){
//		writeStringToUART(MY_UART_COM, "\n Time: ");
//		writeIntToUART(MY_UART_COM, iCountTime_ms);
//	}

//	// tinh gia tri dong ho
//	iCountTime_ms++;
//
//	if (iCountTime_ms < 10) {
//		return;
//	}
//
//	// == 10
//	iCountTime_ms = 0;
//	iSec++;
//
//	if (iSec <= 59) {
//		return;
//	}
//
//	iSec = 0;
//	iMins++;
//
//	if (iMins <= 59) {
//		return;
//	}
//
//	iMins = 0;
//	iHours++;
//	if (iHours <= 23) {
//		return;
//	}
//
//	iHours = 0;
//	iDays++;
//	if (iDays <= 365) {
//		return;
//	}
//
//	iDays = 0;
//	iYears++;
//
//	if (iYears == 270995) { // DoB author :v
//		if (TEST_WITH_COM) {
//			writeStringToUART(MY_UART_COM,
//					"\n MAX YEAR :D Khoa Pham Dep Trai. \n Call my team to update code!");
//		}
//	}

	if (iCountTime_ms++ < 10)
		return;
	iCountTime_ms = 0;
	if (iSec++ < 59)
		return;
	iSec = 0;
	if (iMins++ < 59)
		return;
	iMins = 0;
	if (iHours++ < 23)
		return;
	iHours = 0;
	if (iDays++ < 364)
		return;
	iDays = 0;
	if (iYears++ >= 270995) { // DoB author :v
		if (TEST_WITH_COM) {
			writeStringToUART(MY_UART_COM,
					"\n MAX YEAR :D Khoa Pham Dep Trai. \n Call my team to update code!");
		}
	}
}

// -------------------------- End Interrupt -----------------------------------------------------

// ----------- Config ------------------//
void Config() {
	// SysClock
	SysCtlClockSet(
	SYSCTL_SYSDIV_5 | SYSCTL_USE_PLL | SYSCTL_OSC_MAIN | SYSCTL_XTAL_16MHZ);

	// config DHT11
	SysCtlPeripheralEnable(DHT_PERIPH);

	// Cau hinh led test
	SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOF);
	GPIOPinTypeGPIOOutput(GPIO_PORTF_BASE,
	GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3);

	// config UART TEST for computer
	SysCtlPeripheralEnable(SYSCTL_PERIPH_UART0);
	SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOA);
	GPIOPinConfigure(GPIO_PA0_U0RX);
	GPIOPinConfigure(GPIO_PA1_U0TX);
	GPIOPinTypeUART(GPIO_PORTA_BASE, GPIO_PIN_0 | GPIO_PIN_1);

	UARTConfigSetExpClk(UART0_BASE, SysCtlClockGet(), 115200,
			(UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE | UART_CONFIG_PAR_NONE));
	// end config UART

	// Config UART Bluetooth HC 05
	SysCtlPeripheralEnable(SYSCTL_PERIPH_UART1);
	SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOB);
	GPIOPinConfigure(GPIO_PB0_U1RX); // chọn chân PB0 là chân RX của UART1
	GPIOPinConfigure(GPIO_PB1_U1TX); // chọn chân PB1 là chân TX của UART1
	GPIOPinTypeUART(GPIO_PORTB_BASE, GPIO_PIN_0 | GPIO_PIN_1); // ENABLE chân portb để điều khiển uart

	UARTConfigSetExpClk(UART1_BASE, SysCtlClockGet(), 9600,
			(UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE | UART_CONFIG_PAR_NONE));

	IntMasterEnable(); //enable processor interrupts
	IntEnable(INT_UART1); //enable the UART interrupt
	UARTIntEnable(UART1_BASE, UART_INT_RX | UART_INT_RT); //only enable RX and TX interrupts

	// Config ADC TivaC
	SysCtlPeripheralEnable(SYSCTL_PERIPH_ADC0);
	// Chon ADC0, su dung kieu lay mau kieu 1 (4 mau), trigger lay mau tu bo vi xu ly, do uu tien 0
	ADCSequenceConfigure(ADC0_BASE, 1, ADC_TRIGGER_PROCESSOR, 0);
	// Cau hinh lay mau thu 0, theo chuoi lay mau 1 tai kenh cam bien nhiet do (CTL_TS)
	ADCSequenceStepConfigure(ADC0_BASE, 1, 0, ADC_CTL_TS);
	ADCSequenceStepConfigure(ADC0_BASE, 1, 1, ADC_CTL_TS);
	ADCSequenceStepConfigure(ADC0_BASE, 1, 2, ADC_CTL_TS);
	// Cho phep ngat ADC, ket thuc viec lay mau ADC
	ADCSequenceStepConfigure(ADC0_BASE, 1, 3,
	ADC_CTL_TS | ADC_CTL_IE | ADC_CTL_END);
	// Cho phep lay mau kieu 1 cho ADC0
	ADCSequenceEnable(ADC0_BASE, 1);

	// EEPROM
	SysCtlPeripheralEnable(SYSCTL_PERIPH_EEPROM0);
	EEPROMInit();		// Khoi tao EEPROM
	EEPROMMassErase();  // Xoa EEPROM da co

	// Loa and Led
	SysCtlPeripheralEnable(WARN_PERIPH);
	GPIODirModeSet(WARN_PORT, WARN_PIN, GPIO_DIR_MODE_OUT);
	GPIOPinTypeGPIOOutput(WARN_PORT, WARN_PIN);

	//timer 0A
	uint32_t ui32Period;
	SysCtlPeripheralEnable(SYSCTL_PERIPH_TIMER0);
	TimerConfigure(TIMER0_BASE, TIMER_CFG_PERIODIC);
	ui32Period = SysCtlClockGet() / 10;       // interrupt every 100ms
	TimerLoadSet(TIMER0_BASE, TIMER_A, ui32Period - 1);
	IntEnable(INT_TIMER0A);
	TimerIntEnable(TIMER0_BASE, TIMER_TIMA_TIMEOUT);
	TimerEnable(TIMER0_BASE, TIMER_A);

	// init default variable
	iMode_Module = KNQ_MODE_HOUR; 		// default la do theo thoi gian;
	iMode_Value = KNQ_VALUE_DEFAULT;	// default value for mode;
	iCountAdrEEPROMTempHumi = KNQ_EEPROM_ADR_VLUE;// default = 0x8, set = multil 4 ; 0x0 - 0x07 : set time;

	iKNQ_Status = STATUS_ACCEPTED;		// set status reply is Accepted
	iCountTime_ms = 0;					// bo dem gio bat dau tu 0ms;

	iTempDHT = iHumiDHT = iTempTivaC = iPreTempDHT = iPreHumiDHT =
			iPreTempTivaC = 0;

	/**
	 * iYears 	: 2 so cuoi cua nam. 20xx
	 * iDays	: so ngay trong nam do (max 365);
	 * iHours	: so gio hien tai
	 * iMins	: so phut hien tai
	 * iSec		: so giay hien tai
	 *
	 * Time default: 01/01/2017 00:00:00
	 */

	iYears = 17;
	iDays = 1;
	iHours = 0;
	iMins = 0;
	iSec = 0;
	// set time default and write it to 0x0 - 0x07 EEPROM
	configTime(17001, 235959);
}
// ----------- End Config --------------//

int main(void) {
	Config();
	while (1) {
		getData();
		writeStringToUART(MY_UART_COM, "\n Send A Data: ");
		sendAData();
		writeStringToUART(MY_UART_COM, "\n Send All Data: ");
		sendAllData();
		writeStringToUART(MY_UART_COM, "\n Test int = 0: ");
		writeIntToUART(MY_UART_COM, 0);
		if (TEST_WITH_COM) {
			writeStringToUART(MY_UART_COM, "\n iYears: ");
			writeIntToUART(MY_UART_COM, iYears);

			writeStringToUART(MY_UART_COM, "\n iDays: ");
			writeIntToUART(MY_UART_COM, iDays);

			writeStringToUART(MY_UART_COM, "\n iHours: ");
			writeIntToUART(MY_UART_COM, iHours);

			writeStringToUART(MY_UART_COM, "\n iMins: ");
			writeIntToUART(MY_UART_COM, iMins);

			writeStringToUART(MY_UART_COM, "\n iSec: ");
			writeIntToUART(MY_UART_COM, iSec);

		} else {
			writeStringToUART(MY_UART_BLT, "\n iYears: ");
			writeIntToUART(MY_UART_BLT, iYears);

			writeStringToUART(MY_UART_BLT, "\n iDays: ");
			writeIntToUART(MY_UART_BLT, iDays);

			writeStringToUART(MY_UART_BLT, "\n iHours: ");
			writeIntToUART(MY_UART_BLT, iHours);

			writeStringToUART(MY_UART_BLT, "\n iMins: ");
			writeIntToUART(MY_UART_BLT, iMins);

			writeStringToUART(MY_UART_BLT, "\n iSec: ");
			writeIntToUART(MY_UART_BLT, iSec);

		}
		SysCtlDelay(SysCtlClockGet());

	}
}
