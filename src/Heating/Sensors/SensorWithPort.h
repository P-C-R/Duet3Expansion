/*
 * SensorWithPort.h
 *
 *  Created on: 18 Jul 2019
 *      Author: David
 */

#ifndef SRC_HEATING_SENSORS_SENSORWITHPORT_H_
#define SRC_HEATING_SENSORS_SENSORWITHPORT_H_

#include "TemperatureSensor.h"

class SensorWithPort : public TemperatureSensor
{
protected:
	SensorWithPort(unsigned int sensorNum, const char *type);
	~SensorWithPort();

	// Mark this sensor for deletion by the Heat task
	void FlagForDeletion() override;

	// Try to configure the port
	bool ConfigurePort(const CanMessageGenericParser& parser, const StringRef& reply, PinAccess access, bool& seen);

	// Copy the basic details to the reply buffer. This hides the version in the base class.
	void CopyBasicDetails(const StringRef& reply) const;

	IoPort port;
};

#endif /* SRC_HEATING_SENSORS_SENSORWITHPORT_H_ */
