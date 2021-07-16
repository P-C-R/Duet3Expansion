/*
 * ClosedLoop.cpp
 *
 *  Created on: 9 Jun 2020
 *      Author: David
 */

#include "ClosedLoop.h"

#if SUPPORT_CLOSED_LOOP

# include <math.h>
# include <Platform.h>
# include <CanMessageGenericParser.h>
# include "SpiEncoder.h"
# include "AS5047D.h"
# include "QuadratureEncoder.h"
# include "TLI5012B.h"
# include "AttinyProgrammer.h"
# include <TaskPriorities.h>
# include <CAN/CanInterface.h>
# include <CanMessageBuffer.h>
# include <CanMessageFormats.h>

# if SUPPORT_TMC2160
#  include "Movement/StepperDrivers/TMC51xx.h"
# else
#  error Cannot support closed loop with the specified hardware
# endif


// Control variables
// Variables that can be set by the user to determine how the closed loop controller works

static bool closedLoopEnabled = false;			// Has closed loop been enabled by the user?

static float Kp = 100;							// The proportional constant for the PID controller
static float Ki = 0;							// The proportional constant for the PID controller
static float Kd = 10;							// The proportional constant for the PID controller

static Encoder *encoder = nullptr;				// Pointer to the encoder object in use
static float encoderCountPerStep = 1;			// How many encoder readings do we get per step?

static bool collectingData = false;				// Are we currently collecting data? If so:
static uint16_t filterRequested = 0;			//	- What filter did they request?
static uint16_t samplesRequested = 0;			//	- How many samples did they request?
static ClosedLoop::RecordingMode modeRequested;	//	- What mode did they request?


// Working variables
// These variables are all used to calculate the required motor currents. They are declared here so they can be reported on by the data collection task

static int16_t rawEncoderReading;				// The raw reading taken from the encoder
static bool stepDirection = true;				// The direction the motor is attempting to take steps in
static float currentMotorSteps;					// The number of steps the motor has taken relative to it's zero position
static float targetMotorSteps = 0;				// The number of steps the motor should have taken relative to it's zero position
static int16_t lastError = 0;					// The error from the previous iteration

static float PIDPTerm;							// Proportional term
static float PIDITerm = 0;						// Integral term
static float PIDDTerm;							// Derivative term
static int16_t PIDControlSignal;				// The overall -255 to 255 signal from the PID controller

static int16_t phaseShift;						// The desired shift in the position of the motor
static uint16_t stepPhase;						// The current position of the motor
static uint16_t desiredStepPhase;				// The desired position of the motor

static int16_t coilA;							// The current to run through coil A
static int16_t coilB;							// The current to run through coil A


// Misc. variables

// Masks for each coil register
const uint32_t COIL_A_MASK = 0x000001FF;
const uint32_t COIL_B_MASK = 0x01FF0000;

// ATTiny programming vars
static SharedSpiDevice *encoderSpi = nullptr;
static AttinyProgrammer *programmer;

// Variable & masks to determine if we are tuning, and what type of tuning we are doing
// if tuning & TUNING_TYPE_MASK == true, then we are currently performing TUNING_TYPE
static uint8_t tuning = 0;
const uint8_t ZEROING_MASK = 1 << 0;
const uint8_t ENCODER_STEPS_MASK = 1 << 1;

// Tuning task - handles any pending tuning operations
constexpr size_t TuningTaskStackWords = 200;
static Task<TuningTaskStackWords> *tuningTask;

// Data collection task - handles sampling some of the static vars in this file
constexpr size_t DataCollectionTaskStackWords = 200;
static Task<DataCollectionTaskStackWords> *dataCollectionTask;


static void GenerateAttinyClock()
{
	// Currently we program the DPLL to generate 48MHz output, so to get 16MHz we divide by 3 and set the Improve Duty Cycle bit
	// We could instead program the DPLL to generate 96MHz and divide it by an extra factor of 2 in the other GCLKs
	// Or we could divide by 4 and be content with 12MHz.
	ConfigureGclk(ClockGenGclkNumber, GclkSource::dpll, 3, true);
	SetPinFunction(ClockGenPin, ClockGenPinPeriphMode);
}

// Helper function to set a given coil current in
void SetMotorCurrents(int32_t coilA, int32_t coilB)
{
# if SUPPORT_TMC2160 && SINGLE_DRIVER
	SmartDrivers::SetRegister(0,
			SmartDriverRegister::xDirect,
			((coilB << 16) & COIL_B_MASK) | (coilA & COIL_A_MASK));
# else
#  error Cannot support closed loop with the specified hardware
# endif
}

void ZeroStepper()
{
	// Enable (and keep enabled) the drive
	// TODO: We should modify Platform::EnableDrive such that if closed loop is enabled, it sets a var in here, then we can handle a disabled closed loop driver
# if SUPPORT_TMC2160 && SINGLE_DRIVER
	Platform::EnableDrive(0);
# else
#  error Undefined hardware
# endif

	switch (ClosedLoop::GetEncoderType().ToBaseType())
	{
	case EncoderType::rotaryQuadrature:
# if SUPPORT_TMC2160 && SINGLE_DRIVER
		// Force the motor to 0 phase by setting coilA = sin(0) and coilB = cos(0)
		SetMotorCurrents(0, 255);
# else
#  error Undefined hardware
# endif
		break;
	}

	tuning |= ZEROING_MASK;
}

void  ClosedLoop::EnableEncodersSpi() noexcept
{
# ifdef EXP1HCE
	SetPinFunction(EncoderMosiPin, EncoderMosiPinPeriphMode);
	SetPinFunction(EncoderSclkPin, EncoderSclkPinPeriphMode);
	SetPinFunction(EncoderMisoPin, EncoderMisoPinPeriphMode);
# else
#  error Undefined hardware
# endif
}

void  ClosedLoop::DisableEncodersSpi() noexcept
{
# ifdef EXP1HCE
	ClearPinFunction(EncoderMosiPin);
	ClearPinFunction(EncoderSclkPin);
	ClearPinFunction(EncoderMisoPin);
# else
#  error Undefined hardware
# endif
}

void  ClosedLoop::TurnAttinyOff() noexcept
{
	programmer->TurnAttinyOff();
}

extern "C" [[noreturn]] void TuningTaskLoop(void * param) noexcept {ClosedLoop::TuningLoop();}
extern "C" [[noreturn]] void DataCollectionTaskLoop(void * param) noexcept {ClosedLoop::DataCollectionLoop();}

void ClosedLoop::Init() noexcept
{
	// Init the ATTiny programmer
	pinMode(EncoderCsPin, OUTPUT_HIGH);													// make sure that any attached SPI encoder is not selected
	encoderSpi = new SharedSpiDevice(EncoderSspiSercomNumber, EncoderSspiDataInPad);	// create the encoders SPI device
	GenerateAttinyClock();
	programmer = new AttinyProgrammer(*encoderSpi);
	programmer->InitAttiny();

	// Set up the tuning task
	tuningTask = new Task<TuningTaskStackWords>;
	tuningTask->Create(TuningTaskLoop, "CLTune", nullptr, TaskPriority::ClosedLoop);

	// Set up the data collection task
	dataCollectionTask = new Task<DataCollectionTaskStackWords>;
	dataCollectionTask->Create(DataCollectionTaskLoop, "CLData", nullptr, TaskPriority::ClosedLoop);
}

bool ClosedLoop::GetClosedLoopEnabled() noexcept
{
	return closedLoopEnabled;
}

bool ClosedLoop::SetClosedLoopEnabled(bool enabled, const StringRef &reply) noexcept
{

	if (enabled) {
# if SUPPORT_SLOW_DRIVERS
		// TODO: Check what we need to do here
		if (Platform::IsSlowDriver()) {
			reply.copy("Closed loop drive mode not yet supported")
			return false;
		}
# endif
# if USE_TC_FOR_STEP || !SINGLE_DRIVER || !SUPPORT_TMC2160
		// TODO: Check what we need to do here
		reply.copy("Closed loop drive mode not yet supported")
		return false;
# endif
		if (encoder == nullptr)
		{
			reply.copy("No encoder specified for closed loop drive mode");
			return false;
		}
		ZeroStepper();
	}

	closedLoopEnabled = enabled;
	return true;
}

void ClosedLoop::SetStepDirection(bool dir) noexcept
{
	stepDirection = dir;
}

EncoderType ClosedLoop::GetEncoderType() noexcept
{
	return (encoder == nullptr) ? EncoderType::none : encoder->GetType();
}

// TODO: Instead of having this take step, why not use the current DDA to calculate where we need to be?
void ClosedLoop::TakeStep() noexcept
{
	targetMotorSteps += (stepDirection ? 0.0625 : -0.0625);
	return;

	// TODO: The below causes an error! Perhaps SmartDrivers::GetMicrostepping is returning something unexpected

	bool interpolation;	// TODO: Work out what this is for!
# if HAS_SMART_DRIVERS && SINGLE_DRIVER
	float microstepAngle = 1.0/SmartDrivers::GetMicrostepping(0, interpolation);
# else
#  error Undefined hardware
# endif
	targetMotorSteps += (stepDirection ? microstepAngle : -microstepAngle);
}

// TODO: This isn't currently called anywhere, but it's quite a useful utility. Do we want this in a GCODE command?
// TODO: If we are going to use this, definitely pull it into the tuning loop
GCodeResult ClosedLoop::FindEncoderCountPerStep(const CanMessageGeneric &msg, const StringRef &reply) noexcept
{
	// TODO: Check we are in closed loop mode
	tuning |= ENCODER_STEPS_MASK;

	// Set to 0
	SetMotorCurrents(Trigonometry::FastCos(512), Trigonometry::FastSin(512));
# if SUPPORT_TMC2160 && SINGLE_DRIVER
	while (SmartDrivers::UpdatePending(0)) { }
# else
#  error Cannot support closed loop with the specified hardware
# endif
	delayMicroseconds(100000);
	int32_t zeroReading = encoder->GetReading();

	// Set to 1024
	SetMotorCurrents(Trigonometry::FastCos(1536), Trigonometry::FastSin(1536));
# if SUPPORT_TMC2160 && SINGLE_DRIVER
	while (SmartDrivers::UpdatePending(0)) { }
# else
#  error Cannot support closed loop with the specified hardware
# endif
	delayMicroseconds(100000);
	int32_t quarterReading = encoder->GetReading();

	// Set to 2048
	SetMotorCurrents(Trigonometry::FastCos(2560), Trigonometry::FastSin(2560));
# if SUPPORT_TMC2160 && SINGLE_DRIVER
	while (SmartDrivers::UpdatePending(0)) { }
# else
#  error Cannot support closed loop with the specified hardware
# endif
	delayMicroseconds(100000);
	int32_t halfReading = encoder->GetReading();

	// Set to 3092
	SetMotorCurrents(Trigonometry::FastCos(3584), Trigonometry::FastSin(3584));
# if SUPPORT_TMC2160 && SINGLE_DRIVER
	while (SmartDrivers::UpdatePending(0)) { }
# else
#  error Cannot support closed loop with the specified hardware
# endif
	delayMicroseconds(100000);
	int32_t fullReading = encoder->GetReading();

	// Set back to 0
	SetMotorCurrents(Trigonometry::FastCos(512), Trigonometry::FastSin(512));
# if SUPPORT_TMC2160 && SINGLE_DRIVER
	while (SmartDrivers::UpdatePending(0)) { }
# else
#  error Cannot support closed loop with the specified hardware
# endif
	delayMicroseconds(100000);
	int32_t secondZeroReading = encoder->GetReading();

	tuning &= ~ENCODER_STEPS_MASK;

	reply.catf("\nreading: %d", (int) zeroReading);
	reply.catf("\nreading: %d", (int) quarterReading);
	reply.catf("\nreading: %d", (int) halfReading);
	reply.catf("\nreading: %d", (int) fullReading);
	reply.catf("\nreading: %d", (int) secondZeroReading);

	// Tell the user the encoder readings per step
	int32_t stepOne = quarterReading-zeroReading;
	reply.catf("\nStep 1: %d", (int) stepOne);
	int32_t stepTwo = halfReading-quarterReading;
	reply.catf("\nStep 2: %d", (int) stepTwo);
	int32_t stepThree = fullReading-halfReading;
	reply.catf("\nStep 3: %d", (int) stepThree);
	int32_t stepFour = secondZeroReading-fullReading;
	reply.catf("\nStep 4: %d", (int) stepFour);

	// Work out the average
	float avgStep = (secondZeroReading - zeroReading) / 4.0;
	reply.catf("\nAverage: %f", (double) avgStep);

	return GCodeResult::ok;
}

// TODO: THIS NEEDS TO BE REFACTORED A LITTLE NOW WE'RE USING M569 instead
GCodeResult ClosedLoop::ProcessM569Point1(const CanMessageGeneric &msg, const StringRef &reply) noexcept
{
	CanMessageGenericParser parser(msg, M569Point1Params);
	bool seen = false;
	uint8_t tempUint;
	float tempFloat;

	bool finalClosedLoopEnabled = closedLoopEnabled;	// The 'final' state the user intends closedLoopEnabled to be

	// Check closed loop enable/disable
	if (parser.GetUintParam('S', tempUint))
	{
		seen = true;

		switch(tempUint) {
		case 0:
			finalClosedLoopEnabled = false;
			break;
		case 1:
# if SUPPORT_SLOW_DRIVERS
			// TODO: Check what we need to do here
			if (Platform::IsSlowDriver()) {
				reply.copy("Closed loop mode not yet supported")
				return GCodeResult::error;
			}
# endif
# if USE_TC_FOR_STEP || !SINGLE_DRIVER
			// TODO: Check what we need to do here
			reply.copy("Closed loop mode not yet supported")
			return GCodeResult::error;
# else
			finalClosedLoopEnabled = true;
			break;
# endif
		}
	}

	// Check encoder type
	if (parser.GetUintParam('T', tempUint))
	{
		seen = true;
		if (tempUint < EncoderType::NumValues)
		{
			if (tempUint != GetEncoderType().ToBaseType())
			{
				// Check that the user is requesting a valid selection
				if (finalClosedLoopEnabled && tempUint == EncoderType::none)
				{
					reply.copy("Invalid encoder type for closed loop mode S1");
					return GCodeResult::error;
				}

				//TODO need to get a lock here in case there is any movement
				delete encoder;
				switch (tempUint)
				{
				case EncoderType::none:
				default:
					encoder = nullptr;
					break;

				case EncoderType::as5047:
					encoder = new AS5047D(*encoderSpi, EncoderCsPin);
					break;

				case EncoderType::tli5012:
					encoder = new TLI5012B(*encoderSpi, EncoderCsPin);
					break;

				case EncoderType::linearQuadrature:
					encoder = new QuadratureEncoder(true);
					break;

				case EncoderType::rotaryQuadrature:
					encoder = new QuadratureEncoder(false);
					break;
				}

				if (encoder != nullptr)
				{
					encoder->Enable();
				}
			}
		}
		else
		{
			reply.copy("Encoder type out of range");
			return GCodeResult::error;
		}
	}
	else
	{
		// If no encoder has been specified, and none is already selected
		// Then we cannot be in closed loop mode
		if (encoder == nullptr && finalClosedLoopEnabled)
		{
			reply.copy("Invalid encoder type for closed loop mode S1");
			return GCodeResult::error;
		}
	}

	// 'commit' the finalClosedLoopEnabled value now no errors have been thrown
	if (closedLoopEnabled != finalClosedLoopEnabled)
	{
		closedLoopEnabled = finalClosedLoopEnabled;
	}

	// Check encoder count per unit value
	if (parser.GetFloatParam('E', tempFloat))
	{
		encoderCountPerStep = tempFloat;
	}

	if (!seen)
	{
		reply.printf("Closed loop mode %s", (closedLoopEnabled) ? "enabled" : "disabled");
		reply.catf(", encoder type %s", GetEncoderType().ToString());
		reply.catf(", encoder steps per unit %f", (double) encoderCountPerStep);
	}

	return GCodeResult::ok;
}

void ClosedLoop::Diagnostics(const StringRef& reply) noexcept
{
	reply.printf("Encoder programmed status %s, encoder type %s, tuning: %#x", programmer->GetProgramStatus().ToString(), GetEncoderType().ToString(), tuning);
	if (encoder != nullptr)
	{
		reply.catf(", position %" PRIi32, encoder->GetReading());
		encoder->AppendDiagnostics(reply);
	}
	reply.catf(", collecting data: %s", collectingData ? "yes" : "no");
	if (collectingData)
	{
		reply.catf(" (filter: %#x, samples: %d, mode: %d)", filterRequested, samplesRequested, modeRequested);
	}

	//DEBUG
	//reply.catf(", event status 0x%08" PRIx32 ", TCC2 CTRLA 0x%08" PRIx32 ", TCC2 EVCTRL 0x%08" PRIx32, EVSYS->CHSTATUS.reg, QuadratureTcc->CTRLA.reg, QuadratureTcc->EVCTRL.reg);
}

void ClosedLoop::Spin() noexcept
{
	if (!closedLoopEnabled) {return;}

	if (tuning == 0) {
		ControlMotorCurrents();
	} else {
		tuningTask->Give();
	}
}

void ClosedLoop::ControlMotorCurrents() noexcept
{
	// Calculate the current position & phase from the encoder reading
	rawEncoderReading = encoder->GetReading();
	currentMotorSteps = rawEncoderReading / encoderCountPerStep;

	// Calculate the current error, if it's zero we don't need to do anything!
	int16_t currentError = (int16_t) (targetMotorSteps - currentMotorSteps);
	if (currentError == 0) {return;}

	// Use a PID controller to calculate the required 'torque' - the control signal
	PIDPTerm = ((int16_t) Kp) * currentError;
	PIDITerm += ((int16_t) Ki) * currentError;		// TODO: Is this causing an overflow?
	PIDDTerm = ((int16_t) Kd) * (lastError - currentError);
	float sumOfTerms = PIDPTerm + PIDITerm + PIDDTerm;
	PIDControlSignal = (int16_t) constrain<float>(sumOfTerms, -255, 255);	// Clamp between -255 and 255

	// TODO: This negative depends on if the motor is configured to run forward/backward
	PIDControlSignal = -PIDControlSignal;

	// Calculate the offset required to produce the torque in the correct direction
	// i.e. if we are moving in the positive direction, we must apply currents with a positive phase shift
	// The max abs value of phase shift we want is 25%.
	// Given that PIDControlSignal is -255 .. 255 and phase is 0 .. 4095
	// and that 25% of 4095 ~= 1024, our max phase shift ~= 4 * PIDControlSignal
	// DEBUG: Experimenting with microstepping by doing 0.1 *
	phaseShift = (4 * PIDControlSignal);

	// Calculate stepPhase - a 0-4095 value representing the phase *within* the current step
	float tmp = currentMotorSteps / 4;
	if (tmp >= 0) {
		stepPhase = (tmp - (int) tmp) * 4095;
	} else {
		stepPhase = (1 + tmp - (int) tmp) * 4095;
	}

	// Calculate the required motor currents to induce that torque
	// TODO: Have a minimum holding current
	// (If stepPhase < phaseShift, we need to add on an extra 4095 to bring us back within the correct range)
	desiredStepPhase = stepPhase + phaseShift + ((stepPhase < -phaseShift) * 4095);
	desiredStepPhase = desiredStepPhase % 4096;
	coilA = Trigonometry::FastSin(desiredStepPhase) * (abs(PIDControlSignal) / 255.0);
	// TODO: This negative depends on if the motor is configured to run forward/backward
	coilB = -Trigonometry::FastCos(desiredStepPhase) * (abs(PIDControlSignal) / 255.0);
	// TODO: Should we ::Give() to the task that's responsible for setting these registers here?

	// Assert the required motor currents
	SetMotorCurrents(coilA, coilB);

	// Update vars for the next cycle
	lastError = currentError;
}

[[noreturn]] void ClosedLoop::TuningLoop() noexcept
{

	while (true)
	{
		if (tuning & ZEROING_MASK)
		{
			// We are aware we are zeroing, reset the bit
			tuning &= ~ZEROING_MASK;

			// Wait for the register to have written
# if SUPPORT_TMC2160 && SINGLE_DRIVER
			while (SmartDrivers::UpdatePending(0)) {
				TaskBase::Take(10);
			}
# else
#  error Cannot support closed loop with the specified hardware
# endif

			// Wait for the motor to have finished moving
			TaskBase::Take(100);

			// Zero the encoder
			((QuadratureEncoder*) encoder)->SetReading(0);

			// Set the motor currents back to zero
			SetMotorCurrents(0, 0);

			// We are now done zero-ing
			tuning &= ~ZEROING_MASK;
		}

		TaskBase::Take();
	}
}

GCodeResult ClosedLoop::StartDataCollection(const CanMessageStartClosedLoopDataCollection& msg, const StringRef& reply) noexcept
{
	if (msg.deviceNumber != 0 || encoder == nullptr)
	{
		reply.copy("Drive is not in closed loop mode");
		return GCodeResult::error;
	}

	if (collectingData)
	{
		reply.copy("Drive is already collecting data");
		return GCodeResult::error;
	}

	// Set up the recording vars
	collectingData = true;
	filterRequested = msg.filter;
	samplesRequested = msg.numSamples;
	modeRequested = (RecordingMode) msg.mode;

	// Start the data collection task
	dataCollectionTask->Give();
	return GCodeResult::ok;
}

[[noreturn]] void ClosedLoop::DataCollectionLoop() noexcept
{
	while (true)
	{

		// If we are not collecting data, block the task
		if (!collectingData)
		{
			TaskBase::Take();
		}

		// If we are using RecordingMode::OnNextMove, wait for a move to start
		float startRecordingTrigger = targetMotorSteps;
		while (modeRequested == RecordingMode::OnNextMove && startRecordingTrigger == targetMotorSteps)
		{
			TaskBase::Take(10);
		}

		uint32_t lastWakeTime = xTaskGetTickCount();

		// Loop for each sample
		for (int i = 0; i < samplesRequested; i++)
		{

			{
				// Set up a CAN message
				CanMessageBuffer buf(nullptr);
				CanMessageClosedLoopData& msg = *(buf.SetupStatusMessage<CanMessageClosedLoopData>(CanInterface::GetCanAddress(), CanInterface::GetCurrentMasterAddress()));

				// Populate the control fields
				msg.numSamples = 1;
				msg.lastPacket = (i == samplesRequested - 1);
				msg.firstSampleNumber = i;
				msg.filter = filterRequested;

				// Populate the data fields
				// TODO: Pack more than one set of data into a message
				int dataPointer = 0;
				if (filterRequested & 1)  		{msg.data[dataPointer++] = rawEncoderReading;}
				if (filterRequested & 2)  		{msg.data[dataPointer++] = (int16_t) currentMotorSteps;}	//TODO: Pass back as some kind of float
				if (filterRequested & 4)  		{msg.data[dataPointer++] = (int16_t) targetMotorSteps;}		//TODO: Pass back as some kind of float
				if (filterRequested & 8)  		{msg.data[dataPointer++] = stepPhase;}
				if (filterRequested & 16)  		{msg.data[dataPointer++] = PIDControlSignal;}
				if (filterRequested & 32)  		{msg.data[dataPointer++] = (int16_t) PIDPTerm;}				//TODO: Pass back as some kind of float
				if (filterRequested & 64)  		{msg.data[dataPointer++] = (int16_t) PIDITerm;}				//TODO: Pass back as some kind of float
				if (filterRequested & 128)  	{msg.data[dataPointer++] = (int16_t) PIDDTerm;}				//TODO: Pass back as some kind of float
				if (filterRequested & 256)  	{msg.data[dataPointer++] = phaseShift;}
				if (filterRequested & 512)  	{msg.data[dataPointer++] = desiredStepPhase;}
				if (filterRequested & 1024) 	{msg.data[dataPointer++] = coilA;}
				if (filterRequested & 2048) 	{msg.data[dataPointer++] = coilB;}

				// Send the CAN message
				buf.dataLength = msg.GetActualDataLength();
				CanInterface::Send(&buf);
			}

			// Pause to maintain the sample rate (TODO: Implement variable sample rate)
			vTaskDelayUntil(&lastWakeTime, 25);
		}

		// Mark that we have finished collecting data
		collectingData = false;
	}
}

#endif

// End
