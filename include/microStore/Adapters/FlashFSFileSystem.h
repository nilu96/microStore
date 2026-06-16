/*
 * Copyright (c) 2026 Chad Attermann
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */

#pragma once


#include "../File.h"
#include "../FileSystem.h"
#include "../Log.h"

#include <Cached_SPIFlash.h>
#include <FlashFileSystem.h>

namespace microStore { namespace Adapters {

// Flash definition structure for RAK15001
// Settings for the Gigadevice GD25Q16C 2MiB SPI flash.
// Datasheet: http://www.gigadevice.com/datasheet/gd25q16c/
#define RAK15001							\
{											\
	.total_size = (1UL << 21),				\
	.start_up_time_us = 5000,				\
	.manufacturer_id = 0xc8,				\
	.memory_type = 0x40,					\
	.capacity = 0x15,						\
	.max_clock_speed_mhz = 15,				\
	.quad_enable_bit_mask = 0x00,			\
	.has_sector_protection = false,			\
	.supports_fast_read = true,				\
	.supports_qspi = false,					\
	.supports_qspi_writes = false,			\
	.write_status_register_split = false,	\
	.single_status_byte = true,				\
	.is_fram = false,						\
}

// Flash definition structure for RAK15007
// https://www.infineon.com/assets/row/public/documents/10/49/infineon-cy15b108qn-cy15v108qn-8mb-excelon-lp-ferroelectric-ram-f-ram-serial-spi-1024k-8-40-mhz-industrial-datasheet-en.pdf
// RDID has continuation code: 7F-C2-2E-00
#define RAK15007							\
{											\
	.total_size = 1024UL * 1024,			\
	.start_up_time_us = 5000,				\
	.manufacturer_id = 0x7F,				\
	.memory_type = 0x2e,					\
	.capacity = 0x05,						\
	.max_clock_speed_mhz = 40,				\
	.quad_enable_bit_mask = 0x00,			\
	.has_sector_protection = false,			\
	.supports_fast_read = true,				\
	.supports_qspi = false,					\
	.supports_qspi_writes = true,			\
	.write_status_register_split = false,	\
	.single_status_byte = true,				\
	.is_fram = true,						\
}


class FlashFSFileSystem : public microStore::FileSystem {

public:
	FlashFSFileSystem(const SPIFlash_Device_t* device, uint8_t ss = SS) : microStore::FileSystem(new FileSystemImpl(device, ss)) {}
    virtual ~FlashFSFileSystem() {}

    // Disable heap allocation
    void* operator new(std::size_t) = delete;
    void* operator new[](std::size_t) = delete;
    void* operator new(std::size_t, void*) = delete;

protected:

	class FileImpl : public microStore::FileImpl {

	private:
		std::unique_ptr<Adafruit_SPIFlash_LittleFS::File> _file;
		bool _closed = false;

	public:
		FileImpl(Adafruit_SPIFlash_LittleFS::File* file) : microStore::FileImpl(), _file(file) {}
		virtual ~FileImpl() { if (!_closed) close(); }

	public:
		inline virtual const char* name() const { return _file->name(); }
		inline virtual size_t size() const { return _file->size(); }
		inline virtual void close() { _file->close(); _closed = true; }

		inline virtual int read() { return _file->read(); }
		inline virtual size_t write(uint8_t ch) { return _file->write(ch); }
		inline virtual size_t read(uint8_t* buffer, size_t size) { return _file->read((uint8_t*)buffer, size); }
		inline virtual size_t write(const uint8_t* buffer, size_t size) { return _file->write(buffer, size); }

		inline virtual int available() { return _file->available(); }
		inline virtual int peek() { return _file->peek(); }
		inline virtual size_t tell() { return _file->position(); }
		inline virtual long seek(uint32_t pos, microStore::SeekMode mode) {
			uint8_t smode;
			switch (mode) {
				case microStore::SeekMode::SeekModeCur:
					smode = Adafruit_SPIFlash_LittleFS::SEEK_O_CUR;
					break;
				case microStore::SeekMode::SeekModeEnd:
					smode = Adafruit_SPIFlash_LittleFS::SEEK_O_END;
					break;
				case microStore::SeekMode::SeekModeSet:
				default:
					smode = Adafruit_SPIFlash_LittleFS::SEEK_O_SET;
					break;
			}
			return _file->seek(pos, smode);
		}
		inline virtual void flush() { _file->flush(); }

		inline virtual bool isValid() const { if (!_file) return false; return !_closed; }

	};

	class FileSystemImpl : public microStore::FileSystemImpl {

	public:
		#if defined(EXTERNAL_FLASH_USE_QSPI)
		FileSystemImpl(const SPIFlash_Device_t* device, uint8_t ss = SS) :
			_device(device),
			_transportPtr(new (&_transport.qspi) Adafruit_FlashTransport_QSPI()),
			_flash(_transportPtr) {
				// This is a hack to work around the fact that Adafruit_FlashTransport_QSPI doesn't configure the pins for QSPI mode
				// which causes issues on some platforms (nrf52).
				#if PLATFORM == PLATFORM_NRF52
				nrf_gpio_cfg(
					qspiPin(PIN_QSPI_SCK), 
					NRF_GPIO_PIN_DIR_OUTPUT, 
					NRF_GPIO_PIN_INPUT_DISCONNECT, 
					NRF_GPIO_PIN_NOPULL, 
					NRF_GPIO_PIN_H0H1,  // High Drive
					NRF_GPIO_PIN_NOSENSE
				);

				nrf_gpio_cfg(
					qspiPin(PIN_QSPI_CS), 
					NRF_GPIO_PIN_DIR_OUTPUT, 
					NRF_GPIO_PIN_INPUT_DISCONNECT, 
					NRF_GPIO_PIN_NOPULL, 
					NRF_GPIO_PIN_H0H1,  // High Drive
					NRF_GPIO_PIN_NOSENSE
				);
				#endif
			}
		virtual ~FileSystemImpl() {
			_transport.qspi.~Adafruit_FlashTransport_QSPI();
		}
		#else
		FileSystemImpl(const SPIFlash_Device_t* device, uint8_t ss = SS) :
			_device(device),
			_transportPtr(new (&_transport.spi) Adafruit_FlashTransport_SPI(ss, SPI)),
			_flash(_transportPtr) {}
	    virtual ~FileSystemImpl() {
			_transport.qspi.~Adafruit_FlashTransport_SPI();
		}
		#endif

	public:

		virtual bool format() override {
			USTORE_LOG("[ustore] Formatting FlashFSFileSystem\n");
			if (!FlashFS.format()) {
				USTORE_LOG("[ustore] Failed to format FlashFSFileSystem!\n");
				return false;
			}
			return true;
		}

		virtual bool init(bool reformatOnFail = true) override {
			USTORE_LOG("[ustore] Initializing FlashFSFileSystem\n");
			// Initialize FlashFSFileSystem
			if (!_flash.begin(_device)) {
				USTORE_LOG("[ustore] ERROR: Failed to initialize device for FlashFSFileSystem!\n");
				return false;
			}
			if (!FlashFS.begin(&_flash)) {
				USTORE_LOG("[ustore] ERROR: Failed to initialize FlashFSFileSystem!\n");
				return false;
			}
			if (reformatOnFail) {
				// Ensure filesystem is writable and reformat if not
				bool verified = false;
				microStore::File init_test = open("./__init_test__", microStore::File::ModeWrite, true);
				if (init_test) {
					if (init_test.write("test", 4) == 4) {
						verified = true;
					}
					init_test.close();
				}
				if (!verified) {
					USTORE_LOG("[ustore] WARNING: FlashFSFileSystem check failed, reformatting!\n");
					format();
				}
				else {
					remove("./__init_test__");
					USTORE_LOG("[ustore] FlashFSFileSystem check passed!\n");
				}
			}
			return true;
		}


		virtual microStore::File open(const char* path, microStore::File::Mode mode, const bool create = false) override {
			int pmode;
			switch (mode) {
				// Read only. File must exist. ("r")
				case microStore::File::ModeRead:
					pmode = Adafruit_SPIFlash_LittleFS::FILE_O_READ;
					break;
				// Write only. Creates file or truncates existing file. ("w")
				case microStore::File::ModeWrite:
					pmode = Adafruit_SPIFlash_LittleFS::FILE_O_WRITE;
					// CBA TODO Replace remove with working truncation
					if (FlashFS.exists(path)) {
						FlashFS.remove(path);
					}
					break;
				// Append only. Creates file if it doesn’t exist. Writes go to end. ("a")
				case microStore::File::ModeAppend:
					// CBA Append is the default write mode for nordicnrf52 LittleFS
					pmode = Adafruit_SPIFlash_LittleFS::FILE_O_WRITE;
					break;
				// Read and write. Creates file or truncates existing file. ("w+")
				case microStore::File::ModeReadWrite:
					pmode = Adafruit_SPIFlash_LittleFS::FILE_O_READ | Adafruit_SPIFlash_LittleFS::FILE_O_WRITE;
					// CBA TODO Replace remove with working truncation
					if (FlashFS.exists(path)) {
						FlashFS.remove(path);
					}
					break;
				// Read and append. Creates file if it doesn’t exist. ("a+")
				case microStore::File::ModeReadAppend:
					// CBA Append is the default write mode for nordicnrf52 LittleFS
					pmode = Adafruit_SPIFlash_LittleFS::FILE_O_READ | Adafruit_SPIFlash_LittleFS::FILE_O_WRITE;
					break;
				// Read and write. File must exist. ("r+") ???
				default:
					return {};
			}
			Adafruit_SPIFlash_LittleFS::File* file = new Adafruit_SPIFlash_LittleFS::File(FlashFS);
			if (!file->open(path, pmode)) {
				return {};
			}
			// Seek to beginning to overwrite (this is failing on nrf52)
			//if (mode == microStore::File::ModeWrite) {
			//	file->seek(0);
			//	file->truncate(0);
			//}
			return microStore::File(new FileImpl(file));
		}


		virtual bool exists(const char* path) override {
			return FlashFS.exists(path);
		}

		virtual bool remove(const char* path) override {
			return FlashFS.remove(path);
		}

		virtual bool rename(const char* from_path, const char* to_path) override {
			return FlashFS.rename(from_path, to_path);
		}

		virtual bool mkdir(const char* path) override {
			if (!FlashFS.mkdir(path)) {
				return false;
			}
			return true;
		}

		virtual bool rmdir(const char* path) override {
			if (!FlashFS.rmdir_r(path)) {
				return false;
			}
			return true;
		}


		virtual bool isDirectory(const char* path) override {
			Adafruit_SPIFlash_LittleFS::File file(FlashFS);
			if (file.open(path, Adafruit_SPIFlash_LittleFS::FILE_O_READ)) {
				bool is_directory = file.isDirectory();
				file.close();
				return is_directory;
			}
			return false;
		}

		virtual std::list<std::string> listDirectory(const char* path, Callbacks::DirectoryListing callback = nullptr) override {
			std::list<std::string> files;
			Adafruit_SPIFlash_LittleFS::File root = FlashFS.open(path);
			if (!root) {
				return files;
			}
			Adafruit_SPIFlash_LittleFS::File file = root.openNextFile();
			while (file) {
				if (!file.isDirectory()) {
					char* name = (char*)file.name();
					if (callback) callback(name);
					else files.push_back(name);
				}
				// CBA Following close required to avoid leaking memory
				file.close();
				file = root.openNextFile();
			}
			root.close();
			return files;
		}

		virtual size_t storageSize() override {
			return FlashFS.totalBytes();
		}

		virtual size_t storageAvailable() override {
			return (FlashFS.totalBytes() - FlashFS.usedBytes());
		}

	private:
		union TransportStorage {
			TransportStorage() {}
			~TransportStorage() {}

			Adafruit_FlashTransport_SPI spi;
			Adafruit_FlashTransport_QSPI qspi;
		};

		inline uint8_t qspiPin(uint8_t arduinoPin) {
			return static_cast<uint8_t>(digitalPinToPinName(arduinoPin));
		}

		const SPIFlash_Device_t* _device = nullptr;
		TransportStorage _transport;
		Adafruit_FlashTransport* _transportPtr = nullptr;
		Cached_SPIFlash _flash;
	};

};

} }
