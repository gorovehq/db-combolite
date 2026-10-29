# Prints the slicing-by-8 tables for CRC-32C (reflected polynomial 0x82F63B78)
# as a C initializer, for src/cdb_crc32c.c.
#   T[0][n] = CRC of the single byte n (the classic table)
#   T[k][n] = (T[k-1][n] >> 8) ^ T[0][T[k-1][n] & 0xFF]
POLY = 0x82F63B78

t = [[0] * 256 for _ in range(8)]
for n in range(256):
    c = n
    for _ in range(8):
        c = (c >> 1) ^ POLY if c & 1 else c >> 1
    t[0][n] = c
for k in range(1, 8):
    for n in range(256):
        p = t[k - 1][n]
        t[k][n] = (p >> 8) ^ t[0][p & 0xFF]

print("static const uint32_t crc32c_table[8][256] = {")
for k in range(8):
    print("    {")
    for row in range(0, 256, 4):
        print("        " + ", ".join("0x%08Xu" % v for v in t[k][row:row + 4]) + ",")
    print("    },")
print("};")
