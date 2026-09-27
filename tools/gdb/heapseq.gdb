set pagination off
file build/GNOSKr.elf
target remote :1234
set $b = 0xffff800000101000
set $end = $b + 0x1000000
set $h = $b
set $i = 0
while ($h < $end && $i < 5000)
  set $size = *(unsigned long *)$h
  set $fr = *(unsigned long *)($h + 8)
  if ($size < 32 || $h + $size > $end)
    printf "CORRUPT at h=%p (i=%d) size=%ld free=%ld\n", $h, $i, $size, $fr
    printf "prev block fields:\n"
    set $p = *(unsigned long *)($h - 32)
    set $pf = *(unsigned long *)($h - 24)
    printf "prev h=%p size=%ld free=%ld\n", $h - 32, $p, $pf
    printf "raw qwords at corrupt h:\n"
    x/8gx $h
    set $i = 99999
  end
  set $h = $h + $size
  set $i = $i + 1
end
if ($i != 99999)
  printf "chain walked clean to end (i=%d, h=%p)\n", $i, $h
end
detach
