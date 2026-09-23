#!/usr/bin/env python3
"""compare.py a.png b.png — pixel diff of two screenshots"""
import sys
from PIL import Image, ImageChops
a = Image.open(sys.argv[1]).convert('RGBA'); b = Image.open(sys.argv[2]).convert('RGBA')
print(a.size, b.size)
d = ImageChops.difference(a, b); bbox = d.getbbox(); print('diff bbox', bbox)
if bbox:
    import numpy as np
    m = np.asarray(d).astype(int).max(axis=2)
    print('max diff', m.max(), 'pixels differing', (m > 0).sum(), '>8:', (m > 8).sum())
