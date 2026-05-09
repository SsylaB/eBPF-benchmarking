#!/usr/bin/env python3
import sys, subprocess

def get_offset(binary):
    try:
        out = subprocess.check_output(["readelf", "-sW", binary], text=True)
        for line in out.splitlines():
            # Look for the exact processCommand symbol
            if line.endswith(" processCommand"):
                parts = line.split()
                # Parts: [Num, Value, Size, Type, Bind, Vis, Ndx, Name]
                return int(parts[1], 16)
    except Exception as e:
        pass
    return None

def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "/usr/bin/redis-server"
    offset = get_offset(binary)
    if offset is None:
        print("ERROR: Could not find processCommand symbol in binary.", file=sys.stderr)
        sys.exit(1)
    
    print(f"FUNC_OFFSET=0x{offset:x}")

if __name__ == "__main__":
    main()