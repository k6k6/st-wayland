#!/usr/bin/env python3
"""Display the 423 added sprites; run inside the newly built st.

    ./st -e python3 tools/box_geometry_demo.py
    python3 tools/box_geometry_demo.py --no-wait > /tmp/geometry-demo.txt
"""
import argparse
import sys

GROUPS = [
    ("A", "Outline triangles", [(0x25F8, 0x25FA), (0x25FF, 0x25FF)]),
    ("B", "Third-width blocks", [(0x1FBCE, 0x1FBCF)]),
    ("C", "Sixteenth / partial / centred blocks", [(0x1CE90, 0x1CEAF), (0x1FBE4, 0x1FBE7)]),
    ("D", "Extra box junctions", [(0x1CC1B, 0x1CC1E), (0x1CE16, 0x1CE19)]),
    ("E", "Negative diagonals", [(0x1FBBD, 0x1FBBF)]),
    ("F", "Octants: 2 columns x 4 rows", [(0x1CD00, 0x1CDE5)]),
    ("G", "Separated quadrants (gaps are intentional)", [(0x1CC21, 0x1CC2F)]),
    ("H", "Separated sextants (gaps are intentional)", [(0x1CE51, 0x1CE8F)]),
    ("I", "Branch drawing PUA", [(0xF5D0, 0xF60D)]),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--no-wait", action="store_true", help="do not wait for Enter")
    args = parser.parse_args()
    print("st cell-aligned geometry additions: 423 characters")
    print("Scroll up and change the font size to inspect the patterns.\n")
    for label, title, ranges in GROUPS:
        values = [u for a, b in ranges for u in range(a, b + 1)]
        print(f"{label}. {title} ({len(values)})")
        for offset in range(0, len(values), 16):
            row = values[offset:offset + 16]
            print(f"  U+{row[0]:05X}: " + " ".join(map(chr, row)))
        print()
    print("Joined branch example (with an intentionally hollow middle node):")
    for row in [
        [0xF5F0, 0xF5D0, 0xF5F5, 0xF5D0, 0xF5FE],
        [32, 32, 32, 32, 0xF5D1],
        [0xF5F0, 0xF5D0, 0xF5D0, 0xF5D0, 0xF602],
    ]:
        print("  " + "".join(map(chr, row)))
    print("\nBranch PUA can be disabled separately with boxdraw_branch = 0.")
    if not args.no_wait and sys.stdin.isatty():
        try:
            input("Press Enter to close... ")
        except EOFError:
            pass


if __name__ == "__main__":
    main()
