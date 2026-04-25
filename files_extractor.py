import os
import struct
import sys

def read_null_terminated_string(data):
    return data.split(b'\x00', 1)[0].decode('utf-8', errors='ignore')

def extract_files(input_file):
    output_dir = input_file + "_extracted"
    os.makedirs(output_dir, exist_ok=True)

    with open(input_file, "rb") as f:
        data = f.read()

    # Skip header (16 bytes)
    offset = 16
    entry_size = 48

    entries = []

    # Parse entries
    while offset + entry_size <= len(data):
        entry = data[offset:offset + entry_size]

        filename_raw = entry[0:32]
        file_offset = struct.unpack(">I", entry[32:36])[0]
        file_size = struct.unpack(">I", entry[36:40])[0]

        filename = read_null_terminated_string(filename_raw)

        # Stop if empty filename
        if not filename:
            break

        # Sanitize filename
        filename = filename.replace("/", "_").replace("\\", "_")

        entries.append((filename, file_offset, file_size))
        offset += entry_size

    print(f"[+] {input_file}: Found {len(entries)} files")

    # Extract files
    for filename, file_offset, file_size in entries:
        output_path = os.path.join(output_dir, filename)

        print(f"[+] Extracting {filename} (offset={file_offset}, size={file_size})")

        file_data = data[file_offset:file_offset + file_size]

        with open(output_path, "wb") as out:
            out.write(file_data)

    print(f"[+] Done extracting {input_file} -> {output_dir}\n")


def main():
    if len(sys.argv) < 2:
        print("Usage: python files_extractor.py <file.files>")
        sys.exit(1)

    input_file = sys.argv[1]

    if not os.path.isfile(input_file):
        print(f"[-] File not found: {input_file}")
        sys.exit(1)

    extract_files(input_file)


if __name__ == "__main__":
    main()