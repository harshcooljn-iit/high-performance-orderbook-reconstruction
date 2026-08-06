import os

in_file = 'data/mbo.csv'
out_file = 'data/mbo_2M.csv'

print(f"Reading {in_file}...")
with open(in_file, 'r') as f:
    lines = f.readlines()

header = lines[0]
data = lines[1:]

num_copies = 340
total_lines = len(data) * num_copies + 1

print(f"Writing {out_file} with approximately {total_lines} rows...")
with open(out_file, 'w') as out:
    out.write(header)
    for i in range(num_copies):
        out.writelines(data)
        if (i + 1) % 50 == 0:
            print(f"Wrote {i+1} copies...")

print("Done!")
