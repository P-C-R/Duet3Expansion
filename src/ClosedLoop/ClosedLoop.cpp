/*
 * ClosedLoop.cpp
 *
 *  Created on: 9 Jun 2020
 *      Author: David
 */

#include "ClosedLoop.h"

#if SUPPORT_CLOSED_LOOP

#include "AS5047D.h"
#include "TLI5012B.h"
#include "SpiEncoder.h"

# include <math.h>
# include <Platform.h>
# include <TaskPriorities.h>
# include <CAN/CanInterface.h>
# include <CanMessageBuffer.h>
# include <CanMessageFormats.h>
# include <CanMessageGenericParser.h>

# if defined(EXP1HCE)
#  include "AttinyProgrammer.h"
#  include "QuadratureEncoderAttiny.h"
#elif defined(EXP1HCL)
#  include "QuadratureEncoderPdec.h"
#else
#  error Cannot support closed loop with the specified hardware
# endif

# if SUPPORT_CAN_LOGGING
#  include "Logger.h"
# endif

# if SUPPORT_TMC2160
#  include "Movement/StepperDrivers/TMC51xx.h"
# else
#  error Cannot support closed loop with the specified hardware
# endif

# ifdef EXP1HCL
#  define CLOSED_LOOP_DATA_BUFFER_SIZE 2000		//  (1000 readings of 12 variables)
# else
#  define CLOSED_LOOP_DATA_BUFFER_SIZE 50		//	(50 readings of 12 variables)
# endif

// Control variables
// Variables that can be set by the user to determine how the closed loop controller works

static bool closedLoopEnabled = false;			// Has closed loop been enabled by the user?
static uint8_t tuningError;						// Flags for any tuning errors

static bool coilAPolarity = true;				// True = +ve, False = -ve
static bool coilBPolarity = false;				// True = +ve, False = -ve

static float holdCurrent = 0;					// The minimum holding current when stationary

static float Kp = 100;							// The proportional constant for the PID controller
static float Ki = 0.01;							// The proportional constant for the PID controller
static float Kd = 10;							// The proportional constant for the PID controller

static float errorThresholds[2];				// The error thresholds. [0] is pre-stall, [1] is stall

static float ultimateGain = 0;					// The ultimate gain of the controller (used for tuning)
static float oscillationPeriod = 0;				// The oscillation period when Kp = ultimate gain

static Encoder *encoder = nullptr;				// Pointer to the encoder object in use
static float encoderCountPerStep;				// How many encoder readings do we get per step?

static bool collectingData = false;				// Are we currently collecting data? If so:
static uint16_t rateRequested;					//	- What sample rate did they request?
static uint16_t filterRequested;				//	- What filter did they request?
static uint16_t samplesRequested;				//	- How many samples did they request?
static ClosedLoop::RecordingMode modeRequested;	//	- What mode did they request?
static uint8_t movementRequested;				//	- Which calibration movement did they request? 0=none, 1=polarity, 2=continuous
static float sampleBuffer[CLOSED_LOOP_DATA_BUFFER_SIZE * 12];	//	- Store the samples here (max. CLOSED_LOOP_DATA_BUFFER_SIZE samples of 12 variables)
static uint16_t sampleBufferReadPointer = 0;	//  - Send this sample next to the mainboard
static uint16_t sampleBufferWritePointer = 0;	//  - Store the next sample at this point in the buffer

# if SUPPORT_CAN_LOGGING
static uint32_t backOffTime = 100;				// TODO: ms before a closed loop error should be reported again
# endif


// Working variables
// These variables are all used to calculate the required motor currents. They are declared here so they can be reported on by the data collection task

static int16_t rawEncoderReading;				// The raw reading taken from the encoder
static bool stepDirection = true;				// The direction the motor is attempting to take steps in
static float currentMotorSteps;					// The number of steps the motor has taken relative to it's zero position
static float targetMotorSteps = 0;				// The number of steps the motor should have taken relative to it's zero position
static float currentError;						// The current error
static float lastError = 0;						// The error from the previous iteration

static float PIDPTerm;							// Proportional term
static float PIDITerm = 0;						// Integral term
static float PIDDTerm;							// Derivative term
static int16_t PIDControlSignal;				// The overall -255 to 255 signal from the PID controller

static int16_t phaseShift;						// The desired shift in the position of the motor
static uint16_t stepPhase;						// The current position of the motor
static uint16_t desiredStepPhase = 0;			// The desired position of the motor

static int16_t coilA;							// The current to run through coil A
static int16_t coilB;							// The current to run through coil A

// Misc. variables

// Logging vars
# if SUPPORT_CAN_LOGGING
static float maxError = 0;
static float ewmaError = 0;
# endif

// Masks for each coil register
const uint32_t COIL_A_MASK = 0x000001FF;
const uint32_t COIL_B_MASK = 0x01FF0000;

// Bitmask of any tuning manoeuvres that have been requested
static uint8_t tuning = 0;

// Tuning task - handles any pending tuning operations
constexpr size_t TuningTaskStackWords = 200;
static Task<TuningTaskStackWords> *tuningTask;

// Data collection task - handles sampling some of the static vars in this file
constexpr size_t DataCollectionTaskStackWords = 200;
static Task<DataCollectionTaskStackWords> *dataCollectionTask;

// Data transmission task - handles sending back the buffered sample data
constexpr size_t DataTransmissionTaskStackWords = 200;
static Task<DataTransmissionTaskStackWords> *dataTransmissionTask;

#ifdef EXP1HCE
static SharedSpiDevice *encoderSpi = nullptr;

static void GenerateAttinyClock()
{
	// Currently we program the DPLL to generate 48MHz output, so to get 16MHz we divide by 3 and set the Improve Duty Cycle bit
	// We could instead program the DPLL to generate 96MHz and divide it by an extra factor of 2 in the other GCLKs
	// Or we could divide by 4 and be content with 12MHz.
	ConfigureGclk(ClockGenGclkNumber, GclkSource::dpll, 3, true);
	SetPinFunction(ClockGenPin, ClockGenPinPeriphMode);
}

void  ClosedLoop::TurnAttinyOff() noexcept
{
	programmer->TurnAttinyOff();
}

#endif

// TODO: Helper function to convert between the internal representation of encoderCountPerStep, and the appropriate external representation (e.g. CPR)
#if false
float countPerStepToExternalUnits() {
	// TODO
}

float externalUnitsToCountPerStep() {
	// TODO
}
#endif

void ReportTuningErrors(uint8_t tuningErrorBitmask, const StringRef &reply)
{
	if (tuningErrorBitmask & ClosedLoop::TUNE_ERR_NOT_ZEROED) 					{reply.catf(" The drive has not been zeroed.");}
	if (tuningErrorBitmask & ClosedLoop::TUNE_ERR_NOT_CHECKED_POLARITY) 		{reply.catf(" The drive has not had it's polarity checked.");}
	if (tuningErrorBitmask & ClosedLoop::TUNE_ERR_NOT_CHECKED_CONTROL) 			{reply.catf(" The drive has not had it's control checked.");}
	if (tuningErrorBitmask & ClosedLoop::TUNE_ERR_NOT_CHECKED_ENCODER_STEPS) 	{reply.catf(" The encoder has not had it's count per revolution checked.");}
	if (tuningErrorBitmask & ClosedLoop::TUNE_ERR_INCORRECT_POLARITY) 			{reply.catf(" The drive has been found to have an incorrect polarity.");}
	if (tuningErrorBitmask & ClosedLoop::TUNE_ERR_CONTROL_FAILED) 				{reply.catf(" The drive has been found to be uncontrollable.");}
}

// Helper function to set the motor to a given phase and magnitude
void SetMotorPhase(uint16_t phase, float magnitude)
{
	magnitude = constrain<float>(magnitude, holdCurrent, 1.0);
	coilA = 255 * (coilAPolarity ? magnitude : -magnitude ) * Trigonometry::FastCos(phase);
	coilB = 255 * (coilBPolarity ? magnitude : -magnitude ) * Trigonometry::FastSin(phase);
	// TODO: Should we ::Give() to the task that's responsible for setting these registers here?

# if SUPPORT_TMC2160 && SINGLE_DRIVER
	SmartDrivers::SetRegister(0,
			SmartDriverRegister::xDirect,
			((coilB << 16) & COIL_B_MASK) | (coilA & COIL_A_MASK));
# else
#  error Cannot support closed loop with the specified hardware
# endif
}

#ifdef EXP1HCL

static void GenerateTmcClock()
{
	// Currently we program DPLL0 to generate 120MHz output, so to get 15MHz we divide by 8
	ConfigureGclk(ClockGenGclkNumber, GclkSource::dpll0, 8, true);
	SetPinFunction(ClockGenPin, ClockGenPinPeriphMode);
}

#endif

#ifdef EXP1HCE

void  ClosedLoop::EnableEncodersSpi() noexcept
{
	SetPinFunction(EncoderMosiPin, EncoderMosiPinPeriphMode);
	SetPinFunction(EncoderSclkPin, EncoderSclkPinPeriphMode);
	SetPinFunction(EncoderMisoPin, EncoderMisoPinPeriphMode);
}

void  ClosedLoop::DisableEncodersSpi() noexcept
{
	ClearPinFunction(EncoderMosiPin);
	ClearPinFunction(EncoderSclkPin);
	ClearPinFunction(EncoderMisoPin);
}

#endif

extern "C" [[noreturn]] void TuningTaskLoop(void *param) noexcept
{
	ClosedLoop::TuningLoop();
}
extern "C" [[noreturn]] void DataCollectionTaskLoop(void *param) noexcept { ClosedLoop::DataCollectionLoop(); }
extern "C" [[noreturn]] void DataTransmissionTaskLoop(void *param) noexcept { ClosedLoop::DataTransmissionLoop(); }

void ClosedLoop::Init() noexcept
{
	// Init the ATTiny programmer
	pinMode(EncoderCsPin, OUTPUT_HIGH);													// make sure that any attached SPI encoder is not selected

#if defined(EXP1HCE)
	encoderSpi = new SharedSpiDevice(EncoderSspiSercomNumber, EncoderSspiDataInPad);	// create the encoders SPI device
	GenerateAttinyClock();
	programmer = new AttinyProgrammer(*encoderSpi);
	programmer->InitAttiny();
#elif defined(EXP1HCL)
	// The EXP1HCL board uses the standard shared SPI device
	GenerateTmcClock();
#endif

	// Init that we have not been tuned
	tuningError = TUNE_ERR_NOT_PERFORMED_MINIMAL_TUNE;

	// Initialise to no error thresholds
	errorThresholds[0] = 0;
	errorThresholds[1] = 0;

	// Set up the tuning task
	tuningTask = new Task<TuningTaskStackWords>;
	tuningTask->Create(TuningTaskLoop, "CLTune", nullptr, TaskPriority::ClosedLoop);

	// Set up the data collection task
	dataCollectionTask = new Task<DataCollectionTaskStackWords>;
	dataCollectionTask->Create(DataCollectionTaskLoop, "CLData", nullptr, TaskPriority::ClosedLoop);

	// Set up the data transmission task
	dataTransmissionTask = new Task<DataTransmissionTaskStackWords>;
	dataTransmissionTask->Create(DataTransmissionTaskLoop, "CLData", nullptr, TaskPriority::ClosedLoop);
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
			reply.copy("Closed loop drive mode not yet supported");
			return false;
		}
# endif
# if USE_TC_FOR_STEP || !SINGLE_DRIVER || !SUPPORT_TMC2160
		// TODO: Check what we need to do here
		reply.copy("Closed loop drive mode not yet supported");
		return false;
# endif
		if (encoder == nullptr)
		{
			reply.copy("No encoder specified for closed loop drive mode");
			return false;
		}
	}

	// Reset the tuning
	tuningError = TUNE_ERR_NOT_PERFORMED_MINIMAL_TUNE;

	// Set the target position to the current position
	rawEncoderReading = encoder->GetReading();
	currentMotorSteps = rawEncoderReading / encoderCountPerStep;
	targetMotorSteps = currentMotorSteps;

	// Set the closed loop enabled state
	closedLoopEnabled = enabled;
	return true;
}

void ClosedLoop::SetHoldingCurrent(float percent)
{
	holdCurrent = constrain<long>(percent, 0, 100) / 100.0;
}

void ClosedLoop::SetStepDirection(bool dir) noexcept
{
	stepDirection = dir;
}

void ClosedLoop::ResetError(size_t driver) noexcept
{
# if SINGLE_DRIVER
	if (driver == 0) {
		// Set the target position to the current position
		rawEncoderReading = encoder->GetReading();
		currentMotorSteps = rawEncoderReading / encoderCountPerStep;
		targetMotorSteps = currentMotorSteps;
	}
# else
#  error Cannot support closed loop with the specified hardware
# endif
}

EncoderType ClosedLoop::GetEncoderType() noexcept
{
	return (encoder == nullptr) ? EncoderType::none : encoder->GetType();
}

// TODO: Instead of having this take step, why not use the current DDA to calculate where we need to be?
void ClosedLoop::TakeStep() noexcept
{
	bool interpolation;	// TODO: Work out what this is for!
# if SUPPORT_TMC2160 && SINGLE_DRIVER
	int microsteps = SmartDrivers::GetMicrostepping(0, interpolation);
	float microstepAngle = microsteps == 0 ? 1 : 1.0/microsteps;
	targetMotorSteps += (stepDirection ? microstepAngle : -microstepAngle) * (Platform::GetDirectionValue(1) ? 1 : -1);
# else
#  error Cannot support closed loop with the specified hardware
# endif
}

//bool ClosedLoop::DeferDriverStateControl(size_t driver, const CanMessageMultipleDrivesRequest<DriverStateControl>& msg)
//{
//	if (tuning && driver == 0) {
//
//		deferredStateControlMsg = msg;
//		deferredStateControl = true;
//		return true;
//	} else {
//		return false;
//	}
//}

GCodeResult ClosedLoop::ProcessM569Point1(const CanMessageGeneric &msg, const StringRef &reply) noexcept
{
	CanMessageGenericParser parser(msg, M569Point1Params);

	// Set default parameters
	uint8_t tempEncoderType = GetEncoderType().ToBaseType();
	float tempCPR = encoderCountPerStep;		// TODO: Use countPerStepToExternalUnits() here
	float tempKp = Kp;
	float tempKi = Ki;
	float tempKd = Kd;
	size_t numThresholds = 4;
	float tempErrorThresholds[numThresholds];
	uint8_t tempCoilPolarity = (coilAPolarity << 1) | coilBPolarity;

	// Pull changed parameters
	uint8_t seen = 0;
	seen |= parser.GetUintParam('T', tempEncoderType) 	<< 0;
	seen |= parser.GetFloatParam('C', tempCPR) 			<< 1;
	seen |= parser.GetFloatParam('R', tempKp) 			<< 2;
	seen |= parser.GetFloatParam('I', tempKi) 			<< 3;
	seen |= parser.GetFloatParam('D', tempKd) 			<< 4;
	seen |= parser.GetFloatArrayParam('E',
				numThresholds,
				tempErrorThresholds) 					<< 5;
	seen |= parser.GetUintParam('L', tempCoilPolarity)  << 6;

	// Report back if !seen
	if (!seen) {
		reply.catf("Encoder type: %s", GetEncoderType().ToString());
		reply.catf(", encoder CPR: %f", (double) tempCPR);
		reply.catf(", PID parameters: p=%f, i=%f, d=%f", (double) Kp, (double) Ki, (double) Kd);
		return GCodeResult::ok;
	}

	// Validate the new params
	if (tempEncoderType > EncoderType::NumValues) {reply.copy("Invalid T value. Valid values are 0,1");return GCodeResult::error;}
	if ((seen & 0x1 << 5) && tempErrorThresholds[0] < 0) {reply.copy("Error threshold value must be greater than zero.");return GCodeResult::error;}
	if ((seen & 0x1 << 5) && tempErrorThresholds[1] < 0) {reply.copy("Error threshold value must be greater than zero.");return GCodeResult::error;}
	if (tempCoilPolarity > 3) {reply.copy("Invalid L value. Valid values are 0,1,2,3");return GCodeResult::error;}

	// Set the new params
	encoderCountPerStep = tempCPR;
	Kp = tempKp;
	Ki = tempKi;
	Kd = tempKd;
	coilAPolarity = tempCoilPolarity & 0x2;
	coilBPolarity = tempCoilPolarity & 0x1;

	if (seen & 0x1 << 5) {
		errorThresholds[0] = tempErrorThresholds[0];
		errorThresholds[1] = tempErrorThresholds[1];
	}

	// If encoder type or count per steps has changed, we need to re-tune
	if ((seen & 0x1 << 0) || (seen & 0x1 << 1)) {
		tuningError |= TUNE_ERR_NOT_PERFORMED_MINIMAL_TUNE;
	}

	//TODO need to get a lock here in case there is any movement
	if (seen & 0x1 << 0) {
		DeleteObject(encoder);
		switch (tempEncoderType)
		{
		case EncoderType::none:
		default:
			encoder = nullptr;
			break;

				case EncoderType::as5047:
#ifdef EXP1HCE
					encoder = new AS5047D(*encoderSpi, EncoderCsPin);
#elif defined(EXP1HCL)
					encoder = new AS5047D(*Platform::sharedSpi, EncoderCsPin);
#endif
					break;

				case EncoderType::tli5012:
#ifdef EXP1HCE
					encoder = new TLI5012B(*encoderSpi, EncoderCsPin);
#elif defined(EXP1HCL)
					encoder = new TLI5012B(*Platform::sharedSpi, EncoderCsPin);
#endif
					break;

		case EncoderType::linearQuadrature:
#ifdef EXP1HCE
			encoder = new QuadratureEncoderAttiny(true);
#elif defined(EXP1HCL)
			encoder = new QuadratureEncoderPdec(true);
#else
# error Unknown board
#endif
			break;

		case EncoderType::rotaryQuadrature:
#ifdef EXP1HCE
			encoder = new QuadratureEncoderAttiny(false);
#elif defined(EXP1HCL)
			// TODO: Debug why this can't be set to rotary mode
			encoder = new QuadratureEncoderPdec(true);
#else
# error Unknown board
#endif
			break;
		}

		if (encoder != nullptr)
		{
			//TODO initialise the encoder
			encoder->Enable();
		}
	}

	reply.printf("%x", seen);
	return GCodeResult::ok;
}

GCodeResult ClosedLoop::ProcessM569Point6(const CanMessageGeneric &msg, const StringRef &reply) noexcept
{
	CanMessageGenericParser parser(msg, M569Point6Params);

	// Check we are in direct drive mode
	if (SmartDrivers::GetDriverMode(0) != DriverMode::direct) {
		reply.copy("Drive is not in closed loop mode.");
		return GCodeResult::error;
	}

	uint8_t desiredTuning;
	if (!parser.GetUintParam('V', desiredTuning))
	{
		reply.copy("Missing parameter 'V'");
		return GCodeResult::error;
	}

	if (desiredTuning > FULL_TUNE)
	{
		reply.printf("Invalid 'V' parameter value. V may be 0-%d.", FULL_TUNE);
		return GCodeResult::error;
	}

	uint8_t prevTuningError = tuningError;
	tuning = desiredTuning;

	// TODO: Is this the best way to do this?
	while (tuning) {
		tuningTask->Give();
	}

	// There are now 3 scenarios
	// 1. No tuning errors exist (!tuningError)							= OK
	// 2. No new tuning errors exist !(~prevTuningError & tuningError)	= WARNING
	// 3. A new tuning error has been introduced (else)					= ERROR

	if (!tuningError) {
		return GCodeResult::ok;
	} else if (!(~prevTuningError & tuningError)) {
		reply.copy("No new tuning errors have been found, but some existing tuning errors exist.");
		ReportTuningErrors(tuningError, reply);
		return GCodeResult::warning;
	} else {
		reply.copy("One or more tuning errors occurred. Closed loop mode has been disabled, please correct this error and re-enable closed loop control.");
		ReportTuningErrors(~prevTuningError & tuningError, reply);
		if (prevTuningError & tuningError) {
			reply.catf(" In addition, the following tuning errors were already present:");
			ReportTuningErrors(prevTuningError & tuningError, reply);
		}
		return GCodeResult::error;
	}
}

void ClosedLoop::Diagnostics(const StringRef& reply) noexcept
{
	reply.printf("Closed loop enabled: %s", closedLoopEnabled ? "yes" : "no");
#if defined(EXP1HCE)
	reply.catf(", encoder programmed status %s, encoder type %s", programmer->GetProgramStatus().ToString(), GetEncoderType().ToString());
#elif defined(EXP1HCL)
	reply.catf(", encoder type %s", GetEncoderType().ToString());
#endif

	reply.catf(", pre-error threshold: %f, error threshold: %f", (double) errorThresholds[0], (double) errorThresholds[1]);
	reply.catf(", coil A polarity: %s, coil B polarity: %s", coilAPolarity ? "+" : "-", coilBPolarity ? "+" : "-");
	reply.catf(", tuning: %#x, tuning error: %#x", tuning, tuningError);

	if (encoder != nullptr)
	{
		reply.catf(", position %" PRIi32, encoder->GetReading());
		encoder->AppendDiagnostics(reply);
	}
	reply.catf(", collecting data: %s", collectingData ? "yes" : "no");
	if (collectingData)
	{
		reply.catf(" (filter: %#x, samples: %d, mode: %d, rate: %d, movement: %d)", filterRequested, samplesRequested, modeRequested, rateRequested, movementRequested);
	}

	reply.catf(", ultimateGain=%f, oscillationPeriod=%f", (double) ultimateGain, (double) oscillationPeriod);

	//DEBUG
	//reply.catf(", event status 0x%08" PRIx32 ", TCC2 CTRLA 0x%08" PRIx32 ", TCC2 EVCTRL 0x%08" PRIx32, EVSYS->CHSTATUS.reg, QuadratureTcc->CTRLA.reg, QuadratureTcc->EVCTRL.reg);
}

void ClosedLoop::Spin() noexcept
{
	if (collectingData && rateRequested == 0) {CollectSample();}
	if (!closedLoopEnabled) {return;}
	if (tuningError) {return tuningTask->Give();}

	if (!tuning) {
		ControlMotorCurrents();
		Log();
	} else {
		tuningTask->Give();
	}
}

void ClosedLoop::CollectSample() noexcept
{
	if (filterRequested & 1)  		{sampleBuffer[sampleBufferWritePointer++] = rawEncoderReading;}
	if (filterRequested & 2)  		{sampleBuffer[sampleBufferWritePointer++] = currentMotorSteps;}
	if (filterRequested & 4)  		{sampleBuffer[sampleBufferWritePointer++] = targetMotorSteps;}
	if (filterRequested & 8)  		{sampleBuffer[sampleBufferWritePointer++] = stepPhase;}
	if (filterRequested & 16)  		{sampleBuffer[sampleBufferWritePointer++] = PIDControlSignal;}
	if (filterRequested & 32)  		{sampleBuffer[sampleBufferWritePointer++] = PIDPTerm;}
	if (filterRequested & 64)  		{sampleBuffer[sampleBufferWritePointer++] = PIDITerm;}
	if (filterRequested & 128)  	{sampleBuffer[sampleBufferWritePointer++] = PIDDTerm;}
	if (filterRequested & 256)  	{sampleBuffer[sampleBufferWritePointer++] = phaseShift;}
	if (filterRequested & 512)  	{sampleBuffer[sampleBufferWritePointer++] = desiredStepPhase;}
	if (filterRequested & 1024) 	{sampleBuffer[sampleBufferWritePointer++] = coilA;}
	if (filterRequested & 2048) 	{sampleBuffer[sampleBufferWritePointer++] = coilB;}
	if (filterRequested & 4096) 	{sampleBuffer[sampleBufferWritePointer++] = currentError;}

	// Count how many bits are set in 'filterRequested'
	// TODO: Look into a more efficient way of doing this
	int variableCount = 0;
	int tmpFilter = filterRequested;
	while (tmpFilter != 0) {
		variableCount += tmpFilter & 0x1;
		tmpFilter >>= 1;
	}

	if (sampleBufferWritePointer >= (samplesRequested * variableCount)) {
		// Mark that we have finished collecting data
		collectingData = false;
		movementRequested = 0;	// Just to be safe
		dataTransmissionTask->Give();
	}
}

void ClosedLoop::Log() noexcept
{
# if SUPPORT_CAN_LOGGING
	// Update the error vars
	maxError = currentError > maxError ? currentError : maxError;
	ewmaError = ewmaError == 0 ? currentError : 0.5 * ewmaError + 0.5 * currentError;

	if (currentError > 1)
	{
		// TODO: Does this take too long to do in ::Spin()?
		String<StringLength500> reply;
		reply.printf("Closed loop error exceeded warning threshold. Error = %f", (double) currentError);
		Logger::LogMessage(0, reply.GetRef(), LogLevel::warn);
	}
# endif
}

int phsCtr = 0;

void ClosedLoop::ControlMotorCurrents() noexcept
{
	// Calculate the current position & phase from the encoder reading
	rawEncoderReading = encoder->GetReading();
	currentMotorSteps = rawEncoderReading / encoderCountPerStep;

	// Calculate the current error, if it's zero we don't need to do anything!
	currentError = targetMotorSteps - currentMotorSteps;
	if (!collectingData && currentError == 0) {return;}	// TODO: We are dealing with floats so this should probably be a range

	// Use a PID controller to calculate the required 'torque' - the control signal
	PIDPTerm = Kp * currentError;
	if (abs(PIDITerm + Ki * currentError) < 512)	// We don't want this to overflow, so set an upper bound.
	{
		PIDITerm += Ki * currentError;		// TODO: Is this causing an overflow?
	}
	PIDDTerm = Kd * (lastError - currentError);
	float sumOfTerms = PIDPTerm + PIDITerm + PIDDTerm;
	PIDControlSignal = (int16_t) constrain<float>(sumOfTerms, -255, 255);	// Clamp between -255 and 255

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

//	desiredStepPhase += 128;
////
//	if (desiredStepPhase >= 4096)
//	{
//		desiredStepPhase = 0;
////		phsCtr += 1;
////		if (phsCtr == 4) {phsCtr = 0;}
//////		switch (phsCtr) {
//////		case 0:
//////			coilAPolarity = true;
//////			coilBPolarity = true;
//////			break;
//////		case 1:
//////			coilAPolarity = false;
//////			coilBPolarity = false;
//////			break;
//////		case 2:
//////			coilAPolarity = true;
//////			coilBPolarity = false;
//////			break;
//////		case 3:
//////			coilAPolarity = false;
//////			coilBPolarity = true;
//////			break;
//////		}
//	}

	// Assert the required motor currents
	SetMotorPhase(desiredStepPhase, abs(PIDControlSignal)/255.0);

	// Update vars for the next cycle
	lastError = currentError;
}

// TODO: This isn't currently called anywhere, but it's quite a useful utility. Do we want this in a GCODE command?
// TODO: If we are going to use this, definitely pull it into the tuning loop
GCodeResult ClosedLoop::FindEncoderCountPerStep(const CanMessageGeneric &msg, const StringRef &reply) noexcept
{
	// TODO: Check we are in closed loop mode
	tuning |= ENCODER_STEPS_CHECK;

	// Set to 0
	SetMotorPhase(512, 1);
# if SUPPORT_TMC2160 && SINGLE_DRIVER
	while (SmartDrivers::UpdatePending(0)) { }
# else
#  error Cannot support closed loop with the specified hardware
# endif
	delayMicroseconds(100000);
	int32_t zeroReading = encoder->GetReading();

	// Set to 1024
	SetMotorPhase(1536, 1);
# if SUPPORT_TMC2160 && SINGLE_DRIVER
	while (SmartDrivers::UpdatePending(0)) { }
# else
#  error Cannot support closed loop with the specified hardware
# endif
	delayMicroseconds(100000);
	int32_t quarterReading = encoder->GetReading();

	// Set to 2048
	SetMotorPhase(2560, 1);
# if SUPPORT_TMC2160 && SINGLE_DRIVER
	while (SmartDrivers::UpdatePending(0)) { }
# else
#  error Cannot support closed loop with the specified hardware
# endif
	delayMicroseconds(100000);
	int32_t halfReading = encoder->GetReading();

	// Set to 3092
	SetMotorPhase(3584, 1);
# if SUPPORT_TMC2160 && SINGLE_DRIVER
	while (SmartDrivers::UpdatePending(0)) { }
# else
#  error Cannot support closed loop with the specified hardware
# endif
	delayMicroseconds(100000);
	int32_t fullReading = encoder->GetReading();

	// Set back to 0
	SetMotorPhase(512, 1);
# if SUPPORT_TMC2160 && SINGLE_DRIVER
	while (SmartDrivers::UpdatePending(0)) { }
# else
#  error Cannot support closed loop with the specified hardware
# endif
	delayMicroseconds(100000);
	int32_t secondZeroReading = encoder->GetReading();

	tuning &= ~ENCODER_STEPS_CHECK;

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


[[noreturn]] void ClosedLoop::TuningLoop() noexcept
{

	while (true) {
		// Wait until there is some tuning to do
		while (!tuning) {TaskBase::Take();}

		// Enable all motors & disable them becoming idle
		Platform::DriveEnableOverride(true);

		// Check we are in direct drive mode
		if (SmartDrivers::GetDriverMode(0) != DriverMode::direct) {
			tuningError |= TUNE_ERR_SYSTEM_ERROR;
			tuning = 0;
		}

		// Wait for the driver registers to be written
		while (SmartDrivers::UpdatePending(0)) {TaskBase::Take(10);}

		// Do a polarity detection manoeuvre
		if (tuning & POLARITY_DETECTION_MANOEUVRE) {

			int correctCoilPhase = 0;
			int correctCoilPhaseError = 0;

			for (int coilPhase=0; coilPhase<4; coilPhase++) {
				int totalError = 0;			// The total error made by this phase arrangement
				// Change the coil phase
				coilAPolarity = coilPhase & 0x2;
				coilBPolarity = coilPhase & 0x1;
				for (desiredStepPhase = 0; desiredStepPhase<4096; desiredStepPhase+=256) {
					// Move the motor
					SetMotorPhase(desiredStepPhase, 1);

					// Wait for the motor to move
					while (SmartDrivers::UpdatePending(0)) {TaskBase::Take(10);}
					vTaskDelay(2);

					// Calculate where the motor has moved to
					rawEncoderReading = encoder->GetReading();
					currentMotorSteps = rawEncoderReading / encoderCountPerStep;
					float tmp = currentMotorSteps / 4;
					if (tmp >= 0) {
						stepPhase = (tmp - (int) tmp) * 4095;
					} else {
						stepPhase = (1 + tmp - (int) tmp) * 4095;
					}

					// Calculate & accumulate the error
					int16_t distance1 = abs(stepPhase - desiredStepPhase);
					int16_t distance2 = abs(4095 - max(stepPhase, desiredStepPhase) + min(stepPhase, desiredStepPhase));
					totalError += min<int16_t>(distance1, distance2);
				}
				// Update if this is the correct coil phase
				if (coilPhase == 0 || totalError < correctCoilPhaseError) {
					correctCoilPhase = coilPhase;
					correctCoilPhaseError = totalError;
				}
			}

			coilAPolarity = correctCoilPhase & 0x2;
			coilBPolarity = correctCoilPhase & 0x1;

			tuning &= ~POLARITY_DETECTION_MANOEUVRE;
			tuningError &= ~TUNE_ERR_NOT_FOUND_POLARITY;
		}

		// Do a zeroing manoeuvre
		if (tuning & ZEROING_MANOEUVRE) {
			// Ease the motor from 4096 down to 0
			desiredStepPhase = 4096*2;	// (*2 because we first divide by 2)
			while (desiredStepPhase > 0) {
				if (desiredStepPhase > 1) {
					desiredStepPhase /= 2;
				} else {
					desiredStepPhase = 0;
				}
				SetMotorPhase(desiredStepPhase, 1);
				while (SmartDrivers::UpdatePending(0)) {TaskBase::Take(10);}
				vTaskDelay(2);
				rawEncoderReading = encoder->GetReading();
				if (collectingData && rateRequested == 0) {CollectSample();}
			}

			// Calculate where the motor has moved to
			rawEncoderReading = encoder->GetReading();
			currentMotorSteps = rawEncoderReading / encoderCountPerStep;
			float tmp = currentMotorSteps / 4;
			if (tmp >= 0) {
				stepPhase = (tmp - (int) tmp) * 4095;
			} else {
				stepPhase = (1 + tmp - (int) tmp) * 4095;
			}

			// Set this as the new zero position
			((QuadratureEncoderPdec*) encoder)->SetOffset(-rawEncoderReading);
			targetMotorSteps = 0;

			tuning &= ~ZEROING_MANOEUVRE;
			tuningError &= ~TUNE_ERR_NOT_ZEROED;
		}

		// Do a polarity check manoeuvre
		if (tuning & POLARITY_CHECK) {
			// We are going to step through a full phase, and check that the error never exceeds max_err

			const int max_err = 5 * (1024 / encoderCountPerStep);	// Allow up to 5x the resolution of the encoder
			int deviations = 0;

			for (desiredStepPhase = 0; desiredStepPhase<4096; desiredStepPhase+=256) {
				// Move the motor
				SetMotorPhase(desiredStepPhase, 1);

				// Wait for the motor to move
				while (SmartDrivers::UpdatePending(0)) {TaskBase::Take(10);}
				vTaskDelay(2);

				// Calculate where the motor has moved to
				rawEncoderReading = encoder->GetReading();
				currentMotorSteps = rawEncoderReading / encoderCountPerStep;
				float tmp = currentMotorSteps / 4;
				if (tmp >= 0) {
					stepPhase = (tmp - (int) tmp) * 4095;
				} else {
					stepPhase = (1 + tmp - (int) tmp) * 4095;
				}

				int16_t distance1 = stepPhase - desiredStepPhase;
				int16_t distance2 = 4095 - max(stepPhase, desiredStepPhase) + min(stepPhase, desiredStepPhase);

				// Check the error in the movement
				if (abs(distance1) > max_err && abs(distance2) > max_err) {
					deviations++;
				}
			}

			// Allow a small number of deviations
			if (deviations > 10) {
				tuningError |= TUNE_ERR_INCORRECT_POLARITY;
			}

			tuning &= ~POLARITY_CHECK;
			tuningError &= ~TUNE_ERR_NOT_CHECKED_POLARITY;
		}

		// Do a polarity detection manoeuvre
		if (tuning & CONTROL_CHECK) {
			// TODO
			tuning &= ~CONTROL_CHECK;
			tuningError &= ~TUNE_ERR_NOT_CHECKED_CONTROL;
		}

		if (tuning & ENCODER_STEPS_CHECK) {
			// TODO
			tuning &= ~ENCODER_STEPS_CHECK;
			tuningError &= ~TUNE_ERR_NOT_CHECKED_ENCODER_STEPS;
		}

		// Do a continuous phase increase manoeuvre
		if (tuning & CONTINUOUS_PHASE_INCREASE_MANOEUVRE) {
			// TODO
			tuning &= ~CONTINUOUS_PHASE_INCREASE_MANOEUVRE;
		}

		// Do a step manoeuvre
		if (tuning & STEP_MANOEUVRE) {
			// TODO
			tuning &= ~STEP_MANOEUVRE;
		}

		// Do a ziegler-nichols tuning manoeuvre
		if (tuning & ZIEGLER_NICHOLS_MANOEUVRE) {

			// We will need to restore these afterwards...
			const float prevKp = Kp;
			const float prevKi = Ki;
			const float prevKd = Kd;

			// Reset the PID controller
			Ki = 0;
			Kd = 0;
			Kp = 0;
			PIDITerm = 0;

			ultimateGain = 0;		// Reset the ultimate gain value
			int direction = 1;		// Which direction are we moving in

			float lowerBound = 0;
			float upperBound = 10000;

			while (upperBound - lowerBound > 100) {

				Kp = lowerBound + (upperBound - lowerBound) / 2;

				targetMotorSteps = currentMotorSteps + (direction * 10);

				// Flip the direction
				direction = -direction;

				int initialRiseTime = 0;		// The time it takes to initially meet the target

				float peakError = 0;			// The peak of the current oscillation
				float prevPeakError = 0;		// The peak of the previous oscillation
				int prevTimestamp = 0;			// The previous time of oscillation

				int oscillationCount = 0;		// The number of oscillations that have occurred

				float ewmaDecayFraction = 0;	// An EWMA of the decay fraction of oscillations
				float ewmaOscillationPeriod = 0;// An EWMA of the oscillation period

				// Run up to a maximum of 4096
				for (int time=0; time<16384; time++) {
					TaskBase::Take(10);		// TODO: Use delayuntil here?

					ControlMotorCurrents();

					float currentPosition = direction * currentMotorSteps;
					float targetPosition = direction * targetMotorSteps;
					float error = abs(currentPosition - targetPosition);

					// Search for the initial rise time
					if (initialRiseTime == 0) {
						if (currentPosition > targetPosition) {
							initialRiseTime = time;
						} else {
							continue;
						}
					}

					// Wait another two initial rise times for oscillations to occur
					if (time < 3 * initialRiseTime) {continue;}

					// We're now in the prime time for oscillations - check if they are actually happening:

					// Record data if we are above the target
					if (currentPosition > targetPosition) {
						peakError = max<float>(peakError, error);
						continue;
					}
					// Process data if we have just crossed the target
					float decayFraction;
					if (peakError > 0) {
						if (prevPeakError > 0) {
							decayFraction = peakError / prevPeakError;
							ewmaDecayFraction =
									ewmaDecayFraction == 0
									? decayFraction
									: 0.7 * ewmaDecayFraction + 0.3 * decayFraction;
							if (oscillationCount > 5) {
								ewmaOscillationPeriod =
										ewmaOscillationPeriod == 0
										? (time - prevTimestamp)
										: 0.3 * ewmaOscillationPeriod + 0.7 * (time - prevTimestamp);
							}
						}
						oscillationCount++;
						prevPeakError = peakError;
						peakError = 0;
						prevTimestamp = time;
					}

					PIDPTerm = ewmaOscillationPeriod;
					PIDDTerm = (time - prevTimestamp);

					// Wait for at least 5 oscillations
					if (oscillationCount < 5) {
						continue;
					}

					// Check that the next 5 oscillations all keep the average decay fraction above 98%
					if (ewmaDecayFraction < 0.98) {
						// No oscillations, this is the new lower bound.
						lowerBound = Kp;
						break;
					}
					if (oscillationCount >= 10) {
						// Oscillations found! This is the new upper bound.
						upperBound = Kp;
						oscillationPeriod = ewmaOscillationPeriod;
						break;
					}

					// If we time out of this loop, assume no oscillations
					if (time == 16383) {
						lowerBound = Kp;
					}

				}
			}

			ultimateGain = upperBound;
			Kp = prevKp;
			Ki = prevKi;
			Kd = prevKd;

			tuning &= ~ZIEGLER_NICHOLS_MANOEUVRE;
		}

		Platform::DriveEnableOverride(false);

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

	if (msg.rate == 0) {
		// Count how many bits are set in 'msg.filter'
		// TODO: Look into a more efficient way of doing this
		int variableCount = 0;
		int tmpFilter = msg.filter;
		while (tmpFilter != 0) {
			variableCount += tmpFilter & 0x1;
			tmpFilter >>= 1;
		}

		const int maxSamples = (CLOSED_LOOP_DATA_BUFFER_SIZE * 12) / variableCount;

		if (msg.numSamples > maxSamples)
		{
			reply.printf("Maximum samples is %d when sample rate is continuous (R0) and %d variables are being collected (D%d)", maxSamples, variableCount, msg.filter);
			return GCodeResult::error;
		}
	}

	if (msg.movement > FULL_TUNE) {
		reply.printf("Maximum value for V is %d. V%d is invalid.", FULL_TUNE, msg.movement);
		return GCodeResult::error;
	}

	// Set up the recording vars
	collectingData = true;
	rateRequested = (1000.0 / msg.rate) / portTICK_PERIOD_MS;
	filterRequested = msg.filter;
	tuning |= msg.movement;
	samplesRequested = msg.numSamples;
	modeRequested = (RecordingMode) msg.mode;

	// Start the data collection task
	dataCollectionTask->Give();
	return GCodeResult::ok;
}

[[noreturn]] void ClosedLoop::DataTransmissionLoop() noexcept
{
	while (true)
	{
		// Only attempt to transmit data if we are not collecting data and data has been collected
		// TODO: This is a poor man's version of a lock - implement an actual lock!
		if (!collectingData && sampleBufferWritePointer > 0)
		{
			// Count how many bits are set in 'filterRequested'
			// TODO: Look into a more efficient way of doing this
			int variableCount = 0;
			int tmpFilter = filterRequested;
			while (tmpFilter != 0) {
				variableCount += tmpFilter & 0x1;
				tmpFilter >>= 1;
			}

			// Work out the maximum number of samples that can be sent in 1 packet
			// TODO: This 14 comes from CanMessageFormats.h::1218. Should it be a constant?
			const int maxSamplesInPacket = 14 / variableCount;

			// Loop for until everything has been read
			while (sampleBufferReadPointer < sampleBufferWritePointer) {
				// Set up a CAN message
				CanMessageBuffer buf(nullptr);
				CanMessageClosedLoopData& msg = *(buf.SetupStatusMessage<CanMessageClosedLoopData>(CanInterface::GetCanAddress(), CanInterface::GetCurrentMasterAddress()));

				// Populate the control fields
				msg.firstSampleNumber = sampleBufferReadPointer / variableCount;
				msg.filter = filterRequested;

				const int samplesRemaining = (sampleBufferWritePointer - sampleBufferReadPointer) / variableCount;
				msg.lastPacket = samplesRemaining <= maxSamplesInPacket;
				msg.numSamples = msg.lastPacket ? samplesRemaining : maxSamplesInPacket;

				int dataLength = 0;
				// TODO: Can we memcpy here instead?
				for (int i=0; i<(msg.numSamples * variableCount); i++)
				{
					msg.data[dataLength++] = sampleBuffer[sampleBufferReadPointer++];
				}

				// Send the CAN message
				buf.dataLength = msg.GetActualDataLength();
				CanInterface::Send(&buf);
			}

			// If we are finished collecting data, reset the buffer
			if (!collectingData)
			{
				sampleBufferReadPointer = 0;
				sampleBufferWritePointer = 0;
			}
		}

		TaskBase::Take(100);
	}
}

[[noreturn]] void ClosedLoop::DataCollectionLoop() noexcept
{
	while (true)
	{

		// If we are not collecting data, block the task
		// If rateRequested == 0, the data collection is handled in ::Spin()
		while (!collectingData || rateRequested == 0)
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
				if (filterRequested & 2)  		{msg.data[dataPointer++] = currentMotorSteps;}	// TODO: To pass back a float, * by 1000 and pass back an int
				if (filterRequested & 4)  		{msg.data[dataPointer++] = targetMotorSteps;}	// TODO: To pass back a float, * by 1000 and pass back an int
				if (filterRequested & 8)  		{msg.data[dataPointer++] = stepPhase;}
				if (filterRequested & 16)  		{msg.data[dataPointer++] = PIDControlSignal;}
				if (filterRequested & 32)  		{msg.data[dataPointer++] = PIDPTerm;}	// TODO: To pass back a float, * by 1000 and pass back an int
				if (filterRequested & 64)  		{msg.data[dataPointer++] = PIDITerm;}	// TODO: To pass back a float, * by 1000 and pass back an int
				if (filterRequested & 128)  	{msg.data[dataPointer++] = PIDDTerm;}	// TODO: To pass back a float, * by 1000 and pass back an int
				if (filterRequested & 256)  	{msg.data[dataPointer++] = phaseShift;}
				if (filterRequested & 512)  	{msg.data[dataPointer++] = desiredStepPhase;}
				if (filterRequested & 1024) 	{msg.data[dataPointer++] = coilA;}
				if (filterRequested & 2048) 	{msg.data[dataPointer++] = coilB;}
				if (filterRequested & 4096) 	{msg.data[dataPointer++] = currentError;}

				// Send the CAN message
				buf.dataLength = msg.GetActualDataLength();
				CanInterface::Send(&buf);
			}

			// Pause to maintain the sample rate (TODO: Implement variable sample rate)
			vTaskDelayUntil(&lastWakeTime, rateRequested);
		}

		// Mark that we have finished collecting data
		collectingData = false;
	}
}

#endif

// End
