#!/usr/bin/env python3
import sys
import subprocess
import os

def main():
    if len(sys.argv) < 3:
        print("Usage: generate_map.py <input.elf> <output.txt> [nm_tool]")
        sys.exit(1)

    elf_file = sys.argv[1]
    out_file = sys.argv[2]
    nm_tool = sys.argv[3] if len(sys.argv) > 3 else "nm"

    if not os.path.exists(elf_file):
        print(f"Error: {elf_file} not found")
        sys.exit(1)

    # Запуск nm для извлечения символов
    try:
        res = subprocess.run([nm_tool, "-n", "--defined-only", elf_file], capture_output=True, text=True, check=True)
    except Exception as e:
        # Фоллбэк на системный nm, если i386-elf-nm не найден
        res = subprocess.run(["nm", "-n", "--defined-only", elf_file], capture_output=True, text=True, check=True)

    funcs = []
    for line in res.stdout.strip().split("\n"):
        parts = line.strip().split()
        if len(parts) >= 3:
            addr_str, sym_type, name = parts[0], parts[1], parts[2]
            # Берем только код / функции (T, t, W, w)
            if sym_type in ('T', 't', 'W', 'w'):
                try:
                    addr_hex = f"{int(addr_str, 16):08x}"
                    funcs.append((addr_hex, name))
                except ValueError:
                    continue

    # Создаем директорию назначения, если её нет
    os.makedirs(os.path.dirname(os.path.abspath(out_file)), exist_ok=True)

    # Запись в файл в требуемом формате
    with open(out_file, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"{len(funcs)};\n")
        for addr_hex, name in funcs:
            f.write(f"{addr_hex};{name}\n")

    print(f"  MAP\t{elf_file} -> {out_file} ({len(funcs)} functions)")

if __name__ == "__main__":
    main()