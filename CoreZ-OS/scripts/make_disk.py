import struct
import sys

SECTOR = 512
TOTAL_SECTORS = 80 * 1024 * 1024 // SECTOR  

MBR_SIG = 0x55AA

def part_entry(bootable, fs_type, start_lba, sec_cnt):
    
    return struct.pack(
        "<BBBBBBBBII",
        bootable,
        0, 0, 0,
        fs_type,
        0, 0, 0,
        start_lba & 0xFFFFFFFF,
        sec_cnt & 0xFFFFFFFF,
    )

def mbr_sector(entries, sig=MBR_SIG):
    buf = bytearray(SECTOR)
    for i, e in enumerate(entries[:4]):
        buf[446 + i * 16 : 446 + (i + 1) * 16] = e
    buf[510] = sig & 0xFF
    buf[511] = (sig >> 8) & 0xFF
    return buf

def build(path):
    img = bytearray(TOTAL_SECTORS * SECTOR)

    img[0:SECTOR] = mbr_sector([
        part_entry(0, 0x66, 2048, 32768),      
        part_entry(0, 0x66, 34816, 32768),     
        part_entry(0, 0x66, 67584, 32768),     
        part_entry(0, 0x05, 100352, 63488),    
    ])

    ebr1_lba = 100352
    img[ebr1_lba * SECTOR:(ebr1_lba + 1) * SECTOR] = mbr_sector([
        part_entry(0, 0x66, 2048, 20480),   
        part_entry(0, 0x05, 22528, 40960),  
        b"\x00" * 16,
        b"\x00" * 16,
    ])

    ebr2_lba = 122880
    img[ebr2_lba * SECTOR:(ebr2_lba + 1) * SECTOR] = mbr_sector([
        part_entry(0, 0x66, 2048, 20480),  
        b"\x00" * 16,
        b"\x00" * 16,
        b"\x00" * 16,
    ])

    with open(path, "wb") as f:
        f.write(img)
    print(f"OK: {path} ({TOTAL_SECTORS} sectors, {TOTAL_SECTORS * SECTOR // 1024 // 1024}MB)")
    print("  sda1 @2048 +32768, sda2 @34816 +32768, sda3 @67584 +32768")
    print("  ext @100352, sda5 @102400 +20480, sda6 @124928 +20480")

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "test_hd.img"
    build(out)
