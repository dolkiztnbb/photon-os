#!/usr/bin/env python3
"""
tools/kern_install.py — прототип установщика ядер для PhotonOS

Умеет:
  - добавить новое ядро в существующий fs.vd образ (в /sys/kernel/)
  - установить boot.cfg/default на новое ядро
  - пересобрать os-image.bin из компонентов (если указан --image)

Использование:
  # Добавить ядро в fs.vd
  ./tools/kern_install.py --fs kernel/datslaf/fs.vd --kernel init.bin --name kernel-1.1.0.bin --set-default

  # Добавить ядро и пересобрать os-image.bin
  ./tools/kern_install.py --image output/build/image/os-image.bin --kernel init.bin --name kernel-1.1.0.bin --set-default

  # Просто список ядер в fs.vd
  ./tools/kern_install.py --fs kernel/datslaf/fs.vd --list

Требует: Python 3, без внешних зависимостей.
Использует ту же логику DATSLAF, что и kernel/datslaf/build_fs.py
"""

import argparse
import os
import struct
import sys
import shutil
from datetime import datetime

SECTOR_SIZE = 512
MAX_DISK_NODES = 100
TYPE_DIR = 1
TYPE_FILE = 0
MAGIC = 0xDA751AF5

SUPERBLOCK_FMT = "<III"
DNODE_FMT = "<B32sIIIiI11s"

def get_timestamp():
    epoch = datetime(2000, 1, 1)
    now = datetime.now()
    return int((now - epoch).total_seconds())

class PyDnode:
    def __init__(self, name, node_type, parent_id=-1, size=0, data=b"", data_block=0):
        self.name = name[:31]
        self.type = node_type
        self.size = size
        self.change_time = get_timestamp()
        self.permissions = 0o755
        self.parent_id = parent_id
        self.data_block = data_block
        self.data = data
        self.children = []
        self.id = -1

def parse_fs_vd(path):
    with open(path, "rb") as f:
        img = f.read()
    if len(img) < SECTOR_SIZE:
        sys.exit(f"{path}: too small")
    magic, total_nodes, data_block_start = struct.unpack_from(SUPERBLOCK_FMT, img, 0)
    if magic != MAGIC:
        sys.exit(f"{path}: bad magic {hex(magic)}")
    print(f"FS: magic OK, total_nodes={total_nodes}, data_block_start={data_block_start}")
    nodes = []
    for i in range(total_nodes):
        off = SECTOR_SIZE + i*64
        if off+64 > len(img):
            break
        typ, name_b, size, ctime, perm, parent_id, data_block, _ = struct.unpack_from(DNODE_FMT, img, off)
        name = name_b.split(b'\x00')[0].decode(errors='ignore')
        # skip empty (deleted) except root id 0
        if i!=0 and name=='':
            nodes.append(None)
            continue
        # read data if file
        data = b""
        if typ==TYPE_FILE and data_block>0 and size>0:
            sectors = (size+511)//512
            data_off = data_block*SECTOR_SIZE
            data = img[data_off:data_off+size]
        n = PyDnode(name, typ, parent_id, size, data, data_block)
        n.id = i
        n.change_time = ctime
        n.permissions = perm
        nodes.append(n)
    # rebuild tree for convenience
    id_to_node = {n.id:n for n in nodes if n}
    for n in nodes:
        if n and n.parent_id>=0 and n.parent_id in id_to_node and n.id!=n.parent_id:
            parent = id_to_node[n.parent_id]
            parent.children.append(n)
    return img, nodes, id_to_node

def find_node_by_path(nodes, id_to_node, path):
    # path like /sys/kernel or sys/kernel
    if path.startswith('/'):
        path=path[1:]
    parts=path.split('/') if path else []
    # find root
    root=None
    for n in nodes:
        if n and n.parent_id==-1:
            root=n
            break
    if not root:
        return None
    cur=root
    for part in parts:
        if not part:
            continue
        found=None
        for child in cur.children:
            if child.name==part:
                found=child
                break
        if not found:
            return None
        cur=found
    return cur

def list_kernels(fs_path):
    _, nodes, id_to_node = parse_fs_vd(fs_path)
    kdir = find_node_by_path(nodes, id_to_node, "/sys/kernel")
    if not kdir:
        print("/sys/kernel not found")
        return
    print(f"Found /sys/kernel id={kdir.id} with {len(kdir.children)} children:")
    for child in sorted(kdir.children, key=lambda x: x.name):
        typ = "DIR" if child.type==TYPE_DIR else "FILE"
        print(f"  [{typ}] {child.name} size={child.size} id={child.id} parent={child.parent_id} dblock={child.data_block}")
    # show boot.cfg
    for child in kdir.children:
        if child.name in ("boot.cfg","default","active"):
            print(f"  -> {child.name} content: {child.data[:100]!r}")

def build_fs_from_nodes(nodes, id_to_node, output_path):
    # Re-serialize similar to build_fs.py
    # We need to reassign data_block and rebuild image
    # First, collect all nodes in DFS order to preserve IDs? Simpler: keep original IDs order, but recompute data_block
    # We'll create new image from scratch using existing nodes list order (by id)
    total_nodes = max([n.id for n in nodes if n])+1 if nodes else 0
    # Prepare disk_nodes array size total_nodes
    disk_nodes = [None]*total_nodes
    for n in nodes:
        if n:
            disk_nodes[n.id]=n
    # Now assign data blocks
    current_sector = 15  # FS_START_SECTOR=0 +15
    data_blocks = []
    for i in range(total_nodes):
        n = disk_nodes[i]
        if not n:
            continue
        if n.type==TYPE_FILE and n.size>0 and n.data:
            n.data_block = current_sector
            sectors = (n.size+511)//512
            padded = n.data.ljust(sectors*512, b'\0')
            data_blocks.append((current_sector, padded))
            current_sector+=sectors
        else:
            if n.type==TYPE_FILE and n.size==0:
                n.data_block=0
    max_sector = current_sector
    image_size = max_sector*SECTOR_SIZE
    disk_image = bytearray(image_size)
    # superblock
    sb = struct.pack(SUPERBLOCK_FMT, MAGIC, total_nodes, 15)
    disk_image[0:len(sb)]=sb
    # nodes
    for i in range(total_nodes):
        n = disk_nodes[i]
        if not n:
            # zero node
            continue
        name_b = n.name.encode()[:31].ljust(32,b'\0')
        packed = struct.pack(DNODE_FMT, n.type, name_b, n.size, n.change_time, n.permissions, n.parent_id, n.data_block, b'\0'*11)
        off = SECTOR_SIZE + i*64
        disk_image[off:off+64]=packed
    for sector, data in data_blocks:
        off = sector*SECTOR_SIZE
        disk_image[off:off+len(data)]=data
    with open(output_path, "wb") as f:
        f.write(disk_image)
    print(f"Built {output_path}: {len(disk_image)} bytes, {total_nodes} nodes")

def install_kernel(fs_path, kernel_bin_path, dest_name, set_default=False, output_fs=None):
    if not os.path.exists(kernel_bin_path):
        sys.exit(f"kernel file not found: {kernel_bin_path}")
    with open(kernel_bin_path, "rb") as f:
        kdata = f.read()
    print(f"Kernel {kernel_bin_path}: {len(kdata)} bytes")
    img, nodes, id_to_node = parse_fs_vd(fs_path)
    kdir = find_node_by_path(nodes, id_to_node, "/sys/kernel")
    if not kdir:
        sys.exit("/sys/kernel not found in FS")
    # check if dest exists
    existing = None
    for child in kdir.children:
        if child.name==dest_name:
            existing=child
            break
    if existing:
        print(f"Overwriting existing {dest_name} id={existing.id}")
        existing.data = kdata
        existing.size = len(kdata)
        existing.change_time = get_timestamp()
    else:
        # create new node
        if len([n for n in nodes if n]) >= MAX_DISK_NODES:
            sys.exit(f"Max nodes {MAX_DISK_NODES} reached")
        new_id = max([n.id for n in nodes if n])+1 if nodes else 0
        new_node = PyDnode(dest_name, TYPE_FILE, kdir.id, len(kdata), kdata, 0)
        new_node.id = new_id
        nodes.append(new_node)
        id_to_node[new_id]=new_node
        kdir.children.append(new_node)
        print(f"Created new node {dest_name} id={new_id}")

    if set_default:
        for meta_name in ("boot.cfg","default","active"):
            meta_node = None
            for child in kdir.children:
                if child.name==meta_name:
                    meta_node=child
                    break
            content = (dest_name+"\n").encode()
            if meta_node:
                meta_node.data=content
                meta_node.size=len(content)
                meta_node.change_time=get_timestamp()
                print(f"Updated {meta_name} -> {dest_name}")
            else:
                new_id = max([n.id for n in nodes if n])+1
                mn = PyDnode(meta_name, TYPE_FILE, kdir.id, len(content), content, 0)
                mn.id=new_id
                nodes.append(mn)
                id_to_node[new_id]=mn
                kdir.children.append(mn)
                print(f"Created {meta_name} -> {dest_name}")

    out = output_fs or fs_path
    build_fs_from_nodes(nodes, id_to_node, out)
    print(f"Done. FS written to {out}")

def main():
    ap = argparse.ArgumentParser(description="PhotonOS kernel installer prototype")
    ap.add_argument("--fs", help="path to fs.vd")
    ap.add_argument("--image", help="path to os-image.bin (will extract fs.vd part and reinsert)")
    ap.add_argument("--kernel", help="path to new kernel binary (init.bin)")
    ap.add_argument("--name", help="dest name in /sys/kernel/ (e.g., kernel-1.1.0.bin)")
    ap.add_argument("--set-default", action="store_true", help="update boot.cfg/default/active")
    ap.add_argument("--list", action="store_true", help="list kernels in FS")
    ap.add_argument("--output", help="output fs.vd path (default overwrites input)")
    args = ap.parse_args()

    if args.list:
        if not args.fs:
            sys.exit("--list requires --fs")
        list_kernels(args.fs)
        return

    if not args.kernel or not args.name:
        ap.print_help()
        sys.exit(1)

    fs_path = args.fs
    tmp_fs = None
    if args.image:
        # extract fs.vd from os-image.bin
        # FS starts at sector 384 (from Makefile default), but we should parse MBR header
        with open(args.image, "rb") as f:
            img = f.read()
        # read PHTB header at 0x40
        magic = struct.unpack_from("<I", img, 0x40)[0]
        if magic != 0x42544850:
            sys.exit("Not a PhotonOS image (no PHTB header)")
        fs_lba = struct.unpack_from("<I", img, 0x40+0x10)[0]
        print(f"Image FS LBA = {fs_lba}")
        fs_off = fs_lba*512
        fs_data = img[fs_off:]
        # trim trailing zeros? Keep up to actual fs.vd size (we need to detect)
        # For simplicity, write fs_data to temp file and let parse handle it (it will read total_nodes)
        tmp_fs = "/tmp/photon_fs.vd"
        with open(tmp_fs, "wb") as out:
            out.write(fs_data)
        fs_path = tmp_fs

    if not fs_path:
        sys.exit("Need --fs or --image")

    output_fs = args.output or (fs_path if not args.image else tmp_fs)

    install_kernel(fs_path, args.kernel, args.name, args.set_default, output_fs)

    if args.image and tmp_fs:
        # reinsert fs.vd back into image
        with open(tmp_fs, "rb") as f:
            new_fs = f.read()
        with open(args.image, "rb") as f:
            img = bytearray(f.read())
        fs_lba = struct.unpack_from("<I", img, 0x40+0x10)[0]
        fs_off = fs_lba*512
        # ensure new fs fits
        if len(new_fs) > len(img) - fs_off:
            sys.exit(f"New FS too large: {len(new_fs)} > {len(img)-fs_off}")
        img[fs_off:fs_off+len(new_fs)] = new_fs
        # zero out rest of FS area? Keep as is
        with open(args.image, "wb") as f:
            f.write(img)
        print(f"Updated image {args.image} with new FS")
        os.remove(tmp_fs)

if __name__ == "__main__":
    main()
