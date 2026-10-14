#!/usr/bin/env python3
"""Send debug-console commands to the ESP32 (UART0 bridge) and print replies.

usage: esp_console.py [-p /dev/ttyACM0] [-w SECONDS] "cmd1" "cmd2" ...
"@sleep N" just prints output for N s. Each command waits until a reply line (RSP/FLOOD/RBENCH/BAUD/STATS/ERR) or the timeout.
Opening the port does not reset the ESP (DTR/RTS held low).
"""
import argparse, sys, time
import serial

REPLY = ('RSP', 'FLOOD', 'RBENCH', 'BAUD', 'STATS', 'ERR')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-p', '--port', default='/dev/ttyACM0')
    ap.add_argument('-w', '--wait', type=float, default=20.0, help='max seconds per command')
    ap.add_argument('--log', action='store_true', help='also print ESP log lines')
    ap.add_argument('cmds', nargs='+')
    a = ap.parse_args()
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = a.port, 115200, 0.1
    s.dtr = False
    s.rts = False
    s.open()
    time.sleep(0.2)
    s.reset_input_buffer()
    for cmd in a.cmds:
        print('>>', cmd, flush=True)
        wait = a.wait
        if cmd.startswith('@sleep'):          # just collect output for N seconds
            wait = float(cmd.split()[1])
        else:
            s.write((cmd + '\n').encode())
        t0 = time.time()
        buf = b''
        done = False
        while time.time() - t0 < wait and not done:
            buf += s.read(4096)
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                txt = line.decode(errors='replace').strip()
                if txt.startswith(REPLY) or txt.startswith('EVT'):
                    print(txt, flush=True)
                    if txt.startswith(REPLY):
                        done = True
                elif a.log and txt:
                    print('  |', txt, flush=True)
        if not done and not cmd.startswith('@sleep'):
            print('(no reply)', flush=True)
    s.close()

if __name__ == '__main__':
    sys.exit(main())
