import sys
import os
import struct
from datetime import datetime

# Константы из вашего Си-кода
FS_START_SECTOR = 0
SECTOR_SIZE = 512
MAX_DISK_NODES = 100
TYPE_DIR = 1
TYPE_FILE = 0
MAGIC = 0xDA751AF5

# Форматы упакованных структур для модуля struct (маленький эндиан '<')
# disk_superblock_t: magic (I), total_nodes (I), data_block_start (I)
SUPERBLOCK_FMT = "<III"

# disk_dnode_t: type (B), name (32s), size (I), change_time (I), permissions (I), parent_id (i), data_block (I), reserved (11s)
DNODE_FMT = "<B32sIIIiI11s"


def get_custom_timestamp():
    """Возвращает количество секунд с 1 января 2000 года."""
    epoch = datetime(2000, 1, 1)
    now = datetime.now()
    return int((now - epoch).total_seconds())


def parse_permissions(perm_str):
    """Конвертирует битовую строку вида '00001111' в число."""
    try:
        return int(perm_str.strip(), 2)
    except ValueError:
        return 0o755  # Дефолтные права, если парсинг не удался


class PyDnode:
    """Внутреннее представление узла ФС для сборщика."""

    def __init__(self, name, node_type):
        self.type = node_type
        self.name = name[:31]  # Ограничение длины имени (31 символ + \0)
        self.size = 0
        self.change_time = get_custom_timestamp()
        self.permissions = 0o755
        self.data = b""
        self.children = []


def parse_vd_txt(dir_path):
    """Парсит файл __vd.txt в указанной директории."""
    metadata = {}
    vd_file_path = os.path.join(dir_path, "__vd.txt")

    if not os.path.exists(vd_file_path):
        return metadata

    with open(vd_file_path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue

            parts = line.split(",")
            if len(parts) >= 4:
                filename = parts[0].strip()
                perm = parse_permissions(parts[1])
                owner = parts[2].strip()  # В Си-структуре нет owner, пропускаем
                time_str = parts[3].strip()

                if time_str == "present":
                    timestamp = get_custom_timestamp()
                else:
                    try:
                        timestamp = int(time_str)
                    except ValueError:
                        timestamp = get_custom_timestamp()

                metadata[filename] = {"permissions": perm, "time": timestamp}
    return metadata


def build_tree(root_host_path):
    """Рекурсивно строит дерево узлов из локальной папки."""

    def walk(current_host_path, node_name, is_root=False):
        if os.path.isdir(current_host_path):
            node = PyDnode(node_name, TYPE_DIR)
            metadata = parse_vd_txt(current_host_path)

            # Применяем метаданные для самой папки
            if "." in metadata:
                node.permissions = metadata["."]["permissions"]
                node.change_time = metadata["."]["time"]

            # Обходим содержимое папки
            for item in sorted(os.listdir(current_host_path)):
                if item == "__vd.txt":
                    continue

                item_host_path = os.path.join(current_host_path, item)
                child_node = walk(item_host_path, item)

                # Применяем индивидуальные метаданные из __vd.txt, если они есть
                if item in metadata:
                    child_node.permissions = metadata[item]["permissions"]
                    child_node.change_time = metadata[item]["time"]

                node.children.append(child_node)
            return node
        else:
            node = PyDnode(node_name, TYPE_FILE)
            with open(current_host_path, "rb") as f:
                node.data = f.read()
            node.size = len(node.data)
            return node

    return walk(root_host_path, "", is_root=True)


def serialize_tree(root_node):
    """Линеаризует дерево в список disk_nodes (Pre-order DFS) точно так же, как в Си."""
    disk_nodes = []
    data_blocks = []

    current_data_sector = FS_START_SECTOR + 15

    def dfs(node, parent_id):
        nonlocal current_data_sector
        if len(disk_nodes) >= MAX_DISK_NODES:
            print(f"[Важное предупреждение]: Превышен лимит узлов ({MAX_DISK_NODES})!")
            return

        my_id = len(disk_nodes)

        # Подготовка данных структуры disk_dnode_t
        name_bytes = node.name.encode("utf-8")[:31].ljust(32, b"\0")
        allocated_sector = 0

        if node.type == TYPE_FILE and node.size > 0:
            allocated_sector = current_data_sector
            sectors_to_write = (node.size + 511) // 512
            # Выравниваем данные файла до размера секторов
            padded_data = node.data.ljust(sectors_to_write * 512, b"\0")
            data_blocks.append((current_data_sector, padded_data))
            current_data_sector += sectors_to_write

        # Создаем упакованную структуру
        # dnode_fmt = "<B32sIIIiI11s"
        packed_node = struct.pack(
            DNODE_FMT,
            node.type,
            name_bytes,
            node.size,
            node.change_time,
            node.permissions,
            parent_id,
            allocated_sector,
            b"\0" * 11,  # Reserved
        )
        disk_nodes.append(packed_node)

        # Рекурсивно обходим детей
        for child in node.children:
            dfs(child, my_id)

    dfs(root_node, -1)
    return disk_nodes, data_blocks


def create_vd_image(source_dir, output_vd_path):
    """Основная функция конвертации папки в файл .vd"""
    print(f"Сканирование директории: {source_dir}...")
    root_node = build_tree(source_dir)

    print("Сериализация дерева файловой системы...")
    disk_nodes, data_blocks = serialize_tree(root_node)

    # Инициализируем пустой образ диска (массив байт)
    # Размер подгоняем под финальный сектор данных
    max_sector = FS_START_SECTOR + 15
    if data_blocks:
        last_block_sector, last_block_data = data_blocks[-1]
        max_sector = last_block_sector + (len(last_block_data) // 512)

    image_size = max_sector * SECTOR_SIZE
    disk_image = bytearray(image_size)

    # 1. Запись суперблока (Сектор 384)
    sb_bytes = struct.pack(SUPERBLOCK_FMT, MAGIC, len(disk_nodes), FS_START_SECTOR + 15)
    sb_sector_offset = FS_START_SECTOR * SECTOR_SIZE
    disk_image[sb_sector_offset : sb_sector_offset + len(sb_bytes)] = sb_bytes

    # 2. Запись таблицы узлов (Начиная с сектора 385)
    nodes_offset = (FS_START_SECTOR + 1) * SECTOR_SIZE
    for i, packed_node in enumerate(disk_nodes):
        offset = nodes_offset + (i * 64)
        disk_image[offset : offset + 64] = packed_node

    # 3. Запись блоков данных файлов
    for sector, data in data_blocks:
        offset = sector * SECTOR_SIZE
        disk_image[offset : offset + len(data)] = data

    # Сохраняем результат в файл
    with open(output_vd_path, "wb") as f:
        f.write(disk_image)

    print(f"Успешно создано! Образ сохранен в: {output_vd_path}")
    print(f"Всего упаковано узлов: {len(disk_nodes)} из {MAX_DISK_NODES}")


if __name__ == "__main__":
    # Пример использования:
    # Замените 'my_os_root' на путь к вашей локальной папке, которую нужно упаковать
    HOST_DIRECTORY = sys.argv[1]
    OUTPUT_IMAGE = sys.argv[2]

    # Создадим демонстрационную тестовую папку, если её нет
    if not os.path.exists(HOST_DIRECTORY):
        os.makedirs(os.path.join(HOST_DIRECTORY, "bin"))
        # Создаем тестовый __vd.txt
        with open(
            os.path.join(HOST_DIRECTORY, "__vd.txt"), "w", encoding="utf-8"
        ) as f:
            f.write(".,0000000011111111,kernel,present\n")
            f.write("bin,0000000011110101,kernel,present\n")

        with open(
            os.path.join(HOST_DIRECTORY, "bin", "init.txt"), "w", encoding="utf-8"
        ) as f:
            f.write("Hello from Virtual FS script!")

        with open(
            os.path.join(HOST_DIRECTORY, "bin", "__vd.txt"), "w", encoding="utf-8"
        ) as f:
            f.write("init.txt,0000000011001100,kernel,present\n")

    # Сборка образа
    create_vd_image(HOST_DIRECTORY, OUTPUT_IMAGE)
