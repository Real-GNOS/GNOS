# Example script for MicroPython on hudos.
# Run it from the shell with:   open app/micropython.elf app/demo.py
# or from the tdesktop Applications list (terminal window).
print("MicroPython on hudos -- demo.py")
total = 0
for i in range(1, 6):
    total += i
    print("i =", i, " running total =", total)
print("sum(1..5) =", total)
