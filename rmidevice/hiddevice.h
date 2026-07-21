/*
 * Copyright (C) 2014 Andrew Duggan
 * Copyright (C) 2014 Synaptics Inc
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _HIDDEVICE_H_
#define _HIDDEVICE_H_

#include <linux/hidraw.h>
#include <string>
#include <fstream>
#include <stdint.h>
#include "rmidevice.h"

enum rmi_hid_mode_type {
	HID_RMI4_MODE_MOUSE                     = 0,
	HID_RMI4_MODE_ATTN_REPORTS              = 1,
	HID_RMI4_MODE_NO_PACKED_ATTN_REPORTS    = 2,
};
/*
enum hid_transport_type {
	HID_TRANSPORT_RMI = 0,
	HID_TRANSPORT_DARFON_TPRMI,
};*/

class HIDDevice : public RMIDevice
{
public:
	HIDDevice() : RMIDevice(), m_inputReport(NULL), m_outputReport(NULL), m_attnData(NULL),
		      m_readData(NULL),
		      m_featureReport(NULL),
		      m_inputReportSize(0),
		      m_outputReportSize(0),
		      m_featureReportSize(0),
		      m_deviceOpen(false),
		      m_mode(HID_RMI4_MODE_ATTN_REPORTS),
		      m_initialMode(HID_RMI4_MODE_MOUSE),
		      m_darfonFeatureReportId(0x0A),
		      m_backdoorEnabled(false),
		      m_transportDeviceName(""),
		      m_driverPath(""),
		      hasVendorDefineLIDMode(false),
		      hasVendorDefineWriteReportID(false),
		      hasVendorDefineReadAddrReportID(false),
		      hasVendorDefineReadDataReportID(false),
		      hasVendorDefineAttentionReportID(false),
		      hasVendorDefineRMIModeReportID(false),
		      hasDarfonOdmFeatureReport(false)
	{}
	virtual int Open(const char * filename);
	virtual int Read(unsigned short addr, unsigned char *buf,
				unsigned short len);
	virtual int Write(unsigned short addr, const unsigned char *buf,
				 unsigned short len);
	virtual int SetMode(int mode);
	virtual int ToggleInterruptMask(bool enable);
	virtual int WaitForAttention(struct timeval * timeout = NULL,
					unsigned int source_mask = RMI_INTERUPT_SOURCES_ALL_MASK);
	virtual int GetAttentionReport(struct timeval * timeout, unsigned int source_mask,
					unsigned char *buf, unsigned int *len);
	virtual void Close();
	virtual void RebindDriver();
	~HIDDevice() { Close(); }

	virtual void PrintDeviceInfo();

	virtual bool FindDevice(enum RMIDeviceType type = RMI_DEVICE_TYPE_ANY);
	virtual bool CheckABSEvent();
	virtual int GetBoardSubID(unsigned char *boardSubID);
	virtual int GetPackratID(unsigned long *packratID);
	virtual int EnsureRMIBackdoorMode(bool force);
//	void SetTransportType(enum hid_transport_type transportType) { m_transportType = transportType; }
//	enum hid_transport_type GetTransportType() const { return m_transportType; }
//	void SetDarfonFeatureReportId(unsigned char reportId) { m_darfonFeatureReportId = reportId; }

private:
	int m_fd;

	struct hidraw_report_descriptor m_rptDesc;
	struct hidraw_devinfo m_info;

	unsigned char *m_inputReport;
	unsigned char *m_outputReport;

	unsigned char *m_attnData;
	unsigned char *m_readData;
	unsigned char *m_featureReport;
	int m_dataBytesRead;

	size_t m_inputReportSize;
	size_t m_outputReportSize;
	size_t m_featureReportSize;

	bool m_deviceOpen;

	rmi_hid_mode_type m_mode;
	rmi_hid_mode_type m_initialMode;
	unsigned char m_darfonFeatureReportId;
	bool m_backdoorEnabled;

	std::string m_transportDeviceName;
	std::string m_driverPath;

	bool hasVendorDefineLIDMode;
	bool hasVendorDefineWriteReportID;
	bool hasVendorDefineReadAddrReportID;
	bool hasVendorDefineReadDataReportID;
	bool hasVendorDefineAttentionReportID;
	bool hasVendorDefineRMIModeReportID;
	bool hasDarfonOdmFeatureReport;

	int GetReport(int *reportId, struct timeval * timeout = NULL);
	int ReadHIDI2CRMI(unsigned short addr, unsigned char *buf, unsigned short len);
	int WriteHIDI2CRMI(unsigned short addr, const unsigned char *buf, unsigned short len);
	int ReadHIDPS2RMI(unsigned short addr, unsigned char *buf, unsigned short len);
	int WriteHIDPS2RMI(unsigned short addr, const unsigned char *buf, unsigned short len);
	int EnterRMIBackdoor(bool bForce = false);
	int SetFeatureReport(unsigned char reportId, const unsigned char *data, size_t dataLen);
	int GetFeatureReport(unsigned char reportId);
	int PutDeviceBytesPolled(const unsigned char *values, size_t valueCount,
				      unsigned int responseBytes = 1);
	int PutDeviceBytePolled(unsigned char value, unsigned int responseBytes = 1);
	int ReadDarfonData(unsigned char *data, size_t *dataLen);
	int WaitForDarfonAck();
	int SetResolutionSequence(unsigned char value);
	int SampleRateSequence(unsigned char sequence, unsigned char value);
	int StatusRequestSequence(unsigned char argument, unsigned long *response);
	void PrintReport(const unsigned char *report);
	void ParseReportDescriptor();

	bool WaitForHidRawDevice(int notifyFd, std::string & hidraw);

	// static HID utility functions
	static bool LookupHidDeviceName(uint32_t bus, int16_t vendorId, int16_t productId, std::string &deviceName);
	static bool LookupHidDriverName(std::string &deviceName, std::string &driverName);
	static bool FindTransportDevice(uint32_t bus, std::string & hidDeviceName,
					std::string & transportDeviceName, std::string & driverPath);
 };

#endif /* _HIDDEVICE_H_ */
