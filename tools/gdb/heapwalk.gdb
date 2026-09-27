set pagination off
file build/GNOSKr.elf
target remote :1234
set $h = 0xffff800000142210
set $i = 0
while ($i < 60 && $h != 0)
  set $size = *(unsigned long *)$h
  set $fr = *(unsigned long *)($h + 8)
  set $nxt = *(unsigned long *)($h + 24)
  printf "%d: h=%p size=%ld free=%ld next=%p\n", $i, $h, $size, $fr, $nxt
  if ($size < 32 || $size > 0x1000000 || $fr > 1)
    printf "CORRUPTED ENTRY ABOVE\n"
    set $i = 999
  end
  set $h = $nxt
  set $i = $i + 1
end
detach
