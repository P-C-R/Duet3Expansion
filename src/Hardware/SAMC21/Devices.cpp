/*
 * Devices.cpp
 *
 *  Created on: 28 Jul 2020
 *      Author: David
 */

#include <Hardware/Devices.h>
#include <TaskPriorities.h>

#if SAMC21

#include <AnalogIn.h>
#include <AnalogOut.h>

// Analog input support
constexpr size_t AnalogInTaskStackWords = 120;
static Task<AnalogInTaskStackWords> analogInTask;

# ifdef SAMMYC21

void SerialPortInit(AsyncSerial*) noexcept
{
	SetPinFunction(PortBPin(2), GpioPinFunction::D);		// TxD
}

void SerialPortDeinit(AsyncSerial*) noexcept
{
	pinMode(PortBPin(2), INPUT_PULLUP);
}

AsyncSerial uart0(5, 3, 512, 512, SerialPortInit, SerialPortDeinit);

extern "C" void SERCOM5_Handler()
{
	uart0.Interrupt();
}

#else

void SerialPortInit(AsyncSerial*) noexcept
{
	SetPinFunction(PortAPin(12), GpioPinFunction::D);		// TxD
}

void SerialPortDeinit(AsyncSerial*) noexcept
{
	pinMode(PortAPin(12), INPUT_PULLUP);
}

AsyncSerial uart0(4, 3, 512, 512, SerialPortInit, SerialPortDeinit);

extern "C" void SERCOM4_Handler()
{
	uart0.Interrupt();
}

#endif

void DeviceInit() noexcept
{
	AnalogIn::Init(DmacChanAdc0Rx, DmacPrioAdcRx);
	AnalogOut::Init();
	analogInTask.Create(AnalogIn::TaskLoop, "AIN", nullptr, TaskPriority::AinPriority);
}

#endif

// End
