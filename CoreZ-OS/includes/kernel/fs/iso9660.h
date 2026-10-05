#ifndef FS_ISO9660_H
#define FS_ISO9660_H

#include <cstdint>
#include <stdint.h>

struct ISO9660_DIRECTORY_RECORD {
    uint8_t  length;
    uint8_t  ext_attr_length;
    uint8_t  extent_location[4];
    uint8_t data_length[4];
    uint8_t  rec_date[7];
    uint8_t  file_flags;
    uint8_t  file_unit_size;
    uint8_t  interleave_gap;
    uint8_t  volume_seq_number[2];
    uint8_t  name_len;
    char     name[1];
} __attribute__((packed));

struct ISO9660_PRIMARY_DESC {
    uint8_t  type;
    char     id[5];
    uint8_t  version;
    uint8_t  unused1;
    char     system_id[32];
    char     volume_id[32];
    uint8_t  unused2[8];
    uint8_t  volume_space_size[8];
    uint8_t  volume_set_size[4];
    uint8_t  volume_sequence_number[4];
    uint8_t  logical_block_size[4];
    uint8_t  path_table_size[8];
    uint32_t type_l_path_table;
    uint32_t opt_type_l_path_table;
    uint32_t type_m_path_table;
    uint32_t opt_type_m_path_table;
    uint8_t  root_directory_record[34];
    char     volume_set_identifier[128];
    char     publisher_identifier[128];
    char     data_preparer_identifier[128];
    char     application_identifier[128];
    char     copyright_file_identifier[38];
    char     abstract_file_identifier[36];
    char     bibliographic_file_identifier[37];
    char     volume_creation_datetime[17];
    char     volume_modification_datetime[17];
    char     volume_expiration_datetime[17];
    char     volume_effective_datetime[17];
    uint8_t  file_structure_version;
    uint8_t  unused3;
    uint8_t  application_reserved[512];
    uint8_t  reserved[686];
} __attribute__((packed));

#endif
