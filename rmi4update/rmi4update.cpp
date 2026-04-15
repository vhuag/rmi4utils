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

#include <alloca.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include "rmi4update.h"

#define RMI_F34_QUERY_SIZE		7
#define RMI_F34_HAS_NEW_REG_MAP		(1 << 0)
#define RMI_F34_IS_UNLOCKED		(1 << 1)
#define RMI_F34_HAS_CONFIG_ID		(1 << 2)
#define RMI_F34_BLOCK_SIZE_OFFSET	1
#define RMI_F34_FW_BLOCKS_OFFSET	3
#define RMI_F34_CONFIG_BLOCKS_OFFSET	5

#define RMI_F34_BLOCK_SIZE_V1_OFFSET	0
#define RMI_F34_FW_BLOCKS_V1_OFFSET	0
#define RMI_F34_CONFIG_BLOCKS_V1_OFFSET	2

#define RMI_F34_BLOCK_DATA_OFFSET	2
#define RMI_F34_BLOCK_DATA_V1_OFFSET	1

#define RMI_F34_COMMAND_MASK		0x0F
#define RMI_F34_STATUS_MASK		0x07
#define RMI_F34_STATUS_SHIFT		4
#define RMI_F34_ENABLED_MASK		0x80

#define RMI_F34_COMMAND_V1_MASK		0x3F
#define RMI_F34_STATUS_V1_MASK		0x3F
#define RMI_F34_ENABLED_V1_MASK		0x80

#define RMI_F34_WRITE_FW_BLOCK        0x02
#define RMI_F34_ERASE_ALL             0x03
#define RMI_F34_WRITE_LOCKDOWN_BLOCK  0x04
#define RMI_F34_WRITE_CONFIG_BLOCK    0x06
#define RMI_F34_ENABLE_FLASH_PROG     0x0f

#define RMI_F34_ENABLE_WAIT_MS 300
#define RMI_F34_ERASE_WAIT_MS (5 * 1000)
#define RMI_F34_ERASE_V8_WAIT_MS (10000)
#define RMI_F34_ENABLE_SBL_WAIT_MS 3000

#if defined(__arm__) || defined(__aarch64__)
#define RMI_F34_IDLE_WAIT_MS 1000
#define RMI_F34_PARTITION_READ_WAIT_MS 200
#else
#define RMI_F34_IDLE_WAIT_MS 500
#define RMI_F34_PARTITION_READ_WAIT_MS 20
#endif
/* Most recent device status event */
#define RMI_F01_STATUS_CODE(status)		((status) & 0x0f)
/* Indicates that flash programming is enabled (bootloader mode). */
#define RMI_F01_STATUS_BOOTLOADER(status)	(!!((status) & 0x40))
/* The device has lost its configuration for some reason. */
#define RMI_F01_STATUS_UNCONFIGURED(status)	(!!((status) & 0x80))

/* Indicates that flash programming is enabled V7(bootloader mode). */
#define RMI_F01_STATUS_BOOTLOADER_v7(status) (!!((status) & 0x80))

/*
 * Sleep mode controls power management on the device and affects all
 * functions of the device.
 */
#define RMI_F01_CTRL0_SLEEP_MODE_MASK	0x03

#define RMI_SLEEP_MODE_NORMAL		0x00
#define RMI_SLEEP_MODE_SENSOR_SLEEP	0x01
#define RMI_SLEEP_MODE_RESERVED0	0x02
#define RMI_SLEEP_MODE_RESERVED1	0x03

/*
 * This bit disables whatever sleep mode may be selected by the sleep_mode
 * field and forces the device to run at full power without sleeping.
 */
#define RMI_F01_CRTL0_NOSLEEP_BIT	(1 << 2)

int RMI4Update::UpdateFirmware(bool force, bool performLockdown)
{
	struct timespec start;
	struct timespec end;
	long long int duration_us = 0;
	int rc;
	const unsigned char eraseAll = RMI_F34_ERASE_ALL;
	int sblFirmwareVersionMajor = 0;
	int sblFirmwareVersionMinor = 0;

	// Clear all interrupts before parsing to avoid unexpected interrupts.
	m_device.ToggleInterruptMask(false);
	rc = FindUpdateFunctions();
	if (rc != UPDATE_SUCCESS) {
		m_device.ToggleInterruptMask(true);
		return rc;
	}

	rc = m_device.QueryBasicProperties();
	if (rc < 0) {
		m_device.ToggleInterruptMask(true);
		return UPDATE_FAIL_QUERY_BASIC_PROPERTIES; 
	}

	if (!force && m_firmwareImage.HasIO()) {
		if (m_firmwareImage.GetFirmwareID() <= m_device.GetFirmwareID()) {
			fprintf(stderr, "Firmware image (%ld) is not newer then the firmware on the device (%ld)\n",
				m_firmwareImage.GetFirmwareID(), m_device.GetFirmwareID());
			rc = UPDATE_FAIL_FIRMWARE_IMAGE_IS_OLDER;
			m_device.ToggleInterruptMask(true);
			return rc;
		}
	}

	fprintf(stdout, "Device Properties:\n");
	m_device.PrintProperties();
	if (m_device.GetDeviceType() == RMI_DEVICE_TYPE_TOUCHPAD) {
		rc = m_firmwareImage.VerifyImageProductID(m_device.GetProductID());
		if (rc != UPDATE_SUCCESS) {
			m_device.ToggleInterruptMask(true);
			return rc;
		}
	} else {
		fprintf(stdout, "not touchpad, skip checking product ID\n");
	}
	

	rc = DisableNonessentialInterupts();
	if (rc != UPDATE_SUCCESS) {
		m_device.ToggleInterruptMask(true);
		return rc;
	}
	

	rc = ReadF34Queries();
	if (rc != UPDATE_SUCCESS)
		return rc;

	if (GetDeviceBootloaderVersion() < BL_V10) {
		// Checking size alignment for the device prior to BL v10.
		rc = m_firmwareImage.VerifyImageMatchesDevice(GetFirmwareSize(), GetConfigSize());
		if (rc != UPDATE_SUCCESS) {
			m_device.ToggleInterruptMask(true);
			return rc;
		}
	} 

	// Parse image-side flash config and compare partitions with device.
	//   BL v7+: CheckEachPartitionSize (check partition size for each partition)
	//   BL v10.1+: CheckEachPartitionExistence (check all partitions exist on both sides)
	//   BL v8~v10: CheckTotalPartitionSize + CheckStartAddr (check total size and flash config address)
	if (GetDeviceBootloaderVersion() >= BL_V7) {
		rc = ParseImageFlashConfig();
		if (rc != UPDATE_SUCCESS) {
			fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
			goto reset;
		}

		rc = CheckEachPartitionSize();
		if (rc != UPDATE_SUCCESS) {
			fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
			goto reset;
		}

		if (GetDeviceBootloaderVersion() >= BL_V8) {
			if (GetDeviceBootloaderVersion() >= BL_V10_1) {
				// BL v10.1+: check that all partitions exist on both sides
				rc = CheckEachPartitionExistence();
				if (rc != UPDATE_SUCCESS) {
					fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
					goto reset;
				}
			} else {
				// BL v7~v10: check total firmware size and flash config start address
				rc = CheckTotalPartitionSize();
				if (rc != UPDATE_SUCCESS) {
					fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
					goto reset;
				}

				rc = CheckStartAddr(FLASH_CONFIG_PARTITION);
				if (rc != UPDATE_SUCCESS) {
					fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
					goto reset;
				}
			}
		}
	}

	if (m_firmwareImage.HasSBL()) {
		if (m_hasSBL) {
			fprintf(stdout, "Entering SBL mode...\n");
			rc = EnterSBLModeV10_1();
			if (rc != UPDATE_SUCCESS) {
				fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
				goto reset;
			}
			fprintf(stdout, "Entering SBL mode done...\n");

			if (GetDeviceBootloaderVersion() >= BL_V10_1) {
				sblFirmwareVersionMajor = m_device.GetFirmwareVersionMajor();
				sblFirmwareVersionMinor = m_device.GetFirmwareVersionMinor();
				fprintf(stdout, "SBL Firmware Version: %d.%d\n", 
					sblFirmwareVersionMajor, sblFirmwareVersionMinor);
			}
		}
	}	

	if (m_f34.GetFunctionVersion() == 0x02) {
		fprintf(stdout, "Enable Flash V7+...\n");
		bool needUpdateSBL = false;
		rc = EnterFlashProgrammingV7();
		if (rc != UPDATE_SUCCESS) {
			fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
			goto reset;
		}
		fprintf(stdout, "Enable Flash done V7+...\n");

		if (GetDeviceBootloaderVersion() >= BL_V8_7) {
			if (m_firmwareImage.IsImageHasFirmwareVersion()) {
				rc = ReadMSL();
				if (rc != UPDATE_SUCCESS) {
					fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
					goto reset;
				}
				fprintf(stdout, "MSL : 0x%x\n", m_MSL);
				if (m_MSL > m_firmwareImage.GetFirmwareVersion()) {
					fprintf(stdout, "MSL checking failed. device(0x%x) > image(0x%x)\n", 
						m_MSL, m_firmwareImage.GetFirmwareVersion());
					rc = UPDATE_FAIL_MSL_CHECKING;
					goto reset;
				} else {
					fprintf(stdout, "Passing MSL checking\n");
				}
			}
		}

		if (GetDeviceBootloaderVersion() >= BL_V10) {
			fprintf(stdout, "Writing FLD V10...\n");
			rc = WriteFLDV7();
			if (rc != UPDATE_SUCCESS) {
				fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
				goto reset;
			}
			fprintf(stdout, "Writing FLD done V10...\n");

			fprintf(stdout, "Erasing Flash Config V10...\n");
			rc = EraseFlashConfigV10();
			if (rc != UPDATE_SUCCESS) {
				fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
				goto reset;
			}
			fprintf(stdout, "Erasing Flash Config done V10...\n");

			if (m_firmwareImage.GetFlashConfigData()) {
				fprintf(stdout, "Writing flash configuration V10...\n");
				rc = WriteFlashConfigV7();
				if (rc != UPDATE_SUCCESS) {
					fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
					goto reset;
				}
				fprintf(stdout, "Writing flash config done V10...\n");
			}

			// v10.1 checking SBL
			if (GetDeviceBootloaderVersion() >= BL_V10_1 && m_hasSBLMSL && m_firmwareImage.HasSBL()) {
				fprintf(stdout, "Reading SBL MSL V10.1 ...\n");
				rc = ReadSBLMSL();
				if (rc != UPDATE_SUCCESS) {
					fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
					goto reset;
				}
				fprintf(stdout, "SBL MSL : 0x%x\n", m_sblMSL);
				unsigned short previousSBLFirmwareVersion = sblFirmwareVersionMajor << 8 | sblFirmwareVersionMinor;
				fprintf(stdout, "previousSBLFirmwareVersion : 0x%x\n", previousSBLFirmwareVersion);
				if (m_sblMSL > previousSBLFirmwareVersion) {
					fprintf(stdout, "SBL MSL(0x%x) > SBL FirmwareVersion(0x%x), need to update SBL\n", 
						m_sblMSL, previousSBLFirmwareVersion);
					needUpdateSBL = true;
				} else {
					fprintf(stdout, "SBL MSL(0x%x) <= SBL FirmwareVersion(0x%x), skip update SBL\n", 
						m_sblMSL, previousSBLFirmwareVersion);
				}
			}

			if (m_firmwareImage.HasSBL()) {
				if (needUpdateSBL) {
					fprintf(stdout, "Erasing SBL V10...\n");
					rc = EraseSBLV10_1();
					if (rc != UPDATE_SUCCESS) {
						fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
						goto reset;
					}
					fprintf(stdout, "Erasing SBL done V10...\n");

					if (m_firmwareImage.GetSBLData()) {
						fprintf(stdout, "Writing SBL V10...\n");
						rc = WriteSBLV10_1();
						if (rc != UPDATE_SUCCESS) {
							fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
							goto reset;
						}
						fprintf(stdout, "Writing SBL done V10...\n");
					}
				}
				fprintf(stdout, "Entering SBL mode...\n");
				rc = EnterSBLModeV10_1();
				if (rc != UPDATE_SUCCESS) {
					fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
					goto reset;
				}
				fprintf(stdout, "Entering SBL mode done...\n");
			}

			fprintf(stdout, "Erasing Core Code V10...\n");
			rc = EraseCoreCodeV10();
			if (rc != UPDATE_SUCCESS) {
				fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
				goto reset;
			}
			fprintf(stdout, "Erasing Core Code done V10...\n");

			if (m_firmwareImage.GetFirmwareData()) {
				fprintf(stdout, "Writing Core Code V10...\n");
				rc = WriteFirmwareV7();
				if (rc != UPDATE_SUCCESS) {
					fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
					goto reset;
				}
				fprintf(stdout, "Writing Core Code done V10...\n");
			}

			if (m_firmwareImage.GetConfigData()) {
				fprintf(stdout, "Writing Core Config V10...\n");
				rc = WriteCoreConfigV7();
				if (rc != UPDATE_SUCCESS) {
					fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
					goto reset;
				}
				fprintf(stdout, "Writing Core Config done V10...\n");
				goto reset;
			}
			
			if (m_firmwareImage.GetGlobalParametersSize() && m_hasGlobalParameters) {
				fprintf(stdout, "Writing Global Parameters V10...\n");
				rc = WriteGlobalParametersV7();
				if (rc != UPDATE_SUCCESS) {
					fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
					goto reset;
				}
				fprintf(stdout, "Writing Global Parameters done V10...\n");
				goto reset;	
			}


		} else {
			if (!m_IsErased){
				fprintf(stdout, "Erasing FW V7+...\n");
				rc = EraseFirmwareV7();
				if (rc != UPDATE_SUCCESS) {
					fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
					goto reset;
				}
				fprintf(stdout, "Erasing FW done V7+...\n");
			}
			if(GetDeviceBootloaderVersion() >= BL_V8){
				if (m_firmwareImage.GetFlashConfigData()) {
					fprintf(stdout, "Writing flash configuration V8...\n");
					rc = WriteFlashConfigV7();
					if (rc != UPDATE_SUCCESS) {
						fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
						goto reset;
					}
					fprintf(stdout, "Writing flash config done V8...\n");
					fprintf(stdout, "Resetting after Writing flash config V8...\n");
					m_device.Reset();
					if (rc != UPDATE_SUCCESS) {
						fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
						goto reset;
					}
					fprintf(stdout, "Reset done after Writing flash config V8...\n");
				}
			}
			if (m_firmwareImage.GetFirmwareData()) {
				fprintf(stdout, "Writing firmware V7+...\n");
				rc = WriteFirmwareV7();
				if (rc != UPDATE_SUCCESS) {
					fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
					goto reset;
				}
				fprintf(stdout, "Writing firmware done V7+...\n");
			}
			if (m_firmwareImage.GetConfigData()) {
				fprintf(stdout, "Writing core configuration V7+...\n");
				rc = WriteCoreConfigV7();
				if (rc != UPDATE_SUCCESS) {
					fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
					goto reset;
				}
				fprintf(stdout, "Writing core config done V7+...\n");
				goto reset;
			}
		}
		
		
	} else {
		rc = EnterFlashProgramming();
		if (rc != UPDATE_SUCCESS) {
			fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
			goto reset;
		}
	}

	if (performLockdown && m_unlocked) {
		if (m_firmwareImage.GetLockdownData()) {
			fprintf(stdout, "Writing lockdown...\n");
			clock_gettime(CLOCK_MONOTONIC, &start);
			rc = WriteBlocks(m_firmwareImage.GetLockdownData(),
					m_firmwareImage.GetLockdownSize() / 0x10,
					RMI_F34_WRITE_LOCKDOWN_BLOCK);
			if (rc != UPDATE_SUCCESS) {
				fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
				goto reset;
			}
			clock_gettime(CLOCK_MONOTONIC, &end);
			duration_us = diff_time(&start, &end);
			fprintf(stdout, "Done writing lockdown, time: %lld us.\n", duration_us);
		}

		rc = EnterFlashProgramming();
		if (rc != UPDATE_SUCCESS) {
			fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
			goto reset;
		}
	}

	rc = WriteBootloaderID();
	if (rc != UPDATE_SUCCESS) {
		fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
		goto reset;
	}

	fprintf(stdout, "Erasing FW...\n");
	clock_gettime(CLOCK_MONOTONIC, &start);
	rc = m_device.Write(m_f34StatusAddr, &eraseAll, 1);
	if (rc != 1) {
		fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(UPDATE_FAIL_ERASE_ALL));
		rc = UPDATE_FAIL_ERASE_ALL;
		goto reset;
	}

	rc = WaitForIdle(RMI_F34_ERASE_WAIT_MS);
	if (rc != UPDATE_SUCCESS) {
		fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
		goto reset;
	}
	clock_gettime(CLOCK_MONOTONIC, &end);
	duration_us = diff_time(&start, &end);
	fprintf(stdout, "Erase complete, time: %lld us.\n", duration_us);

	if (m_firmwareImage.GetFirmwareData()) {
		fprintf(stdout, "Writing firmware...\n");
		clock_gettime(CLOCK_MONOTONIC, &start);
		rc = WriteBlocks(m_firmwareImage.GetFirmwareData(), m_fwBlockCount,
						RMI_F34_WRITE_FW_BLOCK);
		if (rc != UPDATE_SUCCESS) {
			fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
			goto reset;
		}
		clock_gettime(CLOCK_MONOTONIC, &end);
		duration_us = diff_time(&start, &end);
		fprintf(stdout, "Done writing FW, time: %lld us.\n", duration_us);
	}

	if (m_firmwareImage.GetConfigData()) {
		fprintf(stdout, "Writing configuration...\n");
		clock_gettime(CLOCK_MONOTONIC, &start);
		rc = WriteBlocks(m_firmwareImage.GetConfigData(), m_configBlockCount,
				RMI_F34_WRITE_CONFIG_BLOCK);
		if (rc != UPDATE_SUCCESS) {
			fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
			goto reset;
		}
		clock_gettime(CLOCK_MONOTONIC, &end);
		duration_us = diff_time(&start, &end);
		fprintf(stdout, "Done writing config, time: %lld us.\n", duration_us);
	}

reset:
	m_device.Reset();
rebind:
	if (GetDeviceBootloaderVersion() >= BL_V10) {
		Sleep(5000);
	}
	m_device.RebindDriver();
	if(!m_device.CheckABSEvent())
	{
		goto rebind;
	}

	// In order to print out new PR
	rc = FindUpdateFunctions();
	if (rc != UPDATE_SUCCESS)
		return rc;

	rc = m_device.QueryBasicProperties();
	if (rc < 0)
		return UPDATE_FAIL_QUERY_BASIC_PROPERTIES;
	fprintf(stdout, "Device Properties:\n");
	m_device.PrintProperties();

	return rc;

}

int RMI4Update::DisableNonessentialInterupts()
{
	int rc;
	unsigned char interruptEnabeMask = m_f34.GetInterruptMask() | m_f01.GetInterruptMask();

	rc = m_device.Write(m_f01.GetControlBase() + 1, &interruptEnabeMask, 1);
	if (rc != 1)
		return rc;

	return UPDATE_SUCCESS;
}

int RMI4Update::FindUpdateFunctions()
{
	if (0 > m_device.ScanPDT())
		return UPDATE_FAIL_SCAN_PDT;

	if (!m_device.GetFunction(m_f01, 0x01))
		return UPDATE_FAIL_NO_FUNCTION_01;

	if (!m_device.GetFunction(m_f34, 0x34))
		return UPDATE_FAIL_NO_FUNCTION_34;

	return UPDATE_SUCCESS;
}

int RMI4Update::rmi4update_poll()
{
	unsigned char f34_status;
	unsigned short dataAddr = m_f34.GetDataBase();
	int rc;

	rc = m_device.Read(dataAddr, &f34_status, sizeof(unsigned char));
	if (rc != sizeof(unsigned char))
		return UPDATE_FAIL_WRITE_FLASH_COMMAND;

	m_flashStatus = f34_status & 0x1F;
	m_inBLmode = f34_status & 0x80;
	if(!m_flashStatus)
		rc = m_device.Read(dataAddr + 4, &m_flashCmd, sizeof(unsigned char));

	return 0;
}

int RMI4Update::ReadFlashConfig()
{
	int rc;
	int ret = UPDATE_SUCCESS;
	int transaction_count, remain_block;
	unsigned char *flash_cfg = NULL;
	int transfer_leng = 0;
	int read_leng = 0;
	int offset = 0;
	unsigned char trans_leng_buf[2];
	unsigned char cmd_buf[1];
	unsigned char off[2] = {0, 0};
	unsigned char partition_id = FLASH_CONFIG_PARTITION;
	unsigned short dataAddr = m_f34.GetDataBase();
	int i;
	int retry = 0;
	unsigned char *data_temp = NULL;
	struct partition_tbl *partition_temp = NULL;

	flash_cfg = (unsigned char *)malloc(m_blockSize * m_flashConfigLength);
	if (flash_cfg == NULL) {
		fprintf(stderr, "%s: Memory allocation failure at flash_cfg\n", __func__);
		ret = UPDATE_FAIL_MEMORY_ALLOCATION;
		goto cleanup;
	}
	memset(flash_cfg, 0, m_blockSize * m_flashConfigLength);

	partition_temp = (partition_tbl *)malloc(sizeof(struct partition_tbl));
	if (partition_temp == NULL) {
		fprintf(stderr, "%s: Memory allocation failure at partition_temp\n", __func__);
		ret = UPDATE_FAIL_MEMORY_ALLOCATION;
		goto cleanup;
	}
	memset(partition_temp, 0, sizeof(struct partition_tbl));	

	/* calculate the count */
	remain_block = (m_flashConfigLength % m_payloadLength);
	transaction_count = (m_flashConfigLength / m_payloadLength);

	if (remain_block > 0)
		transaction_count++;

	/* set partition id for bootloader 7 */
	rc = m_device.Write(dataAddr + 1, &partition_id, sizeof(partition_id));
	if (rc != sizeof(partition_id)) {
		ret = UPDATE_FAIL_WRITE_FLASH_COMMAND;
		goto cleanup;
	}
	rc = m_device.Write(dataAddr + 2, off, sizeof(off));
	if (rc != sizeof(off)) {
		ret = UPDATE_FAIL_WRITE_INITIAL_ZEROS;
		goto cleanup;
	}

	for (i = 0; i < transaction_count; i++)
	{
		if ((i == (transaction_count -1)) && (remain_block > 0))
			transfer_leng = remain_block;
		else
			transfer_leng = m_payloadLength;

		// Set Transfer Length
		trans_leng_buf[0] = (unsigned char)(transfer_leng & 0xFF);
		trans_leng_buf[1] = (unsigned char)((transfer_leng & 0xFF00) >> 8);
		rc = m_device.Write(dataAddr + 3, trans_leng_buf, sizeof(trans_leng_buf));
		if (rc != sizeof(trans_leng_buf)) {
			ret = UPDATE_FAIL_WRITE_FLASH_COMMAND;
			goto cleanup;
		}

		// Set Command to Read
		cmd_buf[0] = (unsigned char)CMD_V7_READ;
		rc = m_device.Write(dataAddr + 4, cmd_buf, sizeof(cmd_buf));
		if (rc != sizeof(cmd_buf))
			return UPDATE_FAIL_WRITE_FLASH_COMMAND;

		if(m_device.GetDeviceType() == RMI_DEVICE_TYPE_TOUCHPAD)  {
			// Sleep 20 ms and wait for attention for touchpad only.
			Sleep(20);
			rc = WaitForIdle(RMI_F34_PARTITION_READ_WAIT_MS, false);
			if (rc != UPDATE_SUCCESS) {
				fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
				ret = UPDATE_FAIL_TIMEOUT_WAITING_FOR_ATTN;
				goto cleanup;
			}
			fprintf(stdout, "Got attention\n");
		}

		//Wait for completion
		do {
			Sleep(20);
			rmi4update_poll();
			if (m_flashStatus == SUCCESS){
				break;
			}
			retry++;
		} while(retry < 20);

		read_leng = transfer_leng * m_blockSize;
		data_temp = (unsigned char *) malloc(sizeof(char) * read_leng);
		if (data_temp == NULL) {
			fprintf(stderr, "%s: Memory allocation failure at data_temp\n", __func__);
			ret = UPDATE_FAIL_MEMORY_ALLOCATION;
			goto cleanup;
		}
		rc = m_device.Read(dataAddr + 5, data_temp, sizeof(char) * read_leng);
		if (rc != ((ssize_t)sizeof(char) * read_leng)) {
			ret = UPDATE_FAIL_READ_F34_QUERIES;
			goto cleanup;
		}

		// Validate that the memcpy won't overflow the flash_cfg buffer
		if (offset + read_leng > (int)(m_blockSize * m_flashConfigLength)) {
			fprintf(stderr, "%s: Buffer overflow detected\n", __func__);
			ret = UPDATE_FAIL_READ_F34_QUERIES;
			goto cleanup;
		}

		memcpy(flash_cfg + offset, data_temp, sizeof(char) * read_leng);
		offset += read_leng;
		free(data_temp);
		data_temp = NULL;
	}

	// Initialize all partition pointers as NULL to avoid segmentation fault.
	m_partitionConfig = NULL;
	m_partitionCore = NULL;
	m_partitionGuest = NULL;
	m_partitionBootloader = NULL;
	m_partitionDeviceConfig = NULL;
	m_partitionFlashConfigEntry = NULL;
	m_partitionManufacturingBlock = NULL;
	m_partitionGuestSerialization = NULL;
	m_partitionGlobalParameters = NULL;
	m_partitionDisplayConfig = NULL;
	m_partitionExternalTouchAFEConfig = NULL;
	m_partitionUtilityParameter = NULL;
	m_partitionFLD = NULL;
	m_partitionSBMB = NULL;
	m_partitionCount = 0;

	/* Parse all partitions from flash config data.
	 * Each partition entry is 10 bytes (struct partition_tbl), starting at offset 2.
	 */
	if (m_device.m_hasDebug) {
		fprintf(stdout, "\n=== Partition Table (from Flash Config) ===\n");
		fprintf(stdout, "%-4s %-24s %-10s %-10s %-10s %-8s\n",
			"ID", "Name", "Length", "Address", "Properties", "CRCType");
		fprintf(stdout, "-----------------------------------------------------------------------\n");
	}

	for (i = 2; i < m_blockSize * m_flashConfigLength; i = i + 10)
	{
		memcpy(partition_temp->data, flash_cfg + i, sizeof(struct partition_tbl));

		if (partition_temp->partition_id == NONE_PARTITION)
			break;

		if (m_device.m_hasDebug) {
			/* Print each partition entry: ID, name, length (blocks), address, properties, CRC type */
			fprintf(stdout, "0x%02X %-24s %-10d 0x%04X     0x%04X     0x%02X\n",
				partition_temp->partition_id,
				GetPartitionName(partition_temp->partition_id),
				partition_temp->partition_len,
				partition_temp->partition_addr,
				partition_temp->partition_prop,
				partition_temp->partition_crc_type);
		}
		m_partitionCount++;

		/* Allocate and store each recognized partition */
		struct partition_tbl **target = NULL;

		switch (partition_temp->partition_id) {
		case BOOTLOADER_PARTITION:
			target = &m_partitionBootloader;
			break;
		case DEVICE_CONFIG_PARTITION:
			target = &m_partitionDeviceConfig;
			break;
		case FLASH_CONFIG_PARTITION:
			target = &m_partitionFlashConfigEntry;
			break;
		case MANUFACTURING_BLOCK_PARTITION:
			target = &m_partitionManufacturingBlock;
			break;
		case GUEST_SERIALIZATION_PARTITION:
			target = &m_partitionGuestSerialization;
			break;
		case GLOBAL_PARAMETERS_PARTITION:
			target = &m_partitionGlobalParameters;
			break;
		case CORE_CODE_PARTITION:
			target = &m_partitionCore;
			break;
		case CORE_CONFIG_PARTITION:
			target = &m_partitionConfig;
			break;
		case GUEST_CODE_PARTITION:
			target = &m_partitionGuest;
			break;
		case DISPLAY_CONFIG_PARTITION:
			target = &m_partitionDisplayConfig;
			break;
		case EXTERNAL_TOUCH_AFE_CONFIG_PARTITION:
			target = &m_partitionExternalTouchAFEConfig;
			break;
		case UTILITY_PARAMETER_PARTITION:
			target = &m_partitionUtilityParameter;
			break;
		case FIXED_LOCATION_DATA_PARTITION:
			target = &m_partitionFLD;
			break;
		case SBMB_PARTITION:
			target = &m_partitionSBMB;
			break;
		default:
			/* Unknown or unhandled partition, skip storage */
			break;
		}

		if (target) {
			*target = (partition_tbl *) malloc(sizeof(struct partition_tbl));
			if (*target == NULL) {
				fprintf(stderr, "%s: Memory allocation failure for partition 0x%02X (%s)\n",
					__func__, partition_temp->partition_id,
					GetPartitionName(partition_temp->partition_id));
				ret = UPDATE_FAIL_MEMORY_ALLOCATION;
				goto cleanup;
			}
			memcpy(*target, partition_temp, sizeof(struct partition_tbl));
		}

		memset(partition_temp, 0, sizeof(struct partition_tbl));
	}

	if (m_device.m_hasDebug) {
		fprintf(stdout, "---------------------------------------------------------------\n");
		fprintf(stdout, "Total partitions found: %d\n\n", m_partitionCount);
	}

	m_fwBlockCount = m_partitionCore ? m_partitionCore->partition_len : 0;
	m_configBlockCount = m_partitionConfig ? m_partitionConfig->partition_len : 0;
	m_guestBlockCount = m_partitionGuest ? m_partitionGuest->partition_len : 0;

	fprintf(stdout, "F34 fw blocks:     %d\n", m_fwBlockCount);
	fprintf(stdout, "F34 config blocks: %d\n", m_configBlockCount);
	fprintf(stdout, "F34 guest blocks:  %d\n", m_guestBlockCount);

	if(m_partitionGuest != NULL && (m_guestBlockCount * m_blockSize) >= 4) {
		m_guestData = (unsigned char *) malloc(m_guestBlockCount * m_blockSize);
		if (m_guestData != NULL) {
			memset(m_guestData, 0, m_guestBlockCount * m_blockSize);
			memset(m_guestData + m_guestBlockCount * m_blockSize -4, 0, 4);
		}
	}

cleanup:
	if (flash_cfg)
		free(flash_cfg);
	if (partition_temp)
		free(partition_temp);
	if(data_temp)
		free(data_temp);

	return ret;
}

struct partition_tbl * RMI4Update::GetDevicePartition(int id)
{
	switch (id) {
		case BOOTLOADER_PARTITION:              return m_partitionBootloader;
		case DEVICE_CONFIG_PARTITION:           return m_partitionDeviceConfig;
		case FLASH_CONFIG_PARTITION:            return m_partitionFlashConfigEntry;
		case MANUFACTURING_BLOCK_PARTITION:     return m_partitionManufacturingBlock;
		case GUEST_SERIALIZATION_PARTITION:     return m_partitionGuestSerialization;
		case GLOBAL_PARAMETERS_PARTITION:       return m_partitionGlobalParameters;
		case CORE_CODE_PARTITION:               return m_partitionCore;
		case CORE_CONFIG_PARTITION:             return m_partitionConfig;
		case GUEST_CODE_PARTITION:              return m_partitionGuest;
		case DISPLAY_CONFIG_PARTITION:          return m_partitionDisplayConfig;
		case EXTERNAL_TOUCH_AFE_CONFIG_PARTITION: return m_partitionExternalTouchAFEConfig;
		case UTILITY_PARAMETER_PARTITION:       return m_partitionUtilityParameter;
		case FIXED_LOCATION_DATA_PARTITION:     return m_partitionFLD;
		case SBMB_PARTITION:                    return m_partitionSBMB;
		default:                                return NULL;
	}
}

int RMI4Update::ParseImageFlashConfig()
{
	unsigned char *imgFlashCfg = m_firmwareImage.GetFlashConfigData();
	unsigned long imgFlashCfgSize = m_firmwareImage.GetFlashConfigSize();

	memset(m_imgPartitions, 0, sizeof(m_imgPartitions));

	if (!imgFlashCfg || imgFlashCfgSize <= 2) {
		fprintf(stdout, "Image has no flash config data, skip image partition parsing\n");
		return UPDATE_SUCCESS;
	}

	struct partition_tbl entry;
	int idx;

	fprintf(stdout, "\n=== Image Partition Table (from firmware image flash config) ===\n");
	fprintf(stdout, "%-4s %-24s %-10s %-10s %-10s %-8s\n",
		"ID", "Name", "Length", "Address", "Properties", "CRCType");
	fprintf(stdout, "-----------------------------------------------------------------------\n");

	for (idx = 2; idx + 10 <= (int)imgFlashCfgSize; idx += 10) {
		memcpy(entry.data, imgFlashCfg + idx, sizeof(struct partition_tbl));

		if (entry.partition_id == NONE_PARTITION)
			break;

		if (entry.partition_id >= MAX_PARTITION_ID) {
			fprintf(stderr, "  >> unknown partition id 0x%02X, skipping\n",
				entry.partition_id);
			continue;
		}

		fprintf(stdout, "0x%02X %-24s %-10d 0x%04X     0x%04X     0x%02X\n",
			entry.partition_id,
			GetPartitionName(entry.partition_id),
			entry.partition_len,
			entry.partition_addr,
			entry.partition_prop,
			entry.partition_crc_type);

		memcpy(&m_imgPartitions[entry.partition_id], &entry, sizeof(struct partition_tbl));
	}
	fprintf(stdout, "=== End Image Partition Table ===\n\n");

	return UPDATE_SUCCESS;
}

// CheckEachPartitionExistence - check that each partition exists on both image and device.
int RMI4Update::CheckEachPartitionExistence()
{
	bool success = true;

	for (int i = 1; i < MAX_PARTITION_ID; i++) {
		struct partition_tbl *devPart = GetDevicePartition(i);
		unsigned short imgLen = m_imgPartitions[i].partition_len;
		unsigned short devLen = devPart ? devPart->partition_len : 0;

		if (imgLen == 0 && devLen != 0) {
			fprintf(stderr, "%s partition doesn't exist in image file.\n",
				GetPartitionName(i));
			success = false;
		} else if (imgLen != 0 && devLen == 0) {
			fprintf(stderr, "%s partition doesn't exist in device.\n",
				GetPartitionName(i));
			success = false;
		}
	}

	if (!success) {
		fprintf(stderr, "Partition mismatch between image and device\n");
		return UPDATE_FAIL_PARTITION_NOT_MATCH;
	}

	fprintf(stdout, "CheckEachPartitionExistence: all partitions match\n");
	return UPDATE_SUCCESS;
}


// CheckEachPartitionSize - compare partition_len for each partition between image and device.
int RMI4Update::CheckEachPartitionSize()
{
	bool success = true;

	fprintf(stdout, "\n=== Check Each Partition Size (image vs device) ===\n");

	for (int i = 1; i < MAX_PARTITION_ID; i++) {
		struct partition_tbl *devPart = GetDevicePartition(i);
		unsigned short imgLen = m_imgPartitions[i].partition_len;
		unsigned short devLen = devPart ? devPart->partition_len : 0;

		/* Skip if neither side has this partition */
		if (imgLen == 0 && devLen == 0)
			continue;

		if (imgLen != devLen) {
			fprintf(stderr, "  %s: SIZE MISMATCH image=%d blocks, device=%d blocks\n",
				GetPartitionName(i), imgLen, devLen);
			success = false;
		} else {
			fprintf(stdout, "  %s: size match (%d blocks)\n",
				GetPartitionName(i), imgLen);
		}
	}

	if (!success) {
		fprintf(stderr, "Partition size mismatch detected\n");
		return UPDATE_FAIL_PARTITION_SIZE_NOT_MATCH;
	}

	fprintf(stdout, "CheckEachPartitionSize: all partition sizes match\n\n");
	return UPDATE_SUCCESS;
}

// CheckTotalPartitionSize - compare total partition lengths between image and device.
int RMI4Update::CheckTotalPartitionSize()
{
	int dev_len = 0, img_len = 0;

	for (int i = 1; i < MAX_PARTITION_ID; i++) {
		struct partition_tbl *devPart = GetDevicePartition(i);
		if (devPart) {
			img_len += m_imgPartitions[i].partition_len;
			dev_len += devPart->partition_len;
		}
	}

	fprintf(stdout, "CheckTotalPartitionSize: image total=%d blocks, device total=%d blocks\n",
		img_len, dev_len);

	if (img_len != dev_len) {
		fprintf(stderr, "Application area size does not match with device.\n");
		return UPDATE_FAIL_PARTITION_SIZE_NOT_MATCH;
	}

	return UPDATE_SUCCESS;
}


//CheckStartAddr - compare start physical block address for a specific partition.
int RMI4Update::CheckStartAddr(int partition_id)
{
	if (partition_id <= NONE_PARTITION || partition_id >= MAX_PARTITION_ID)
		return UPDATE_SUCCESS;

	struct partition_tbl *devPart = GetDevicePartition(partition_id);
	if (!devPart)
		return UPDATE_SUCCESS;

	unsigned short imgAddr = m_imgPartitions[partition_id].partition_addr;
	unsigned short devAddr = devPart->partition_addr;

	/* Skip check if either address is 0 (partition may not have meaningful address) */
	if (imgAddr == 0 || devAddr == 0)
		return UPDATE_SUCCESS;

	if (imgAddr != devAddr) {
		fprintf(stderr, "%s start physical block address is not matched with device. "
			"(image=0x%04X, device=0x%04X)\n",
			GetPartitionName(partition_id), imgAddr, devAddr);
		return UPDATE_FAIL_PARTITION_START_ADDR_NOT_MATCH;
	}

	fprintf(stdout, "CheckStartAddr: %s address match (0x%04X)\n",
		GetPartitionName(partition_id), imgAddr);
	return UPDATE_SUCCESS;
}

int RMI4Update::ReadF34QueriesV7()
{
	int rc;
	struct f34_v7_query_0 query_0;
	struct f34_v7_query_1_7 query_1_7;
	unsigned char idStr[3];
	unsigned short queryAddr = m_f34.GetQueryBase();
	unsigned char offset;

	rc = m_device.Read(queryAddr, query_0.data, sizeof(query_0.data));
	if (rc != sizeof(query_0.data))
		return UPDATE_FAIL_READ_BOOTLOADER_ID;

	offset = query_0.subpacket_1_size + 1;
	rc = m_device.Read(queryAddr + offset, query_1_7.data, sizeof(query_1_7.data));
	if (rc != sizeof(query_1_7.data))
		return UPDATE_FAIL_READ_BOOTLOADER_ID;

	m_bootloaderID[0] = query_1_7.bl_minor_revision;
	m_bootloaderID[1] = query_1_7.bl_major_revision;
	m_hasConfigID = query_0.has_config_id;
	m_blockSize = query_1_7.block_size_15_8 << 8 |
			query_1_7.block_size_7_0;
	m_flashConfigLength = query_1_7.flash_config_length_15_8 << 8 |
				query_1_7.flash_config_length_7_0;
	m_payloadLength = query_1_7.payload_length_15_8 << 8 |
			query_1_7.payload_length_7_0;
	m_buildID = query_1_7.bl_fw_id_7_0 |
			query_1_7.bl_fw_id_15_8 << 8 |
			query_1_7.bl_fw_id_23_16 << 16 |
			query_1_7.bl_fw_id_31_24 << 24;

	idStr[0] = m_bootloaderID[0];
	idStr[1] = m_bootloaderID[1];
	idStr[2] = 0;

	m_hasCoreCode = query_1_7.has_core_code;
	m_hasCoreConfig = query_1_7.has_core_config;
	m_hasFlashConfig = query_1_7.has_flash_config;
	m_hasFLD = query_1_7.has_fld;
	m_hasGlobalParameters = query_1_7.has_global_parameters;
	m_hasSBL = query_1_7.has_bootloader;
	m_hasSBLMSL = query_0.f34_query0_b6__7 >> 1;
	m_hasSecurity = query_0.f34_query0_b6__7 & 0x01;

	fprintf(stdout, "F34 has CoreCode: %d\n", m_hasCoreCode);
	fprintf(stdout, "F34 has CoreConfig: %d\n", m_hasCoreConfig);
	fprintf(stdout, "F34 has FlashConfig: %d\n", m_hasFlashConfig);
	fprintf(stdout, "F34 has FLD: %d\n", m_hasFLD);
	fprintf(stdout, "F34 has SecondaryBootloader: %d\n", m_hasSBL);
	fprintf(stdout, "F34 has SBL MSL: %d\n", m_hasSBLMSL);
	fprintf(stdout, "F34 has Security: %d\n", m_hasSecurity);

	fprintf(stdout, "F34 bootloader id: %s (%#04x %#04x)\n", idStr, m_bootloaderID[0],
		m_bootloaderID[1]);
	fprintf(stdout, "F34 has config id: %d\n", m_hasConfigID);
	fprintf(stdout, "F34 unlocked:      %d\n", m_unlocked);
	fprintf(stdout, "F34 block size:    %d\n", m_blockSize);
	fprintf(stdout, "F34 flash cfg leng:%d\n", m_flashConfigLength);
	fprintf(stdout, "F34 payload length:%d\n", m_payloadLength);
	fprintf(stdout, "F34 build id:      %lu\n", m_buildID);

	return ReadFlashConfig();
}

int RMI4Update::ReadF34Queries()
{
	int rc;
	unsigned char idStr[3];
	unsigned char buf[8];
	unsigned short queryAddr = m_f34.GetQueryBase();
	unsigned short f34Version = m_f34.GetFunctionVersion();
	unsigned short querySize;

	if (f34Version == 0x2)
		return ReadF34QueriesV7();
	else if (f34Version == 0x1)
		querySize = 8;
	else
		querySize = 2;

	rc = m_device.Read(queryAddr, m_bootloaderID, RMI_BOOTLOADER_ID_SIZE);
	if (rc != RMI_BOOTLOADER_ID_SIZE)
		return UPDATE_FAIL_READ_BOOTLOADER_ID;

	if (f34Version == 0x1)
		++queryAddr;
	else
		queryAddr += querySize;

	if (f34Version == 0x1) {
		rc = m_device.Read(queryAddr, buf, 1);
		if (rc != 1)
			return UPDATE_FAIL_READ_F34_QUERIES;

		m_hasNewRegmap = buf[0] & RMI_F34_HAS_NEW_REG_MAP;
		m_unlocked = buf[0] & RMI_F34_IS_UNLOCKED;;
		m_hasConfigID = buf[0] & RMI_F34_HAS_CONFIG_ID;

		++queryAddr;

		rc = m_device.Read(queryAddr, buf, 2);
		if (rc != 2)
			return UPDATE_FAIL_READ_F34_QUERIES;

		m_blockSize = extract_short(buf + RMI_F34_BLOCK_SIZE_V1_OFFSET);

		++queryAddr;

		rc = m_device.Read(queryAddr, buf, 8);
		if (rc != 8)
			return UPDATE_FAIL_READ_F34_QUERIES;

		m_fwBlockCount = extract_short(buf + RMI_F34_FW_BLOCKS_V1_OFFSET);
		m_configBlockCount = extract_short(buf + RMI_F34_CONFIG_BLOCKS_V1_OFFSET);
	} else {
		rc = m_device.Read(queryAddr, buf, RMI_F34_QUERY_SIZE);
		if (rc != RMI_F34_QUERY_SIZE)
			return UPDATE_FAIL_READ_F34_QUERIES;

		m_hasNewRegmap = buf[0] & RMI_F34_HAS_NEW_REG_MAP;
		m_unlocked = buf[0] & RMI_F34_IS_UNLOCKED;;
		m_hasConfigID = buf[0] & RMI_F34_HAS_CONFIG_ID;
		m_blockSize = extract_short(buf + RMI_F34_BLOCK_SIZE_OFFSET);
		m_fwBlockCount = extract_short(buf + RMI_F34_FW_BLOCKS_OFFSET);
		m_configBlockCount = extract_short(buf + RMI_F34_CONFIG_BLOCKS_OFFSET);
	}

	idStr[0] = m_bootloaderID[0];
	idStr[1] = m_bootloaderID[1];
	idStr[2] = 0;

	fprintf(stdout, "F34 bootloader id: %s (%#04x %#04x)\n", idStr, m_bootloaderID[0],
		m_bootloaderID[1]);
	fprintf(stdout, "F34 has config id: %d\n", m_hasConfigID);
	fprintf(stdout, "F34 unlocked:      %d\n", m_unlocked);
	fprintf(stdout, "F34 new reg map:   %d\n", m_hasNewRegmap);
	fprintf(stdout, "F34 block size:    %d\n", m_blockSize);
	fprintf(stdout, "F34 fw blocks:     %d\n", m_fwBlockCount);
	fprintf(stdout, "F34 config blocks: %d\n", m_configBlockCount);
	fprintf(stdout, "\n");

	if (f34Version == 0x1)
		m_f34StatusAddr = m_f34.GetDataBase() + 2;
	else
		m_f34StatusAddr = m_f34.GetDataBase() + RMI_F34_BLOCK_DATA_OFFSET + m_blockSize;

	return UPDATE_SUCCESS;
}

int RMI4Update::ReadF34Controls()
{
	int rc;
	unsigned char buf[2];

	if (m_f34.GetFunctionVersion() == 0x1) {
		rc = m_device.Read(m_f34StatusAddr, buf, 2);
		if (rc != 2)
			return UPDATE_FAIL_READ_F34_CONTROLS;

		m_f34Command = buf[0] & RMI_F34_COMMAND_V1_MASK;
		m_f34Status = buf[1] & RMI_F34_STATUS_V1_MASK;
		m_programEnabled = !!(buf[1] & RMI_F34_ENABLED_MASK);

	} else {
		rc = m_device.Read(m_f34StatusAddr, buf, 1);
		if (rc != 1)
			return UPDATE_FAIL_READ_F34_CONTROLS;

		m_f34Command = buf[0] & RMI_F34_COMMAND_MASK;
		m_f34Status = (buf[0] >> RMI_F34_STATUS_SHIFT) & RMI_F34_STATUS_MASK;
		m_programEnabled = !!(buf[0] & RMI_F34_ENABLED_MASK);
	}
	
	return UPDATE_SUCCESS;
}

int RMI4Update::WriteBootloaderID()
{
	int rc;
	int blockDataOffset = RMI_F34_BLOCK_DATA_OFFSET;

	if (m_f34.GetFunctionVersion() == 0x1)
		blockDataOffset = RMI_F34_BLOCK_DATA_V1_OFFSET;

	rc = m_device.Write(m_f34.GetDataBase() + blockDataOffset,
				m_bootloaderID, RMI_BOOTLOADER_ID_SIZE);
	if (rc != RMI_BOOTLOADER_ID_SIZE)
		return UPDATE_FAIL_WRITE_BOOTLOADER_ID;

	return UPDATE_SUCCESS;
}

/*
 * Common function to write a partition in bootloader V7+ protocol.
 *
 * Flags (bitmask):
 *   WPF_SLEEP_BEFORE_WAIT  (0x01) - Sleep 100 ms before WaitForIdle on touchpad
 *   WPF_CHECK_WRITE_PROT   (0x02) - Check WRITE_PROTECTION in poll loop (BL >= V8_7)
 *   WPF_WRITE_SIGNATURE    (0x04) - Write signature after data transfer
 */
int RMI4Update::WritePartitionV7(
	unsigned char partitionId,
	unsigned short blockCount,
	const unsigned char *data,
	enum signature_BLv7 signatureIdx,
	unsigned int flags)
{
	int transaction_count, remain_block;
	int transfer_leng = 0;
	int offset = 0;
	unsigned char trans_leng_buf[2];
	unsigned char cmd_buf[1];
	unsigned char off[2] = {0, 0};
	int i;
	int retry = 0;
	unsigned char *data_temp = NULL;
	int rc;
	unsigned short left_bytes;
	unsigned short write_size;
	unsigned short max_write_size;
	unsigned short dataAddr = m_f34.GetDataBase();

	bool sleepBeforeWait   = (flags & WPF_SLEEP_BEFORE_WAIT) != 0;
	bool checkWriteProt    = (flags & WPF_CHECK_WRITE_PROT) != 0;
	bool writeSignature    = (flags & WPF_WRITE_SIGNATURE) != 0;

	/* calculate the count */
	remain_block = (blockCount % m_payloadLength);
	transaction_count = (blockCount / m_payloadLength);

	if (remain_block > 0)
		transaction_count++;

	/* set partition id for bootloader 7 */
	rc = m_device.Write(dataAddr + 1, &partitionId, sizeof(partitionId));
	if (rc != sizeof(partitionId))
		return UPDATE_FAIL_WRITE_FLASH_COMMAND;

	rc = m_device.Write(dataAddr + 2, off, sizeof(off));
	if (rc != sizeof(off))
		return UPDATE_FAIL_WRITE_INITIAL_ZEROS;

	for (i = 0; i < transaction_count; i++)
	{
		if ((i == (transaction_count -1)) && (remain_block > 0))
			transfer_leng = remain_block;
		else
			transfer_leng = m_payloadLength;

		if ((i == 0) || (transfer_leng == remain_block)) {
			// Set Transfer Length
			trans_leng_buf[0] = (unsigned char)(transfer_leng & 0xFF);
			trans_leng_buf[1] = (unsigned char)((transfer_leng & 0xFF00) >> 8);

			rc = m_device.Write(dataAddr + 3, trans_leng_buf, sizeof(trans_leng_buf));
			if (rc != sizeof(trans_leng_buf))
				return UPDATE_FAIL_WRITE_FLASH_COMMAND;
		}

		// Set Command to Write
		cmd_buf[0] = (unsigned char)CMD_V7_WRITE;
		rc = m_device.Write(dataAddr + 4, cmd_buf, sizeof(cmd_buf));
		if (rc != sizeof(cmd_buf))
			return UPDATE_FAIL_WRITE_FLASH_COMMAND;

		max_write_size = 16;
		if (max_write_size >= transfer_leng * m_blockSize)
			max_write_size = transfer_leng * m_blockSize;
		else if (max_write_size > m_blockSize)
			max_write_size -= max_write_size % m_blockSize;
		else
			max_write_size = m_blockSize;

		left_bytes = transfer_leng * m_blockSize;
		do {
			if (left_bytes / max_write_size)
				write_size = max_write_size;
			else
				write_size = left_bytes;

			data_temp = (unsigned char *) malloc(sizeof(unsigned char) * write_size);
			if (data_temp != NULL) {
				memcpy(data_temp, data + offset, sizeof(char) * write_size);
				rc = m_device.Write(dataAddr + 5, data_temp, sizeof(char) * write_size);
				if (rc != ((ssize_t)sizeof(char) * write_size)) {
					fprintf(stdout, "err write_size = %d; rc = %d\n", write_size, rc);
					return UPDATE_FAIL_READ_F34_QUERIES;
				}

				offset += write_size;
				left_bytes -= write_size;
				free(data_temp);
				data_temp = NULL;
			}
		} while (left_bytes);

		if(m_device.GetDeviceType() == RMI_DEVICE_TYPE_TOUCHPAD)  {
			if (sleepBeforeWait)
				Sleep(100);
			rc = WaitForIdle(RMI_F34_IDLE_WAIT_MS, false);
			if (rc != UPDATE_SUCCESS) {
				fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
				return UPDATE_FAIL_TIMEOUT_WAITING_FOR_ATTN;
			}
		}

		//Wait for completion
		do {
			Sleep(20);
			rmi4update_poll();
			if (checkWriteProt && GetDeviceBootloaderVersion() >= BL_V8_7) {
				if (m_flashStatus == WRITE_PROTECTION)
					return UPDATE_FAIL_WRITE_PROTECTED;
			}
			if (m_flashStatus == SUCCESS){
				break;
			}
			retry++;
		} while(retry < 20);

		if (m_flashStatus != SUCCESS) {
			fprintf(stdout, "err flash_status = %d\n", m_flashStatus);
			return UPDATE_FAIL_WRITE_F01_CONTROL_0;
		}

	}

	if (writeSignature && m_device.GetDeviceType() == RMI_DEVICE_TYPE_TOUCHPAD) {
		if (m_firmwareImage.GetSignatureInfo()[signatureIdx].bExisted) {
			// Write signature.
			rc = WriteSignatureV7(signatureIdx, (unsigned char *)data, offset);
			if (rc != UPDATE_SUCCESS) {
				fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
				return rc;
			}
		}
	}

	return UPDATE_SUCCESS;
}

int RMI4Update::WriteFirmwareV7()
{
	if (GetDeviceBootloaderVersion() >= BL_V10) {
		m_fwBlockCount = m_firmwareImage.GetFirmwareSize() / m_blockSize;
	}

	return WritePartitionV7(
		CORE_CODE_PARTITION,
		m_fwBlockCount,
		m_firmwareImage.GetFirmwareData(),
		BLv7_CORE_CODE,
		WPF_SLEEP_BEFORE_WAIT | WPF_WRITE_SIGNATURE);
}

int RMI4Update::WriteCoreConfigV7()
{
	if (GetDeviceBootloaderVersion() >= BL_V10) {
		m_configBlockCount = m_firmwareImage.GetConfigSize() / m_blockSize;
	}

	return WritePartitionV7(
		CORE_CONFIG_PARTITION,
		m_configBlockCount,
		m_firmwareImage.GetConfigData(),
		BLv7_CORE_CONFIG,
		WPF_WRITE_SIGNATURE);
}

int RMI4Update::WriteFlashConfigV7()
{
	unsigned short blockCount = m_firmwareImage.GetFlashConfigSize() / m_blockSize;

	return WritePartitionV7(
		FLASH_CONFIG_PARTITION,
		blockCount,
		m_firmwareImage.GetFlashConfigData(),
		BLv7_FLASH_CONFIG,
		WPF_CHECK_WRITE_PROT | WPF_WRITE_SIGNATURE);
}

int RMI4Update::WriteFLDV7()
{
	if (GetDeviceBootloaderVersion() < BL_V10) {
		// Not support writing FLD before bootloader v10
		return UPDATE_SUCCESS;
	}

	unsigned short blockCount = m_firmwareImage.GetFLDSize() / m_blockSize;

	return WritePartitionV7(
		FIXED_LOCATION_DATA_PARTITION,
		blockCount,
		m_firmwareImage.GetFLDData(),
		BLv7_FLD,
		WPF_SLEEP_BEFORE_WAIT | WPF_CHECK_WRITE_PROT | WPF_WRITE_SIGNATURE);
}

int RMI4Update::WriteGlobalParametersV7()
{
	unsigned short blockCount = m_firmwareImage.GetGlobalParametersSize() / m_blockSize;

	return WritePartitionV7(
		GLOBAL_PARAMETERS_PARTITION,
		blockCount,
		m_firmwareImage.GetGlobalParametersData(),
		(enum signature_BLv7)0,  /* unused, signature not written */
		WPF_SLEEP_BEFORE_WAIT);
}

int RMI4Update::EraseFlashConfigV10()
{
	unsigned char erase_cmd[8] = {0, 0, 0, 0, 0, 0, 0, 0};
	int retry = 0;
	int rc;

	/* set partition id for bootloader 10 */
	erase_cmd[0] = FLASH_CONFIG_PARTITION;
	/* write bootloader id */
	erase_cmd[6] = m_bootloaderID[0];
	erase_cmd[7] = m_bootloaderID[1];
	erase_cmd[5] = (unsigned char)CMD_V7_ERASE;
	
	fprintf(stdout, "Erase command : ");
	for(int i = 0 ;i<8;i++){
		fprintf(stdout, "%d ", erase_cmd[i]);
	}
	fprintf(stdout, "\n");

	rmi4update_poll();
	if (!m_inBLmode)
		return UPDATE_FAIL_DEVICE_NOT_IN_BOOTLOADER;
	
	// For BL8 device, we need hold 1 seconds after querying
	// F34 status to avoid not get attention by following giving 
	// erase command.
	Sleep(1000);

	rc = m_device.Write(m_f34.GetDataBase() + 1, erase_cmd, sizeof(erase_cmd));
	if (rc != sizeof(erase_cmd))
		return UPDATE_FAIL_WRITE_F01_CONTROL_0;

	Sleep(100);

	//Wait from ATTN
	rc = WaitForIdle(RMI_F34_ERASE_V8_WAIT_MS, false);
	if (rc != UPDATE_SUCCESS) {
		fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
		return UPDATE_FAIL_TIMEOUT_WAITING_FOR_ATTN;
	}

	do {
		Sleep(20);
		rmi4update_poll();
		if (GetDeviceBootloaderVersion() >= BL_V8_7) {
			if (m_flashStatus == WRITE_PROTECTION)
				return UPDATE_FAIL_WRITE_PROTECTED;
		}
		if (m_flashStatus == SUCCESS){
			break;
		}
		retry++;
	} while(retry < 20);

	if (m_flashStatus != SUCCESS) {
		fprintf(stdout, "err flash_status = %d\n", m_flashStatus);
		return UPDATE_FAIL_WRITE_F01_CONTROL_0;
	}

	return UPDATE_SUCCESS;
}

int RMI4Update::EraseCoreCodeV10()
{
	unsigned char erase_cmd[8] = {0, 0, 0, 0, 0, 0, 0, 0};
	int retry = 0;
	int rc;

	/* set partition id for bootloader 10 */
	erase_cmd[0] = CORE_CODE_PARTITION;
	/* write bootloader id */
	erase_cmd[6] = m_bootloaderID[0];
	erase_cmd[7] = m_bootloaderID[1];
	erase_cmd[5] = (unsigned char)CMD_V7_ERASE_AP;
	
	fprintf(stdout, "Erase command : ");
	for(int i = 0 ;i<8;i++){
		fprintf(stdout, "%d ", erase_cmd[i]);
	}
	fprintf(stdout, "\n");

	rmi4update_poll();
	if (!m_inBLmode)
		return UPDATE_FAIL_DEVICE_NOT_IN_BOOTLOADER;
	
	// For BL8 device, we need hold 1 seconds after querying
	// F34 status to avoid not get attention by following giving 
	// erase command.
	Sleep(1000);

	rc = m_device.Write(m_f34.GetDataBase() + 1, erase_cmd, sizeof(erase_cmd));
	if (rc != sizeof(erase_cmd))
		return UPDATE_FAIL_WRITE_F01_CONTROL_0;

	Sleep(100);

	//Wait from ATTN
	rc = WaitForIdle(RMI_F34_ERASE_V8_WAIT_MS, false);
	if (rc != UPDATE_SUCCESS) {
		fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
		return UPDATE_FAIL_TIMEOUT_WAITING_FOR_ATTN;
	}

	do {
		Sleep(20);
		rmi4update_poll();
		if (m_flashStatus == SUCCESS){
			break;
		}
		retry++;
	} while(retry < 20);

	if (m_flashStatus != SUCCESS) {
		fprintf(stdout, "err flash_status = %d\n", m_flashStatus);
		return UPDATE_FAIL_WRITE_F01_CONTROL_0;
	}

	return UPDATE_SUCCESS;
}

int RMI4Update::EraseFirmwareV7()
{
	unsigned char erase_cmd[8] = {0, 0, 0, 0, 0, 0, 0, 0};
	int retry = 0;
	int rc;

	/* set partition id for bootloader 7 */
	erase_cmd[0] = CORE_CODE_PARTITION;
	/* write bootloader id */
	erase_cmd[6] = m_bootloaderID[0];
	erase_cmd[7] = m_bootloaderID[1];
	if(m_bootloaderID[1] == 8){
		/* Set Command to Erase AP for BL8*/
		erase_cmd[5] = (unsigned char)CMD_V7_ERASE_AP;
	} else {
		/* Set Command to Erase AP for BL7*/
		erase_cmd[5] = (unsigned char)CMD_V7_ERASE;
	}
	
	fprintf(stdout, "Erase command : ");
	for(int i = 0 ;i<8;i++){
		fprintf(stdout, "%d ", erase_cmd[i]);
	}
	fprintf(stdout, "\n");

	rmi4update_poll();
	if (!m_inBLmode)
		return UPDATE_FAIL_DEVICE_NOT_IN_BOOTLOADER;
	if(m_bootloaderID[1] == 8){
		// For BL8 device, we need hold 1 seconds after querying
		// F34 status to avoid not get attention by following giving 
		// erase command.
		Sleep(1000);
	}

	rc = m_device.Write(m_f34.GetDataBase() + 1, erase_cmd, sizeof(erase_cmd));
	if (rc != sizeof(erase_cmd))
		return UPDATE_FAIL_WRITE_F01_CONTROL_0;

	Sleep(100);

	//Wait from ATTN
	if(m_bootloaderID[1] == 8){
		if(m_device.GetDeviceType() == RMI_DEVICE_TYPE_TOUCHPAD)  {
			// Wait for attention for BL8 touchpad.
			rc = WaitForIdle(RMI_F34_ERASE_V8_WAIT_MS, false);
			if (rc != UPDATE_SUCCESS) {
				fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
				return UPDATE_FAIL_TIMEOUT_WAITING_FOR_ATTN;
			}
		}
	}
	do {
		Sleep(20);
		rmi4update_poll();
		if (GetDeviceBootloaderVersion() >= BL_V8_7) {
			if (m_flashStatus == WRITE_PROTECTION)
				return UPDATE_FAIL_WRITE_PROTECTED;
		}
		if (m_flashStatus == SUCCESS){
			break;
		}
		retry++;
	} while(retry < 20);

	if (m_flashStatus != SUCCESS) {
		fprintf(stdout, "err flash_status = %d\n", m_flashStatus);
		return UPDATE_FAIL_WRITE_F01_CONTROL_0;
	}
 
	if(m_bootloaderID[1] == 7){
		// For BL7, we need erase config partition.
		fprintf(stdout, "Start to erase config\n");
		erase_cmd[0] = CORE_CONFIG_PARTITION;
		erase_cmd[6] = m_bootloaderID[0];
		erase_cmd[7] = m_bootloaderID[1];
		erase_cmd[5] = (unsigned char)CMD_V7_ERASE;

		Sleep(100);
		rmi4update_poll();
		if (!m_inBLmode)
		  return UPDATE_FAIL_DEVICE_NOT_IN_BOOTLOADER;

		rc = m_device.Write(m_f34.GetDataBase() + 1, erase_cmd, sizeof(erase_cmd));
		if (rc != sizeof(erase_cmd))
			return UPDATE_FAIL_WRITE_F01_CONTROL_0;

		if(m_device.GetDeviceType() == RMI_DEVICE_TYPE_TOUCHPAD)  {
			//Wait from ATTN for touchpad only.
			Sleep(100);

			rc = WaitForIdle(RMI_F34_ERASE_WAIT_MS, true);
			if (rc != UPDATE_SUCCESS) {
				fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
				return UPDATE_FAIL_TIMEOUT_WAITING_FOR_ATTN;
			}
		}


		do {
			Sleep(20);
			rmi4update_poll();
			if (m_flashStatus == SUCCESS){
				break;
			}
			retry++;
		} while(retry < 20);

		if (m_flashStatus != SUCCESS) {
			fprintf(stdout, "err flash_status = %d\n", m_flashStatus);
			return UPDATE_FAIL_WRITE_F01_CONTROL_0;
		}
	}

	return UPDATE_SUCCESS;
}

int RMI4Update::EnterFlashProgrammingV7()
{
	int rc;
	unsigned char f34_status;
	rc = m_device.Read(m_f34.GetDataBase(), &f34_status, sizeof(unsigned char));
	m_inBLmode = f34_status & 0x80;
	if(!m_inBLmode || GetDeviceBootloaderVersion() >= BL_V10_1) {
		fprintf(stdout, "Not in BL mode, going to BL mode...\n");
		unsigned char EnterCmd[8] = {0, 0, 0, 0, 0, 0, 0, 0};
		int retry = 0;

		/* set partition id for bootloader 7 */
		EnterCmd[0] = BOOTLOADER_PARTITION;

		/* write bootloader id */
		EnterCmd[6] = m_bootloaderID[0];
		EnterCmd[7] = m_bootloaderID[1];

		// Set Command to EnterBL
		EnterCmd[5] = (unsigned char)CMD_V7_ENTER_BL;

		rc = m_device.Write(m_f34.GetDataBase() + 1, EnterCmd, sizeof(EnterCmd));
		if (rc != sizeof(EnterCmd))
			return UPDATE_FAIL_WRITE_F01_CONTROL_0;

		if(m_device.GetDeviceType() == RMI_DEVICE_TYPE_TOUCHPAD)  {
			rc = WaitForIdle(RMI_F34_ENABLE_WAIT_MS, false);
			if (rc != UPDATE_SUCCESS) {
				fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
				return UPDATE_FAIL_TIMEOUT_WAITING_FOR_ATTN;
			}
		}

		//Wait from ATTN
		do {
			Sleep(20);
			rmi4update_poll();
			if (m_flashStatus == SUCCESS){
				break;
			}
			retry++;
		} while(retry < 20);

		if (m_flashStatus != SUCCESS) {
			fprintf(stdout, "err flash_status = %d\n", m_flashStatus);
			return UPDATE_FAIL_WRITE_F01_CONTROL_0;
		}

		Sleep(RMI_F34_ENABLE_WAIT_MS);

		fprintf(stdout, "%s\n", __func__);
		rmi4update_poll();
		if (!m_inBLmode)
			return UPDATE_FAIL_DEVICE_NOT_IN_BOOTLOADER;
			
	} else
		fprintf(stdout, "Already in BL mode, skip...\n");

	if(m_device.GetDeviceType() != RMI_DEVICE_TYPE_TOUCHPAD) {
		// workaround for touchscreen only
		fprintf(stdout, "Erase in BL mode\n");
		rc = EraseFirmwareV7();
		if (rc != UPDATE_SUCCESS) {
			fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
			return UPDATE_FAIL_ERASE_ALL;
		}
		fprintf(stdout, "Erase in BL mode end\n");
		m_device.RebindDriver();
	}

	Sleep(RMI_F34_ENABLE_WAIT_MS);

	if(m_device.GetDeviceType() == RMI_DEVICE_TYPE_TOUCHPAD) {
		m_device.SetMode(m_device.GetDesiredMode());
	}

	rc = FindUpdateFunctions();
	if (rc != UPDATE_SUCCESS)
		return rc;

	rc = ReadF34Queries();
	if (rc != UPDATE_SUCCESS)
		return rc;

	return UPDATE_SUCCESS;
}

int RMI4Update::EnterFlashProgramming()
{
	int rc;
	unsigned char f01Control_0;
	const unsigned char enableProg = RMI_F34_ENABLE_FLASH_PROG;

	rc = WriteBootloaderID();
	if (rc != UPDATE_SUCCESS)
		return rc;

	fprintf(stdout, "Enabling flash programming.\n");
	rc = m_device.Write(m_f34StatusAddr, &enableProg, 1);
	if (rc != 1)
		return UPDATE_FAIL_ENABLE_FLASH_PROGRAMMING;

	
	if(m_device.GetDeviceType() != RMI_DEVICE_TYPE_TOUCHPAD) {
		fprintf(stdout, "not TouchPad, rebind driver here\n");
		Sleep(RMI_F34_ENABLE_WAIT_MS);
		m_device.RebindDriver();
		rc = WaitForIdle(0);
		if (rc != UPDATE_SUCCESS)
			return UPDATE_FAIL_NOT_IN_IDLE_STATE;
	} else {
		// For TouchPad
		rc = WaitForIdle(RMI_F34_ENABLE_WAIT_MS);
		if (rc != UPDATE_SUCCESS)
			return UPDATE_FAIL_NOT_IN_IDLE_STATE;
	}

	if (!m_programEnabled)
		return UPDATE_FAIL_PROGRAMMING_NOT_ENABLED;

	fprintf(stdout, "Programming is enabled.\n");

	rc = FindUpdateFunctions();
	if (rc != UPDATE_SUCCESS)
		return rc;

	rc = m_device.Read(m_f01.GetDataBase(), &m_deviceStatus, 1);
	if (rc != 1)
		return UPDATE_FAIL_READ_DEVICE_STATUS;

	if(m_f34.GetFunctionVersion() > 0x1){
		if (!RMI_F01_STATUS_BOOTLOADER_v7(m_deviceStatus))
			return UPDATE_FAIL_DEVICE_NOT_IN_BOOTLOADER;
		fprintf(stdout, "Already in BL mode V7\n");
	} else {
		if (!RMI_F01_STATUS_BOOTLOADER(m_deviceStatus))
			return UPDATE_FAIL_DEVICE_NOT_IN_BOOTLOADER;
		fprintf(stdout, "Already in BL mode\n");
	}

	rc = ReadF34Queries();
	if (rc != UPDATE_SUCCESS)
		return rc;

	rc = m_device.Read(m_f01.GetControlBase(), &f01Control_0, 1);
	if (rc != 1)
		return UPDATE_FAIL_READ_F01_CONTROL_0;

	f01Control_0 |= RMI_F01_CRTL0_NOSLEEP_BIT;
	f01Control_0 = (f01Control_0 & ~RMI_F01_CTRL0_SLEEP_MODE_MASK) | RMI_SLEEP_MODE_NORMAL;

	rc = m_device.Write(m_f01.GetControlBase(), &f01Control_0, 1);
	if (rc != 1)
		return UPDATE_FAIL_WRITE_F01_CONTROL_0;

	return UPDATE_SUCCESS;
}

int RMI4Update::WriteBlocks(unsigned char *block, unsigned short count, unsigned char cmd)
{
	int blockNum;
	unsigned char zeros[] = { 0, 0 };
	int rc;
	unsigned short addr;
	unsigned char *blockWithCmd = (unsigned char *)alloca(m_blockSize + 1);

	if (m_f34.GetFunctionVersion() == 0x1)
		addr = m_f34.GetDataBase() + RMI_F34_BLOCK_DATA_V1_OFFSET;
	else
		addr = m_f34.GetDataBase() + RMI_F34_BLOCK_DATA_OFFSET;

	rc = m_device.Write(m_f34.GetDataBase(), zeros, 2);
	if (rc != 2)
		return UPDATE_FAIL_WRITE_INITIAL_ZEROS;

	for (blockNum = 0; blockNum < count; ++blockNum) {
		if (m_writeBlockWithCmd) {
			memcpy(blockWithCmd, block, m_blockSize);
			blockWithCmd[m_blockSize] = cmd;

			rc = m_device.Write(addr, blockWithCmd, m_blockSize + 1);
			if (rc != m_blockSize + 1) {
				fprintf(stderr, "failed to write block %d\n", blockNum);
				return UPDATE_FAIL_WRITE_BLOCK;
			}
		} else {
			rc = m_device.Write(addr, block, m_blockSize);
			if (rc != m_blockSize) {
				fprintf(stderr, "failed to write block %d\n", blockNum);
				return UPDATE_FAIL_WRITE_BLOCK;
			}

			rc = m_device.Write(m_f34StatusAddr, &cmd, 1);
			if (rc != 1) {
				fprintf(stderr, "failed to write command for block %d\n", blockNum);
				return UPDATE_FAIL_WRITE_FLASH_COMMAND;
			}
		}

		rc = WaitForIdle(RMI_F34_IDLE_WAIT_MS, !m_writeBlockWithCmd);
		if (rc != UPDATE_SUCCESS) {
			fprintf(stderr, "failed to go into idle after writing block %d\n", blockNum);
			return UPDATE_FAIL_NOT_IN_IDLE_STATE;
		}

		block += m_blockSize;
	}

	return UPDATE_SUCCESS;
}

int RMI4Update::WriteSignatureV7(enum signature_BLv7 signature_partition, unsigned char* data, int offset)
{
	fprintf(stdout, "Write Signature...\n");
	int rc;
	unsigned char off[2] = {0, 0};
	unsigned char cmd_buf[1];
	unsigned short dataAddr = m_f34.GetDataBase();
	int transfer_leng = 0;
	signature_info signature = m_firmwareImage.GetSignatureInfo()[signature_partition];
	unsigned char trans_leng_buf[2];
	unsigned short left_bytes;
	unsigned short write_size;
	unsigned short max_write_size;
	unsigned char *data_temp = NULL;
	int retry = 0;
	rc = m_device.Write(dataAddr + 2, off, sizeof(off));
	if (rc != sizeof(off))
		return UPDATE_FAIL_WRITE_INITIAL_ZEROS;

	// Set Transfer Length
	transfer_leng = signature.size / m_blockSize;
	trans_leng_buf[0] = (unsigned char)(transfer_leng & 0xFF);
	trans_leng_buf[1] = (unsigned char)((transfer_leng & 0xFF00) >> 8);

	rc = m_device.Write(dataAddr + 3, trans_leng_buf, sizeof(trans_leng_buf));
	if (rc != sizeof(trans_leng_buf))
		return UPDATE_FAIL_WRITE_FLASH_COMMAND;

	// Set Command to Signature
	cmd_buf[0] = (unsigned char)CMD_V7_SIGNATURE;
	rc = m_device.Write(dataAddr + 4, cmd_buf, sizeof(cmd_buf));
	if (rc != sizeof(cmd_buf))
		return UPDATE_FAIL_WRITE_FLASH_COMMAND;

	max_write_size = 16;
	if (max_write_size >= transfer_leng * m_blockSize)
		max_write_size = transfer_leng * m_blockSize;
	else if (max_write_size > m_blockSize)
		max_write_size -= max_write_size % m_blockSize;
	else
		max_write_size = m_blockSize;

	left_bytes = transfer_leng * m_blockSize;

	do {
		if (left_bytes / max_write_size)
			write_size = max_write_size;
		else
			write_size = left_bytes;

		data_temp = (unsigned char *) malloc(sizeof(unsigned char) * write_size);
		if (data_temp != NULL) {
			memcpy(data_temp, data + offset, sizeof(char) * write_size);
			rc = m_device.Write(dataAddr + 5, data_temp, sizeof(char) * write_size);
			if (rc != ((ssize_t)sizeof(char) * write_size)) {
				fprintf(stdout, "err write_size = %d; rc = %d\n", write_size, rc);
				return UPDATE_FAIL_WRITE_BLOCK;
			}

			offset += write_size;
			left_bytes -= write_size;
			free(data_temp);
			data_temp = NULL;
		}
	} while (left_bytes);

	// Wair for attention for touchpad only.
	rc = WaitForIdle(RMI_F34_IDLE_WAIT_MS, false);
	if (rc != UPDATE_SUCCESS) {
		fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
		return UPDATE_FAIL_TIMEOUT_WAITING_FOR_ATTN;
	}
	
	//Wait for completion
	do {
		Sleep(20);
		rmi4update_poll();
		if (m_flashStatus == SUCCESS){
			break;
		}
		retry++;
	} while(retry < 20);

	if (m_flashStatus != SUCCESS) {
		fprintf(stdout, "err flash_status = %d\n", m_flashStatus);
		return UPDATE_FAIL_WRITE_F01_CONTROL_0;
	}
	return UPDATE_SUCCESS;
}

/*
 * This is a limited implementation of WaitForIdle which assumes WaitForAttention is supported
 * this will be true for HID, but other protocols will need to revert polling. Polling
 * is not implemented yet.
 */
int RMI4Update::WaitForIdle(int timeout_ms, bool readF34OnSucess)
{
	int rc = 0;
	struct timeval tv;

	if (timeout_ms > 0) {
		tv.tv_sec = timeout_ms / 1000;
		tv.tv_usec = (timeout_ms % 1000) * 1000;

		// For touchpad case, we cannot receive the ATTN with RMI function ID inclded.
		// Skip the interrupt mask when waiting for ATTN report to make sure we can receive the ATTN report.
		bool skipInterruptMask = (m_device.GetDeviceType() == RMI_DEVICE_TYPE_TOUCHPAD) ? true : false;

		rc = m_device.WaitForAttention(&tv, skipInterruptMask ? 0 : m_f34.GetInterruptMask());
		if (rc == -ETIMEDOUT){
			/*
			 * If for some reason we are not getting attention reports for HID devices
			 * then we can still continue after the timeout and read F34 status
			 * but if we have to wait for the timeout to ellapse everytime then this
			 * will be slow. If this message shows up a lot then something is wrong
			 * with receiving attention reports and that should be fixed.
			 */
			fprintf(stderr, "RMI4Update::WaitForIdle Timed out waiting for attn report\n");
		}
	}

	if (rc <= 0 || readF34OnSucess) {
		rc = ReadF34Controls();
		if (rc != UPDATE_SUCCESS)
			return rc;

		if (!m_f34Status && !m_f34Command) {
			if (!m_programEnabled) {
				fprintf(stderr, "RMI4Update::WaitForIdle Bootloader is idle but program_enabled bit isn't set.\n");
				return UPDATE_FAIL_PROGRAMMING_NOT_ENABLED;
			} else {
				return UPDATE_SUCCESS;
			}
		}
		fprintf(stderr, "RMI4Update::WaitForIdle\n");
		fprintf(stderr, "  ERROR: Waiting for idle status.\n");
		fprintf(stderr, "  Command: %#04x\n", m_f34Command);
		fprintf(stderr, "  Status:  %#04x\n", m_f34Status);
		fprintf(stderr, "  Enabled: %d\n", m_programEnabled);
		fprintf(stderr, "  Idle:    %d\n", !m_f34Command && !m_f34Status);

		return UPDATE_FAIL_NOT_IN_IDLE_STATE;
	}

	return UPDATE_SUCCESS;
}

int RMI4Update::GetDeviceBootloaderVersion()
{
	if (m_f34.GetFunctionVersion() == 0x02) {
		return m_bootloaderID[1] * 10 + m_bootloaderID[0];
	} else 
		return BL_V7Before;
}

int RMI4Update::ReadMSL()
{
	int rc;
	unsigned short query9Addr = m_f34.GetQueryBase() + 9;
	unsigned char MSL[2];

	rc = m_device.Read(query9Addr, MSL, sizeof(MSL));
	if (rc != sizeof(MSL))
		return UPDATE_FAIL_READ_F34_QUERIES;

	m_MSL = MSL[1] << 8 | MSL[0];
	return UPDATE_SUCCESS;
}

int RMI4Update::ReadSBLMSL()
{
	int rc;
	unsigned short querySBLMSLAddr = m_f34.GetQueryBase() + (m_hasSecurity ? 10 : 8);
	unsigned char sblMSL[2];

	rc = m_device.Read(querySBLMSLAddr, sblMSL, sizeof(sblMSL));
	if (rc != sizeof(sblMSL))
		return UPDATE_FAIL_READ_F34_QUERIES;

	m_sblMSL = sblMSL[1] << 8 | sblMSL[0];
	return UPDATE_SUCCESS;
}

int RMI4Update::WriteSBLV10_1()
{
	if (GetDeviceBootloaderVersion() < BL_V10_1) {
		// Not support writing SBL before bootloader v10.1
		return UPDATE_SUCCESS;
	}

	unsigned short blockCount = m_firmwareImage.GetSBLSize() / m_blockSize;

	return WritePartitionV7(
		BOOTLOADER_PARTITION,
		blockCount,
		m_firmwareImage.GetSBLData(),
		BLv7_SBL,
		WPF_SLEEP_BEFORE_WAIT | WPF_CHECK_WRITE_PROT | WPF_WRITE_SIGNATURE);
}

int RMI4Update::EraseSBLV10_1()
{
	unsigned char erase_cmd[8] = {0, 0, 0, 0, 0, 0, 0, 0};
	int retry = 0;
	int rc;

	/* set partition id for bootloader 10 */
	erase_cmd[0] = BOOTLOADER_PARTITION;
	/* write bootloader id */
	erase_cmd[6] = m_bootloaderID[0];
	erase_cmd[7] = m_bootloaderID[1];
	erase_cmd[5] = (unsigned char)CMD_V7_ERASE;
	
	fprintf(stdout, "Erase command : ");
	for(int i = 0 ;i<8;i++){
		fprintf(stdout, "%d ", erase_cmd[i]);
	}
	fprintf(stdout, "\n");

	rmi4update_poll();
	if (!m_inBLmode)
		return UPDATE_FAIL_DEVICE_NOT_IN_BOOTLOADER;
	
	// For BL8 device, we need hold 1 seconds after querying
	// F34 status to avoid not get attention by following giving 
	// erase command.
	Sleep(1000);

	rc = m_device.Write(m_f34.GetDataBase() + 1, erase_cmd, sizeof(erase_cmd));
	if (rc != sizeof(erase_cmd))
		return UPDATE_FAIL_WRITE_F01_CONTROL_0;

	Sleep(100);

	//Wait from ATTN
	rc = WaitForIdle(RMI_F34_ERASE_V8_WAIT_MS, false);
	if (rc != UPDATE_SUCCESS) {
		fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
		return UPDATE_FAIL_TIMEOUT_WAITING_FOR_ATTN;
	}

	do {
		Sleep(20);
		rmi4update_poll();
		if (GetDeviceBootloaderVersion() >= BL_V8_7) {
			if (m_flashStatus == WRITE_PROTECTION)
				return UPDATE_FAIL_WRITE_PROTECTED;
		}
		if (m_flashStatus == SUCCESS){
			break;
		}
		retry++;
	} while(retry < 20);

	if (m_flashStatus != SUCCESS) {
		fprintf(stdout, "err flash_status = %d\n", m_flashStatus);
		return UPDATE_FAIL_WRITE_F01_CONTROL_0;
	}

	return UPDATE_SUCCESS;
}

int RMI4Update::EnterSBLModeV10_1()
{
	int rc;
	if(GetDeviceBootloaderVersion() >= BL_V10_1) {
		fprintf(stdout, "Going to SBL mode...\n");
		unsigned char EnterCmd[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
		int retry = 0;

		/* set partition id for bootloader 7 */
		EnterCmd[0] = BOOTLOADER_PARTITION;

		/* write bootloader id */
		EnterCmd[6] = m_bootloaderID[0];
		EnterCmd[7] = m_bootloaderID[1];
		EnterCmd[8] = 1;

		// Set Command to EnterBL
		EnterCmd[5] = (unsigned char)CMD_V7_ENTER_BL;

		rc = m_device.Write(m_f34.GetDataBase() + 1, EnterCmd, sizeof(EnterCmd));
		if (rc != sizeof(EnterCmd))
			return UPDATE_FAIL_WRITE_F01_CONTROL_0;

		if(m_device.GetDeviceType() == RMI_DEVICE_TYPE_TOUCHPAD)  {
			rc = WaitForIdle(RMI_F34_ENABLE_WAIT_MS, false);
			if (rc != UPDATE_SUCCESS) {
				fprintf(stderr, "%s: %s\n", __func__, update_err_to_string(rc));
				return UPDATE_FAIL_TIMEOUT_WAITING_FOR_ATTN;
			}
		}

		Sleep(RMI_F34_ENABLE_SBL_WAIT_MS);

		//Wait from ATTN
		do {
			Sleep(20);
			rmi4update_poll();
			if (m_flashStatus == SUCCESS){
				break;
			}
			retry++;
		} while(retry < 20);

		if (m_flashStatus != SUCCESS) {
			fprintf(stdout, "err flash_status = %d\n", m_flashStatus);
			return UPDATE_FAIL_WRITE_F01_CONTROL_0;
		}

		Sleep(RMI_F34_ENABLE_WAIT_MS);

		fprintf(stdout, "%s\n", __func__);
		if (!m_inBLmode)
			return UPDATE_FAIL_DEVICE_NOT_IN_BOOTLOADER;

		if(m_device.GetDeviceType() == RMI_DEVICE_TYPE_TOUCHPAD) {
			m_device.SetMode(m_device.GetDesiredMode());
		}
			
	} 
	Sleep(RMI_F34_ENABLE_WAIT_MS);

	rc = FindUpdateFunctions();
	if (rc != UPDATE_SUCCESS)
		return rc;

	rc = ReadF34Queries();
	if (rc != UPDATE_SUCCESS)
		return rc;

	return UPDATE_SUCCESS;
}
