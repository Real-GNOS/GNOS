#ifndef ARCH_X86_UEFI_EFI_H
#define ARCH_X86_UEFI_EFI_H

#include <stdint.h>

typedef uint8_t  EFI_BOOLEAN;
typedef uint16_t CHAR16;
typedef void     *EFI_HANDLE;
typedef void     *EFI_EVENT;
typedef uint64_t EFI_STATUS;
typedef uint64_t EFI_PHYSICAL_ADDRESS;
typedef uint64_t EFI_VIRTUAL_ADDRESS;
typedef uint64_t EFI_TPL;

#define EFIAPI __attribute__((ms_abi))

#define EFI_SUCCESS 0ull
#define EFI_ERR_BIT 0x8000000000000000ull

#define EFI_LOAD_ERROR (EFI_ERR_BIT | 1ull)
#define EFI_INVALID_PARAMETER (EFI_ERR_BIT | 2ull)
#define EFI_BUFFER_TOO_SMALL  (EFI_ERR_BIT | 5ull)
#define EFI_NOT_FOUND         (EFI_ERR_BIT | 14ull)

#define EFI_PAGE_SIZE 4096ull

#define EFI_ALLOCATE_ANY_PAGES 0u
#define EFI_ALLOCATE_MAX_ADDRESS 1u
#define EFI_ALLOCATE_ADDRESS 2u
#define EFI_LOADER_DATA 2u

#define EFI_MEMORY_WB 0x8ull

#define EFI_FILE_MODE_READ 0x0000000000000001ull

struct EFI_GUID {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];
};

struct EFI_TABLE_HEADER {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
};

struct EFI_CONFIGURATION_TABLE {
    struct EFI_GUID vendor_guid;
    void *vendor_table;
};

typedef EFI_STATUS(EFIAPI *efi_reserved_fn)(void);

struct EFI_BOOT_SERVICES;
struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
struct EFI_FILE_PROTOCOL;
struct EFI_GRAPHICS_OUTPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    efi_reserved_fn reset;
    EFI_STATUS(EFIAPI *output_string)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self,
                                      CHAR16 *string);
    efi_reserved_fn test_string;
    efi_reserved_fn query_mode;
    efi_reserved_fn set_mode;
    efi_reserved_fn set_attribute;
    efi_reserved_fn clear_screen;
    efi_reserved_fn set_cursor_position;
    efi_reserved_fn enable_cursor;
    void *mode;
};

struct EFI_SYSTEM_TABLE {
    struct EFI_TABLE_HEADER hdr;
    CHAR16 *firmware_vendor;
    uint32_t firmware_revision;
    EFI_HANDLE console_in_handle;
    void *con_in;
    EFI_HANDLE console_out_handle;
    struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *con_out;
    EFI_HANDLE std_err_handle;
    void *std_err;
    void *runtime_services;
    struct EFI_BOOT_SERVICES *boot_services;
    uint64_t number_of_table_entries;
    struct EFI_CONFIGURATION_TABLE *configuration_table;
};

typedef EFI_STATUS(EFIAPI *efi_allocate_pages)(uint32_t type, uint32_t memory_type,
                                               uint64_t pages, void **memory);
typedef EFI_STATUS(EFIAPI *efi_get_memory_map)(uint64_t *map_size, void *map,
                                               uint64_t *map_key,
                                               uint64_t *descriptor_size,
                                               uint32_t *descriptor_version);
typedef EFI_STATUS(EFIAPI *efi_allocate_pool_t)(uint32_t pool_type,
                                                uint64_t size, void **buffer);
typedef EFI_STATUS(EFIAPI *efi_handle_protocol_t)(EFI_HANDLE handle,
                                                  const struct EFI_GUID *guid,
                                                  void **interface);
typedef EFI_STATUS(EFIAPI *efi_exit_boot_services_t)(EFI_HANDLE image,
                                                     uint64_t map_key);
typedef EFI_STATUS(EFIAPI *efi_locate_protocol_t)(const struct EFI_GUID *guid,
                                                  void *registration, void **interface);
typedef EFI_STATUS(EFIAPI *efi_free_pool_t)(void *buffer);
typedef EFI_STATUS(EFIAPI *efi_set_mem_t)(void *buffer, uint64_t size,
                                          uint8_t value);
typedef EFI_STATUS(EFIAPI *efi_copy_mem_t)(void *dst, const void *src,
                                           uint64_t len);

struct EFI_BOOT_SERVICES {
    struct EFI_TABLE_HEADER hdr;
    efi_reserved_fn raise_tpl;
    efi_reserved_fn restore_tpl;
    efi_allocate_pages allocate_pages;
    efi_reserved_fn free_pages;
    efi_get_memory_map get_memory_map;
    efi_allocate_pool_t allocate_pool;
    efi_free_pool_t free_pool;
    efi_reserved_fn create_event;
    efi_reserved_fn set_timer;
    efi_reserved_fn wait_for_event;
    efi_reserved_fn signal_event;
    efi_reserved_fn close_event;
    efi_reserved_fn check_event;
    efi_reserved_fn install_protocol_interface;
    efi_reserved_fn reinstall_protocol_interface;
    efi_reserved_fn uninstall_protocol_interface;
    efi_handle_protocol_t handle_protocol;
    efi_reserved_fn reserved;
    efi_reserved_fn register_protocol_notify;
    efi_reserved_fn locate_handle;
    efi_reserved_fn locate_device_path;
    efi_reserved_fn install_configuration_table;
    efi_reserved_fn load_image;
    efi_reserved_fn start_image;
    efi_reserved_fn exit;
    efi_reserved_fn unload_image;
    efi_exit_boot_services_t exit_boot_services;
    efi_reserved_fn get_next_monotonic_count;
    efi_reserved_fn stall;
    efi_reserved_fn set_watchdog_timer;
    efi_reserved_fn connect_controller;
    efi_reserved_fn disconnect_controller;
    efi_reserved_fn open_protocol;
    efi_reserved_fn close_protocol;
    efi_reserved_fn open_protocol_information;
    efi_reserved_fn protocols_per_handle;
    efi_reserved_fn locate_handle_buffer;
    efi_locate_protocol_t locate_protocol;
    efi_reserved_fn install_multiple_protocol_interfaces;
    efi_reserved_fn uninstall_multiple_protocol_interfaces;
    efi_reserved_fn calculate_crc32;
    efi_copy_mem_t copy_mem;
    efi_set_mem_t set_mem;
    efi_reserved_fn create_event_ex;
};

struct EFI_MEMORY_DESCRIPTOR {
    uint32_t type;
    uint32_t pad;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
};

struct EFI_PIXEL_BITMASK {
    uint32_t red_mask;
    uint32_t green_mask;
    uint32_t blue_mask;
    uint32_t reserved_mask;
};

struct EFI_GRAPHICS_OUTPUT_MODE_INFORMATION {
    uint32_t version;
    uint32_t horizontal_resolution;
    uint32_t vertical_resolution;
    uint32_t pixel_format;
    struct EFI_PIXEL_BITMASK pixel_information;
    uint32_t pixels_per_scan_line;
};

struct EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE {
    uint32_t max_mode;
    uint32_t mode;
    struct EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
    uint64_t size_of_info;
    uint64_t frame_buffer_base;
    uint64_t frame_buffer_size;
};

typedef EFI_STATUS(EFIAPI *efi_gop_query_mode)(
    struct EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, uint32_t mode_number,
    uint64_t *size_of_info, struct EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **info);
typedef EFI_STATUS(EFIAPI *efi_gop_set_mode)(
    struct EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, uint32_t mode_number);

struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    efi_gop_query_mode query_mode;
    efi_gop_set_mode set_mode;
    efi_reserved_fn blt;
    struct EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *mode;
};

typedef EFI_STATUS(EFIAPI *efi_file_open_t)(
    struct EFI_FILE_PROTOCOL *file, struct EFI_FILE_PROTOCOL **new_handle,
    CHAR16 *file_name, uint64_t open_mode, uint64_t attributes);
typedef EFI_STATUS(EFIAPI *efi_file_read_t)(struct EFI_FILE_PROTOCOL *file,
                                            uint64_t *buffer_size, void *buffer);
typedef EFI_STATUS(EFIAPI *efi_file_set_position_t)(
    struct EFI_FILE_PROTOCOL *file, uint64_t position);
typedef EFI_STATUS(EFIAPI *efi_file_close_t)(struct EFI_FILE_PROTOCOL *file);

struct EFI_FILE_PROTOCOL {
    uint64_t revision;
    efi_file_open_t open;
    efi_file_close_t close;
    efi_reserved_fn delete_file;
    efi_file_read_t read;
    efi_reserved_fn write;
    efi_reserved_fn get_position;
    efi_file_set_position_t set_position;
    efi_reserved_fn get_info;
    efi_reserved_fn set_info;
    efi_reserved_fn flush;
};

typedef EFI_STATUS(EFIAPI *efi_open_volume_t)(
    struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs,
    struct EFI_FILE_PROTOCOL **root);

struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    uint64_t revision;
    efi_open_volume_t open_volume;
};

struct EFI_LOADED_IMAGE_PROTOCOL {
    uint32_t revision;
    EFI_HANDLE parent_handle;
    struct EFI_SYSTEM_TABLE *system_table;
    EFI_HANDLE device_handle;
    void *file_path;
    void *reserved;
    uint32_t load_options_size;
    void *load_options;
    void *image_base;
    uint64_t image_size;
    uint32_t image_code_type;
    uint32_t image_data_type;
    efi_reserved_fn unload;
};

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    {0x5B1B31A1, 0x9562, 0x11d2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    {0x964E5B22, 0x6459, 0x11d2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    {0x9042A9DE, 0x23DC, 0x4A38, {0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A}}
#define EFI_ACPI_TABLE_GUID \
    {0xeb9d2d30, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}}
#define EFI_ACPI_20_TABLE_GUID \
    {0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}}

#endif /* ARCH_X86_UEFI_EFI_H */