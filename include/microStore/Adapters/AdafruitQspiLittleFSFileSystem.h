/* AdafruitQspiLittleFSFileSystem.h
 * Adapter for Adafruit_LittleFS using nrfx_qspi, fully encapsulated.
 */

#pragma once

#include "../File.h"
#include "../FileSystem.h"

#include <Arduino.h>
#include <Adafruit_LittleFS.h>
#include <Adafruit_LittleFS_File.h>
#include <memory>
#include <fcntl.h> // Provides O_RDWR, O_CREAT, O_APPEND, etc.

extern "C" {
#include "nrfx_qspi.h"
}

namespace microStore { namespace Adapters {

class AdafruitQspiLittleFSFileSystem : public microStore::FileSystem {

public:
    // The constructor takes ONLY the QSPI pins and an optional basepath
    AdafruitQspiLittleFSFileSystem(uint8_t sck, uint8_t cs, uint8_t io0, 
                                   uint8_t io1, uint8_t io2, uint8_t io3, 
                                   const char* basepath = "") 
        : microStore::FileSystem(new FileSystemImpl(sck, cs, io0, io1, io2, io3, basepath)) {}
    
    virtual ~AdafruitQspiLittleFSFileSystem() {}

    // Disable heap allocation
    void* operator new(std::size_t) = delete;
    void* operator new[](std::size_t) = delete;
    void* operator new(std::size_t, void*) = delete;

protected:

    class FileImpl : public microStore::FileImpl {

    private:
        std::unique_ptr<Adafruit_LittleFS_Namespace::File> _file;
        bool _closed = false;

    public:
        FileImpl(Adafruit_LittleFS_Namespace::File* file) : microStore::FileImpl(), _file(file) {}
        virtual ~FileImpl() { if (!_closed) close(); }

    public:
        inline virtual const char* name() const override { return _file->name(); }
        inline virtual size_t size() const override { return _file->size(); }
        inline virtual void close() override { _file->close(); _closed = true; }

        inline virtual int read() override { return _file->read(); }
        inline virtual size_t write(uint8_t ch) override { return _file->write(ch); }
        inline virtual size_t read(uint8_t* buffer, size_t size) override { return _file->read(buffer, size); }
        inline virtual size_t write(const uint8_t* buffer, size_t size) override { return _file->write(buffer, size); }

        inline virtual int available() override { return _file->available(); }
        inline virtual int peek() override { return _file->peek(); }
        inline virtual size_t tell() override { return _file->position(); }
        
        inline virtual long seek(uint32_t pos, microStore::SeekMode mode) override {
            uint32_t targetPos = pos;
            
            switch (mode) {
                case microStore::SeekMode::SeekModeCur:
                    targetPos = _file->position() + pos;
                    break;
                case microStore::SeekMode::SeekModeEnd:
                    // In case the caller passes a bit-casted negative offset, 
                    // int32_t cast ensures we subtract from the end size properly.
                    targetPos = _file->size() + (int32_t)pos; 
                    break;
                case microStore::SeekMode::SeekModeSet:
                default:
                    targetPos = pos;
                    break;
            }
            
            // Adafruit's seek returns a bool. The ESP32 adapter implicitly 
            // returned 1 (true) or 0 (false) cast to long. 
            return _file->seek(targetPos) ? 1 : 0;
        }
        
        inline virtual void flush() override { _file->flush(); }
        inline virtual bool isValid() const override { if (!_file || !(*_file)) return false; return !_closed; }
    };


    class FileSystemImpl : public microStore::FileSystemImpl {

    private:
        Adafruit_LittleFS _extFS;
        struct lfs_config _config;
        const char* _basepath;
        
        // QSPI Pins
        uint8_t _sck, _cs, _io0, _io1, _io2, _io3;

        // Hardware constants matching the W25Q128
        static constexpr uint32_t FLASH_SIZE_BYTES = 16UL * 1024UL * 1024UL; 
        static constexpr uint32_t FLASH_SECTOR_SIZE = 4096;
        static constexpr uint32_t FLASH_PAGE_SIZE = 256;
        static constexpr uint32_t LITTLEFS_BLOCK_COUNT = FLASH_SIZE_BYTES / FLASH_SECTOR_SIZE;
        static constexpr uint8_t CMD_READ_STATUS_1 = 0x05;
        static constexpr uint8_t CMD_READ_STATUS_2 = 0x35;
        static constexpr uint8_t CMD_WRITE_ENABLE = 0x06;
        static constexpr uint8_t CMD_WRITE_STATUS_2 = 0x31;
        static constexpr uint8_t STATUS_1_BUSY = 0x01;
        static constexpr uint8_t STATUS_2_QE = 0x02;

        // Hardware buffers (aligned for DMA/QSPI requirements)
        uint8_t _lfsReadBuffer[FLASH_PAGE_SIZE]      __attribute__((aligned(4)));
        uint8_t _lfsProgBuffer[FLASH_PAGE_SIZE]      __attribute__((aligned(4)));
        uint8_t _lfsFileBuffer[FLASH_PAGE_SIZE]      __attribute__((aligned(4)));
        uint8_t _lfsLookaheadBuffer[128]             __attribute__((aligned(4)));
        uint8_t _qspiScratch[FLASH_PAGE_SIZE]        __attribute__((aligned(4)));

        // --- Hardware Helper Methods ---

        void preconditionQspiPins() {
            // 1. Set High Drive Strength for high-speed signal integrity
            // (This is safe to do and not blocked by the SoftDevice)
            nrf_gpio_cfg(
                qspiPin(_sck), 
                NRF_GPIO_PIN_DIR_OUTPUT, 
                NRF_GPIO_PIN_INPUT_DISCONNECT, 
                NRF_GPIO_PIN_NOPULL, 
                NRF_GPIO_PIN_H0H1,  // High Drive
                NRF_GPIO_PIN_NOSENSE
            );

            nrf_gpio_cfg(
                qspiPin(_cs), 
                NRF_GPIO_PIN_DIR_OUTPUT, 
                NRF_GPIO_PIN_INPUT_DISCONNECT, 
                NRF_GPIO_PIN_NOPULL, 
                NRF_GPIO_PIN_H0H1,  // High Drive
                NRF_GPIO_PIN_NOSENSE
            );

            // 2. Actively drive CS high and SCK low to prevent handover glitches
            digitalWrite(_cs, HIGH);
            digitalWrite(_sck, LOW);

            // 3. Optional but recommended: pull IO lines high
            pinMode(_io0, OUTPUT); digitalWrite(_io0, HIGH);
            pinMode(_io1, OUTPUT); digitalWrite(_io1, HIGH);
            pinMode(_io2, OUTPUT); digitalWrite(_io2, HIGH);
            pinMode(_io3, OUTPUT); digitalWrite(_io3, HIGH);
            
            // Give the pins a millisecond to settle
            delay(1); 
        }
        
        inline uint8_t qspiPin(uint8_t arduinoPin) {
            return static_cast<uint8_t>(digitalPinToPinName(arduinoPin));
        }

        inline void waitForQspiPeripheralReady() {
            while (nrfx_qspi_mem_busy_check() == NRFX_ERROR_BUSY) {
                delay(1);
            }
        }

        uint8_t qspiReadStatus(uint8_t command) {
            uint8_t rx_buffer = 0;
            nrf_qspi_cinstr_conf_t cinstr_cfg = {
                .opcode    = command,
                .length    = NRF_QSPI_CINSTR_LEN_2B, // 1 byte opcode + 1 byte response
                .io2_level = true,
                .io3_level = true,
                .wipwait   = false,
                .wren      = false
            };
            nrfx_qspi_cinstr_xfer(&cinstr_cfg, nullptr, &rx_buffer);
            return rx_buffer;
        }

        void qspiWriteEnable() {
            nrf_qspi_cinstr_conf_t cinstr_cfg = {
                .opcode    = CMD_WRITE_ENABLE,
                .length    = NRF_QSPI_CINSTR_LEN_1B, // Just the 1 byte opcode
                .io2_level = true,
                .io3_level = true,
                .wipwait   = false,
                .wren      = false
            };
            nrfx_qspi_cinstr_xfer(&cinstr_cfg, nullptr, nullptr);
        }

        void qspiWriteStatus2(uint8_t status_val) {
            nrf_qspi_cinstr_conf_t cinstr_cfg = {
                .opcode    = CMD_WRITE_STATUS_2,
                .length    = NRF_QSPI_CINSTR_LEN_2B, // 1 byte opcode + 1 byte payload
                .io2_level = true,
                .io3_level = true,
                .wipwait   = false,
                .wren      = true // Let the hardware handle Write Enable automatically if needed
            };
            nrfx_qspi_cinstr_xfer(&cinstr_cfg, &status_val, nullptr);
        }

        bool waitForStatusWriteComplete() {
            for (uint32_t i = 0; i < 10000UL; ++i) {
                if ((qspiReadStatus(CMD_READ_STATUS_1) & STATUS_1_BUSY) == 0) {
                    return true;
                }
                delay(1);
            }
            return false;
        }

        bool ensureQuadEnable() {
            uint8_t status2 = qspiReadStatus(CMD_READ_STATUS_2);

            bool ok = true;
            if ((status2 & STATUS_2_QE) == 0) {
                qspiWriteEnable();
                qspiWriteStatus2(status2 | STATUS_2_QE);
                ok = waitForStatusWriteComplete();
                status2 = qspiReadStatus(CMD_READ_STATUS_2);
            }
            
            return ok && ((status2 & STATUS_2_QE) != 0);
        }

        // --- Static LFS C-Callbacks ---
        // These extract the C++ instance pointer from c->context to access the hardware instance

        static int littlefsRead(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size) {
            FileSystemImpl* self = static_cast<FileSystemImpl*>(c->context);
            uint32_t address = block * FLASH_SECTOR_SIZE + off;
            uint8_t *dst = static_cast<uint8_t *>(buffer);

            while (size) {
                lfs_size_t chunk = min(size, static_cast<lfs_size_t>(sizeof(self->_qspiScratch)));
                nrfx_err_t err = nrfx_qspi_read(self->_qspiScratch, chunk, address);
                if (err != NRFX_SUCCESS) return LFS_ERR_IO;
                memcpy(dst, self->_qspiScratch, chunk);
                dst += chunk;
                address += chunk;
                size -= chunk;
            }
            return LFS_ERR_OK;
        }

        static int littlefsProg(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buffer, lfs_size_t size) {
            FileSystemImpl* self = static_cast<FileSystemImpl*>(c->context);
            uint32_t address = block * FLASH_SECTOR_SIZE + off;
            const uint8_t *src = static_cast<const uint8_t *>(buffer);

            while (size) {
                lfs_size_t chunk = min(size, static_cast<lfs_size_t>(sizeof(self->_qspiScratch)));
                memcpy(self->_qspiScratch, src, chunk);
                nrfx_err_t err = nrfx_qspi_write(self->_qspiScratch, chunk, address);
                if (err != NRFX_SUCCESS) return LFS_ERR_IO;
                self->waitForQspiPeripheralReady();
                src += chunk;
                address += chunk;
                size -= chunk;
            }
            return LFS_ERR_OK;
        }

        static int littlefsErase(const struct lfs_config *c, lfs_block_t block) {
            FileSystemImpl* self = static_cast<FileSystemImpl*>(c->context);

            // Ensure idle before we start
            self->waitForQspiPeripheralReady();

            nrfx_err_t err = nrfx_qspi_erase(NRF_QSPI_ERASE_LEN_4KB, block * FLASH_SECTOR_SIZE);
            if (err != NRFX_SUCCESS) return LFS_ERR_IO;

            self->waitForQspiPeripheralReady();
            return LFS_ERR_OK;
        }

        static int littlefsSync(const struct lfs_config *c) {
            FileSystemImpl* self = static_cast<FileSystemImpl*>(c->context);
            self->waitForQspiPeripheralReady();
            return LFS_ERR_OK;
        }

        static int littlefsCountBlocks(void *p, lfs_block_t block) {
            (void)block; // Suppress unused parameter warning
            size_t *count = static_cast<size_t *>(p);
            *count += 1;
            return 0; // Return 0 to continue traversal
        }

    public:
        FileSystemImpl(uint8_t sck, uint8_t cs, uint8_t io0, uint8_t io1, uint8_t io2, uint8_t io3, const char* basepath) 
            : _basepath(basepath), _sck(sck), _cs(cs), _io0(io0), _io1(io1), _io2(io2), _io3(io3) 
        {
            _config = {};
            
            // Wire up the C++ instance to the C config
            _config.context = this;
            _config.read = littlefsRead;
            _config.prog = littlefsProg;
            _config.erase = littlefsErase;
            _config.sync = littlefsSync;
            
            _config.read_size = 16;
            _config.prog_size = FLASH_PAGE_SIZE;
            _config.block_size = FLASH_SECTOR_SIZE;
            _config.block_count = LITTLEFS_BLOCK_COUNT;
            _config.lookahead = sizeof(_lfsLookaheadBuffer) * 8;
            
            _config.read_buffer = _lfsReadBuffer;
            _config.prog_buffer = _lfsProgBuffer;
            _config.lookahead_buffer = _lfsLookaheadBuffer;
            _config.file_buffer = _lfsFileBuffer;
        }
        
        virtual ~FileSystemImpl() {
            // Optional: uninit QSPI if you want it torn down when adapter dies
            // nrfx_qspi_uninit();
        }

        inline virtual bool format() override {
            printf("[ustore] Formatting AdafruitQspiLittleFSFileSystem\n");
            _extFS.end();
            if (lfs_format(_extFS._getFS(), &_config) != LFS_ERR_OK) {
                printf("[ustore] Failed to format AdafruitQspiLittleFSFileSystem!\n");
                return false;
            }
            return true;
        }

        inline virtual bool init(bool reformatOnFail = true) override {
            printf("[ustore] Initializing AdafruitQspiLittleFSFileSystem Hardware\n");

            preconditionQspiPins();
            
            // 1. Initialize nRFx QSPI
            nrfx_qspi_config_t qspi_config = {
                .xip_offset = 0,
                .pins = {
                    .sck_pin = qspiPin(_sck),
                    .csn_pin = qspiPin(_cs),
                    .io0_pin = qspiPin(_io0),
                    .io1_pin = qspiPin(_io1),
                    .io2_pin = qspiPin(_io2),
                    .io3_pin = qspiPin(_io3),
                },
                .prot_if = {
                    .readoc = NRF_QSPI_READOC_READ4O,
                    .writeoc = NRF_QSPI_WRITEOC_PP4O,
                    .addrmode = NRF_QSPI_ADDRMODE_24BIT,
                    .dpmconfig = false,
                },
                .phy_if = {
                    .sck_delay = 2,
                    .dpmen = false,
                    .spi_mode = NRF_QSPI_MODE_0,
                    .sck_freq = NRF_QSPI_FREQ_DIV2, // 32 MHz / 2 = 16 MHz
                },
                .irq_priority = 3,
            };

            // Ignore NRFX_ERROR_INVALID_STATE if it happens to already be initialized
            nrfx_err_t err = nrfx_qspi_init(&qspi_config, nullptr, nullptr);
            if (err != NRFX_SUCCESS && err != NRFX_ERROR_INVALID_STATE) {
                printf("[ustore] nrfx_qspi_init failed: %d\n", err);
                return false;
            }

            // ensure quad mode is enabled on the flash chip
            if (!ensureQuadEnable()) {
                printf("[ustore] Failed to enable quad mode\n");
                return false;
            }

            // 2. Begin LittleFS
            if (!_extFS.begin(&_config)) {
                printf("[ustore] Failed to initialize Adafruit_LittleFS!\n");
                
                if (reformatOnFail) {
                    printf("[ustore] WARNING: Adafruit_LittleFS mount failed, reformatting!\n");
                    if (!format() || !_extFS.begin(&_config)) {
                        printf("[ustore] FATAL: Format and re-mount failed.\n");
                        return false;
                    }
                } else {
                    return false;
                }
            }

            bool verified = false;
            for (int attempt = 0; attempt < 1; ++attempt) {
                printf("[ustore] Check File System ...\n");

                microStore::File init_test = open("/__init_test__", microStore::File::ModeWrite, true);
                if (init_test) {
                    if (init_test.write((const uint8_t*)"test", 4) == 4) {
                        verified = true;
                    }
                    init_test.close();
                }
                
                if (reformatOnFail && !verified && attempt == 0) {
                    printf("[ustore] WARNING: File System write check failed, reformatting!\n");
                    if (!format() || !_extFS.begin(&_config)) {
                        printf("[ustore] FATAL: Format and re-mount failed.\n");
                        break;
                    }
                    continue; // Retry verification after reformat
                }

                break; // Exit loop after first attempt if not reformatting, or after successful verification
            }

            if (verified) {
                remove("/__init_test__");
                printf("[ustore] AdafruitQspiLittleFSFileSystem initialized successfully!\n");
                return true;
            }
            else {
                printf("[ustore] ERROR: File System verification failed.\n");
            }

            return false;
        }

        virtual microStore::File open(const char* path, microStore::File::Mode mode, const bool create = false) override {
            int pmode;
            switch (mode) {
				// Read only. File must exist. ("r")
				case microStore::File::ModeRead:
					pmode = Adafruit_LittleFS_Namespace::FILE_O_READ;
					break;
				// Write only. Creates file or truncates existing file. ("w")
				case microStore::File::ModeWrite:
					pmode = Adafruit_LittleFS_Namespace::FILE_O_WRITE;
					// CBA TODO Replace remove with working truncation
					if (_extFS.exists(path)) {
						_extFS.remove(path);
					}
					break;
				// Append only. Creates file if it doesn’t exist. Writes go to end. ("a")
				case microStore::File::ModeAppend:
					// CBA Append is the default write mode for nordicnrf52 LittleFS
					pmode = Adafruit_LittleFS_Namespace::FILE_O_WRITE;
					break;
				// Read and write. Creates file or truncates existing file. ("w+")
				case microStore::File::ModeReadWrite:
					pmode = Adafruit_LittleFS_Namespace::FILE_O_READ | Adafruit_LittleFS_Namespace::FILE_O_WRITE;
					// CBA TODO Replace remove with working truncation
					if (_extFS.exists(path)) {
						_extFS.remove(path);
					}
					break;
				// Read and append. Creates file if it doesn’t exist. ("a+")
				case microStore::File::ModeReadAppend:
					// CBA Append is the default write mode for nordicnrf52 LittleFS
					pmode = Adafruit_LittleFS_Namespace::FILE_O_READ | Adafruit_LittleFS_Namespace::FILE_O_WRITE;
					break;
				// Read and write. File must exist. ("r+") ???
				default:
					return {};
			}

            Adafruit_LittleFS_Namespace::File* file = new Adafruit_LittleFS_Namespace::File(_extFS.open(path, pmode));
            if (file == nullptr || !(*file)) {
                if (file) delete file;
                return {};
            }
            return microStore::File(new FileImpl(file));
        }

        inline virtual bool exists(const char* path) override { return _extFS.exists(path); }
        inline virtual bool remove(const char* path) override { return _extFS.remove(path); }
        inline virtual bool rename(const char* from_path, const char* to_path) override { return _extFS.rename(from_path, to_path); }
        inline virtual bool mkdir(const char* path) override { return _extFS.mkdir(path); }
        inline virtual bool rmdir(const char* path) override { return _extFS.rmdir(path); }

        virtual bool isDirectory(const char* path) override {
            Adafruit_LittleFS_Namespace::File file = _extFS.open(path, FILE_O_READ);
            if (file) {
                bool is_directory = file.isDirectory();
                file.close();
                return is_directory;
            }
            return false;
        }

        virtual std::list<std::string> listDirectory(const char* path, Callbacks::DirectoryListing callback = nullptr) override {
            std::list<std::string> files;
            Adafruit_LittleFS_Namespace::File root = _extFS.open(path, FILE_O_READ);
            if (!root || !root.isDirectory()) return files;

            Adafruit_LittleFS_Namespace::File file = root.openNextFile();
            while (file) {
                if (!file.isDirectory()) {
                    char* name = (char*)file.name();
                    if (callback) callback(name);
                    else files.push_back(name);
                }
                file.close();
                file = root.openNextFile();
            }
            root.close();
            return files;
        }

        inline virtual size_t usedBytes() {
            lfs_t* lfs_ptr = _extFS._getFS();
            if (!lfs_ptr) return 0;

            size_t allocated_blocks = 0;
            
            // lfs_fs_traverse visits every block currently allocated by the filesystem
            int err = lfs_traverse(lfs_ptr, littlefsCountBlocks, &allocated_blocks);
            
            if (err < 0) {
                printf("[ustore] Error getting LFS size via traversal: %d\n", err);
                return 0; 
            }

            // Calculate total used bytes: blocks * sector size
            return allocated_blocks * FLASH_SECTOR_SIZE;
        }

        inline virtual size_t storageSize() override { return FLASH_SIZE_BYTES; }
        inline virtual size_t storageAvailable() override { return storageSize() - usedBytes(); }
    };

};

} }
